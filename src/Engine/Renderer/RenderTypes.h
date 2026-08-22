#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

#include <DirectXMath.h>
#include <d3d11.h>

#include "Engine/Core/EntityID.h"

namespace mye {

// ---- sRGB → リニア変換 (M38a リニアパイプライン) ----
// authored な色 (マテリアル baseColor / ライト色 / スカイ / フォグ) は sRGB 認知色として
// 保存されている前提で、CB へ載せる直前に変換する。トーンマップ後の OETF (applyGamma)
// と対になり「作者が選んだ色 ≒ 画面の色」を保つ。render-only (sim/hash 非関与)。
inline float SrgbToLinear(float c)
{
    return (c <= 0.04045f) ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
inline DirectX::XMFLOAT3 SrgbToLinear(const DirectX::XMFLOAT3& c)
{
    return { SrgbToLinear(c.x), SrgbToLinear(c.y), SrgbToLinear(c.z) };
}
inline DirectX::XMFLOAT4 SrgbToLinear(const DirectX::XMFLOAT4& c)
{
    return { SrgbToLinear(c.x), SrgbToLinear(c.y), SrgbToLinear(c.z), c.w }; // α はそのまま
}

// 「収集 → ソート → 提出」モデル (engine_spec.md 6.3)。
// 即時描画 API は提供しない — 将来のマルチスレッド化 / API 差し替えの余地を残す。

// ボーンパレット最大数 (M18、M45 で 64 → 128)。定数バッファは 128*64B = 8KB で D3D11 の
// 64KB 上限に十分収まる。Mixamo の標準ヒューマノイドが約 65 ジョイントで 64 を超えるため拡張。
// **HLSL 側の MYE_MAX_BONES (forward_skinned.hlsl / deferred_gbuffer_skinned.hlsl) と必ず一致
// させること** — 食い違うと定数バッファのサイズ不一致で描画が壊れる。
// tools\check_rules.ps1 の規則 9 が C++/HLSL 3 箇所の一致を静的に検査する。
constexpr int kMaxBones = 128;

// 自己発光強度を G-Buffer へ詰めるときの正規化上限 (M46i)。
// gbMaterial は R8G8B8A8_UNORM で b チャンネルが空いていたので、そこへ
// saturate(emissiveIntensity / kEmissiveMaxIntensity) を書き、ライトパスで逆変換する。
// **HLSL 側の MYE_EMISSIVE_MAX (common.hlsli) と必ず一致させること** — 食い違うと
// 発光の明るさが静かに定数倍ずれる。tools\check_rules.ps1 の規則 9 が一致を検査する。
// 8 は「1 = 白の拡散面と同じ明るさ」を基準に、屋内の面光源が飽和しない範囲として選んだ値
constexpr int kEmissiveMaxIntensity = 8;

// common.hlsli の EncodeEmissive / DecodeEmissive の CPU ミラー (PostFxMath.h と同じ方針)。
// selftest がこの 2 本で往復と飽和を検証し、HLSL 側との式の一致は目視 + 規則 9 で担保する。
// **0 はちょうど 0 に落ちる** — これが「発光を使わないマテリアルは M46i 以前と
// ビット単位で同じ絵になる」という受け入れ基準の根拠
inline float EncodeEmissive(float intensity)
{
    const float t = intensity / static_cast<float>(kEmissiveMaxIntensity);
    return (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t); // HLSL saturate と同じ
}

inline float DecodeEmissive(float encoded)
{
    return encoded * static_cast<float>(kEmissiveMaxIntensity);
}

struct RenderItem {
    AssetID mesh = {};
    AssetID material = {};
    DirectX::XMFLOAT4X4 world = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    float viewZ = 0.0f; // ソート用 (カメラ空間深度)。RenderQueue::Sort が使用
    // スキニング (M18)。非 null = スキンメッシュ → パスがスキニングシェーダ + ボーン CB を使う。
    // 指す先は RenderSystem のフレームアリーナ (transpose 済みボーン行列、描画完了まで有効)。
    const DirectX::XMFLOAT4X4* bones = nullptr;
    int32_t boneCount = 0;
    // ---- M55c: 前フレームに **実際に描いた** ワールド行列 (末尾 append、velocity 用) ----
    // 「前 tick」ではない (詳細は RenderSystem.h の PrevRenderWorldStore の頭)。
    // 履歴が無い場合は world と同値が入る = 画面速度が厳密に 0 になり、
    // 消費側は「カメラ再投影のみ」へ自然に縮退する
    DirectX::XMFLOAT4X4 prevWorld = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
};

// ---- 局所ライトのシャドウアトラス (M54c) ----
// 4096^2 の深度テクスチャ 1 枚を正方タイルに割り、スポットは 1 枚 (透視 1 面)、
// 点光源は 6 枚 (M54d) を使う。タイル数は 4096^2 / 1024^2 = 16。
// **HLSL の MYE_MAX_SHADOW_TILES (common.hlsli) と必ず一致させること** —
// 定数バッファの配列長そのものなので、食い違うとレイアウト不一致として静かに壊れる。
// tools\check_rules.ps1 の規則 9 が一致を検査する。
// ★per-light パラメータを StructuredBuffer ではなく CB で渡しているのは、統合契約の
//   予約 2 が M54 に許した SRV スロットが t12 (アトラス本体) の 1 本きりだから
//   (計画本文の「StructuredBuffer で t7/t13」は予約表と食い違っており、予約表が正)。
//   16 枚 × 96 バイト = 1.5KB で、64KB の CB 上限には遠く届かない
constexpr int kMaxShadowTiles = 16;

// アトラスのタイル 1 枚。ShadowAtlas が描画に、光パスが CB 充填に使う (描画専用)。
// lightViewProj は**非転置** — ShadowAtlas が world と合成するため。CB へ載せる側が転置する
struct ShadowTile {
    DirectX::XMFLOAT4X4 lightViewProj = {}; // 行ベクトル規約 (world * lightViewProj)
    float uvScale[2] = { 0.0f, 0.0f };      // タイル UV [0,1] → アトラス UV の拡大率
    float uvOffset[2] = { 0.0f, 0.0f };
    int32_t pixelX = 0;     // アトラス内のピクセル矩形 (RSSetViewports 用)
    int32_t pixelY = 0;
    int32_t pixelSize = 0;
    float depthBias = 0.0f; // シェーダ側の定数バイアス (NDC 深度。ラスタライザ側と併用)
};

// 定数バッファへ載せる形のタイル (HLSL common.hlsli の ShadowTile と同一 96 バイト、M54c)。
// 上の ShadowTile (描画側の生データ) を転置 + 詰め替えたもの。
// ★M54e で Deferred 光パス / Forward / Deferred 透明後段の **3 箇所**が同じ変換を要求する
//   ようになったのでここへ引き上げた。転置を 1 箇所でも書き忘れると
//   「その経路だけ影が明後日の方向に出る」という、絵は出るのに合わないだけの壊れ方をする
struct ShadowTileCB {
    DirectX::XMFLOAT4X4 lightViewProj = {}; // transpose(lightView*lightProj)
    DirectX::XMFLOAT4 uvScaleBias = {};     // xy = スケール / zw = オフセット
    DirectX::XMFLOAT4 params = {};          // x = 定数深度バイアス (NDC) / yzw = 予約
};
static_assert(sizeof(ShadowTileCB) == 96, "ShadowTileCB must match HLSL 16-byte packing");

struct RenderView {
    DirectX::XMFLOAT4X4 view = {};
    DirectX::XMFLOAT4X4 proj = {};
    DirectX::XMFLOAT3 cameraPos = { 0, 0, 0 };
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    int width = 0;
    int height = 0;
    float clearColor[4] = { 0.08f, 0.09f, 0.11f, 1.0f };
    // ---- シャドウ (M17 単一 → M38d CSM)。RenderSystem がシャドウパス後に埋める。描画専用 ----
    DirectX::XMFLOAT4X4 lightViewProj[3] = {}; // 各カスケードの transpose(lightView*lightProj)
    float cascadeSplits[3] = { 0, 0, 0 };      // 各カスケードの far 境界 (view 深度、デバッグ用)
    int32_t cascadeCount = 0;                  // 0 = 影無効
    ID3D11ShaderResourceView* shadowSRV = nullptr; // シャドウ深度 Texture2DArray (R32_FLOAT)
    float shadowTexelSize = 0.0f;                  // 1/解像度 (PCF オフセット)
    // ---- 環境 (M29d)。RenderSystem が最初の active Skybox/Fog から埋める。描画専用 ----
    int32_t skyMode = -1; // -1=無効 (clearColor 背景) / 0=グラデーション / 1=cubemap (M38b)
    AssetID skyCubemapId = {};                          // CollectEnvironment が埋める (純データ)
    ID3D11ShaderResourceView* skyCubemap = nullptr;     // RenderSystem が解決 (null=フォールバック)
    DirectX::XMFLOAT3 skyTop = { 0.24f, 0.42f, 0.83f };
    DirectX::XMFLOAT3 skyHorizon = { 0.74f, 0.81f, 0.90f };
    DirectX::XMFLOAT3 skyBottom = { 0.28f, 0.25f, 0.22f };
    int32_t fogMode = -1; // -1=フォグ無効 / 0=linear 1=exp 2=exp2
    DirectX::XMFLOAT3 fogColor = { 0.65f, 0.70f, 0.75f };
    float fogDensity = 0.02f;
    float fogStart = 10.0f;
    float fogEnd = 80.0f;
    // ---- IBL (M38c)。RenderSystem が EnvMapBaker から埋める。null = 定数アンビエント ----
    ID3D11ShaderResourceView* iblIrradiance = nullptr;
    ID3D11ShaderResourceView* iblPrefiltered = nullptr;
    ID3D11ShaderResourceView* iblBrdfLut = nullptr;
    float iblSpecMips = 0.0f;
    // ---- SSAO (M38e)。Deferred のみ消費 (Forward は無視) ----
    int32_t ssaoEnabled = 0;
    float ssaoRadius = 0.8f;    // M40d: CameraPostFx から (シーンカメラ経路のみ上書き)
    float ssaoIntensity = 1.0f;
    // ---- メッシュ GPU インスタンシング (M38f)。0 = 全て per-item 描画 (A/B 比較用) ----
    int32_t instancingEnabled = 1;
    // ---- SceneView 表示モード (M40b)。0=Lit 1=Unlit (白ライト) 2=Wireframe (+Unlit)。
    //      CameraOverride 経由でエディタのみ設定 — GameView/Runtime は常に 0 ----
    int32_t debugViewMode = 0;
    // ---- M42a: シーン深度 SRV 基盤 (末尾 append)。FrameTarget からパススルー。
    //      depthSRV は dsv と同一テクスチャ — 読む側は dsvReadOnly バインド中のみ合法。
    //      null = 深度読み系効果 (ソフトパーティクル等) を自然無効化 (AssetPreview) ----
    ID3D11ShaderResourceView* depthSRV = nullptr;
    ID3D11DepthStencilView* dsvReadOnly = nullptr;
    float nearZ = 0.1f;   // 深度線形化用 (カメラ/override から充填)
    float farZ = 1000.0f;
    // ---- M42d: 歪みバッファ (PostProcess::Target::distort)。RenderSystem が
    //      「blendMode=2 エミッタあり && HDR 経路」のときだけクリアして充填。
    //      null = 歪みパーティクルは描かれない (postfx off / AssetPreview) ----
    ID3D11RenderTargetView* distortionRTV = nullptr;
    // ---- M43a: ハイトフォグ + 太陽インスキャッタ (末尾 append。既定 = 恒等 = 従来と同一)。
    //      fog 系は CollectEnvironment のパススルー、太陽は RenderSystem が
    //      最初の type==0 平行光から充填 (リニア・強度込み。無ければ intensity を 0 に潰す) ----
    float fogHeightFalloff = 0.0f;      // 0 = 高さ一様 (従来)
    float fogBaseHeight = 0.0f;
    float fogInscatterIntensity = 0.0f; // 0 = 無効
    float fogInscatterPower = 8.0f;
    DirectX::XMFLOAT3 sunDirection = { 0.0f, -1.0f, 0.0f }; // 光の進行方向 (正規化)
    DirectX::XMFLOAT3 sunColor = { 0.0f, 0.0f, 0.0f };      // リニア・強度込み
    // ---- M44d: カメラモーションブラー (末尾 append)。RenderSystem が viewKey 毎の
    //      前フレーム viewProj を供給。valid=0 = 初フレーム/リサイズ = ブラー 0 ----
    DirectX::XMFLOAT4X4 prevViewProj = {}; // 未転置 (view*proj)
    int32_t prevViewProjValid = 0;
    // ---- M46b: ハイブリッド・パストレーシング (末尾 append。既定 = 0/null = 従来と同一)。
    //      rtScene/rtPasses が null のパス (Forward / AssetPreview) では自然に無効化される ----
    int32_t rtDebugMode = 0; // 0=off 1=BVH ヒートマップ 2=ヒット法線 3=インスタンス ID 4=生 GI
    const struct RtSceneBindings* rtScene = nullptr;
    class RtPasses* rtPasses = nullptr;
    // ---- M46c: 拡散 GI ----
    float rtResolutionScale = 0.5f; // GI を撃つ内部解像度の倍率 (0.25〜1.0)
    int32_t rtBounces = 1;          // 二次光線のバウンス数
    uint32_t rtFrameIndex = 0;      // 乱数列をフレームでずらす (freeze 時は 0 固定)
    // ---- M46d: テンポラル蓄積 (末尾 append)。RenderSystem が充填 ----
    int32_t rtTemporal = 1;    // 0 = 蓄積せず 1spp のまま (A/B 比較用)
    uint32_t rtViewKey = 0;    // 履歴の格納先 (FrameTarget::viewKey と同値)
    uint32_t rtViewSerial = 0; // このビューが描かれた通番。+1 で連続 = 履歴が使える
    // 前フレームのカメラ位置 (再投影の深度照合。prevViewProj とセットで有効)
    DirectX::XMFLOAT3 prevCameraPos = { 0, 0, 0 };
    // ---- M46e: SVGF 空間フィルタ (末尾 append)。テンポラル蓄積が前提 (幾何バッファの出所) ----
    int32_t rtSvgf = 1;       // 0 = 分散推定 + A-Trous を掛けない (A/B 比較用)
    int32_t rtFreezeSeed = 0; // 1 = 乱数固定 → 分散推定はテンポラルでなく空間へ落とす
    // ---- M46f: 最終画像への合成 (末尾 append)。1 = ライトパスの拡散環境項を GI で置換 ----
    int32_t rtGiEnabled = 0;
    // ---- M46g: RT 影 (末尾 append)。1 = 平行光のシャドウ係数を CSM でなくレイトレで作る ----
    int32_t rtShadowEnabled = 0;
    // ---- M46h: RT 反射 (末尾 append)。1 = ライトパスのスペキュラ環境項を
    //      roughness に応じてレイトレ反射で置換する (粗い面は IBL のまま) ----
    int32_t rtReflEnabled = 0;
    // ---- M54c: 局所ライト (スポット/点) のシャドウアトラス (末尾 append)。
    //      null / 0 = 従来と完全に同一の絵。RenderSystem がアトラス描画後に埋める。
    //      アトラスを持たない経路 (AssetPreview は永久に = enableShadows=false) は
    //      SRV が null のままなので、光パス側の「null ならフラグ 0」ゲートで自然に無効化される ----
    ID3D11ShaderResourceView* shadowAtlasSRV = nullptr; // Texture2D (R32_FLOAT)
    float shadowAtlasTexel = 0.0f;                      // 1/アトラス解像度 (PCF オフセット)
    int32_t shadowTileCount = 0;                        // 0 = 影を投げる局所ライトが居ない
    ShadowTile shadowTiles[kMaxShadowTiles] = {};
    // ---- M55b: カメラジッタ (末尾 append。既定 = 振幅 0 = proj と 1 ビットも変わらない) ----
    //   proj         = ラスタライズに使う射影 (ジッタ込み)。
    //   projNoJitter = ジッタを載せる前の射影。**再投影 (prevViewProj / モーションブラー /
    //     RT テンポラル)・シャドウのカスケードフィット・視錐台カリング・太陽の画面位置は
    //     必ずこちらを読む** (詳細は PostFxMath.h の camerajitter の頭)。
    //   RenderSystem::Render が両方を必ず埋める。手組みの RenderView (selftest) では
    //     projNoJitter が単位行列のままになるので、そちらを読む経路は通らないこと。
    DirectX::XMFLOAT4X4 projNoJitter = {};
    float jitterPixels[2] = { 0.0f, 0.0f }; // proj に載せたサブピクセル量 (0,0 = ジッタ無効)
    float jitterNdc[2] = { 0.0f, 0.0f };    // 同じものを NDC で (TAA が履歴サンプルに使う)
    uint32_t viewFrameIndex = 0;            // viewKey 別の描画通番 = ジッタ列のインデックス
    // ---- M55c: velocity バッファの可視化 (末尾 append。0 = 何も起きない) ----
    // Deferred のみ。GBuffer RT4 を画面へ貼り替える純デバッグ表示で、消費側は誰もいない
    int32_t velocityDebug = 0;
    // ---- M55d: TAA (末尾 append。既定 0/null = 従来と 1 ビットも変わらない) ----
    //   taaEnabled  = このビューで TAA を走らせる (= カメラジッタも載っている)。
    //     RenderSystem が「CameraPostFx の taaOn / グローバル設定」と
    //     「パスが velocity を書くか (Deferred のみ)」の両方を見て決める。
    //     ★ジッタと TAA は**必ず同じ条件**で on/off する — 片方だけだと画面が
    //       毎フレーム半ピクセル揺れるだけになる。
    //   velocitySRV = GBuffer RT4。path.Render の直後に RenderSystem が
    //     IRenderPath::VelocitySRV() から充填する (Forward は null)。
    //   viewKey     = 履歴スロット (FrameTarget::viewKey と同値。0=AssetPreview は履歴なし)。
    //     履歴の連続性判定は既存の viewFrameIndex (viewKey 毎の描画通番) を使う
    int32_t taaEnabled = 0;
    ID3D11ShaderResourceView* velocitySRV = nullptr;
    uint32_t viewKey = 0;
    // ---- M58c: 地形の可視チャンク (末尾 append)。RenderSystem が TerrainSystem の
    //      収集結果を指す。実体は TerrainPass.h の TerrainDrawList (Renderer 層の純データ)。
    //      **null / 空 = 地形なし = 従来と完全に同じ絵** — AssetPreviewCache の
    //      RenderSystem はここを埋めないので、サムネイルは地形を一切描かない ----
    const struct TerrainDrawList* terrain = nullptr;
};

// M55c: 「**前フレームに実際に描いた** world 行列」の viewKey 別ストア (velocity の出所)。
//
// ★★RenderSystem.h の PrevWorldStore (M36b) では代用できない。あちらは **tick 頭**の
//   スナップショットで、実際に描かれるのは LerpWorld(prev, cur, interpAlpha)。つまり
//   前フレームの画面にあった行列は「LerpWorld(prev, cur, 前フレームの alpha)」であって
//   prev ではない。prev をそのまま velocity に使うと最大 1 tick 分過大になり、
//   TAA が履歴を外しモーションブラーが過剰にブレる。
//   ★さらに悪いことに **決定的撮影モードでは dt 固定で interpAlpha == 1.0 になる**ので、
//   この誤りは golden に 1 ピクセルも現れない (対話プレイでだけ出る)。だから機械検査は
//   スクショではなく selftest (RenderSelfTest の TestPrevRenderWorldStore) 側にある。
//
// 構造は RtPasses::RtHistory (viewKey 別 + 描画通番の連続性判定 + リサイズ破棄) に倣う。
// 「前フレームも描かれたか」はスロット毎の通番で見るので、消えた/カリングされた
// エンティティの古い行列を拾うことがない (毎フレームのクリアも要らない)。
struct PrevRenderWorldStore {
    std::vector<DirectX::XMFLOAT4X4> world; // entity.index キー
    std::vector<uint32_t> generation;       // entity.generation + 1 (0 = 未使用スロット)
    std::vector<uint32_t> slotSerial;       // そのスロットを書いたときの描画通番

    // 今フレームの描画通番とビューサイズを宣言する。
    // 戻り値 = 前フレームの記録が使えるか (通番がちょうど 1 つ違い かつ 同サイズ)。
    // 使えないときも記録は続ける (次フレームのため)
    bool Begin(uint32_t frameSerial, int width, int height)
    {
        usable_ = valid_ && w_ == width && h_ == height && lastSerial_ + 1u == frameSerial;
        cur_ = frameSerial;
        prev_ = frameSerial - 1u; // frameSerial==0 は 0xFFFFFFFF へ回る = どのスロットとも不一致
        lastSerial_ = frameSerial;
        w_ = width;
        h_ = height;
        valid_ = true;
        return usable_;
    }

    // 前フレームに描いた行列 (無ければ null)。同じエンティティの Record より **先**に呼ぶこと
    const DirectX::XMFLOAT4X4* Lookup(EntityID e) const
    {
        if (!usable_ || e.index >= world.size()) {
            return nullptr;
        }
        if (generation[e.index] != e.generation + 1u || slotSerial[e.index] != prev_) {
            return nullptr; // 前フレームは描かれていない / index が再利用された
        }
        return &world[e.index];
    }

    void Record(EntityID e, const DirectX::XMFLOAT4X4& m)
    {
        if (e.index >= world.size()) {
            world.resize(e.index + 1);
            generation.resize(e.index + 1, 0);
            slotSerial.resize(e.index + 1, 0);
        }
        world[e.index] = m;
        generation[e.index] = e.generation + 1u;
        slotSerial[e.index] = cur_;
    }

private:
    uint32_t cur_ = 0;
    uint32_t prev_ = 0;
    uint32_t lastSerial_ = 0;
    int w_ = 0;
    int h_ = 0;
    bool valid_ = false;
    bool usable_ = false;
};

// view のタイル列を CB 形式へ詰め替える (M54e)。dst は kMaxShadowTiles 要素を要求する。
// 呼び出し側が「アトラスを使うか」を判定した後で呼ぶ — ここは判定しない
// (使わない経路では dst をゼロのまま渡せばよく、シェーダ側のフラグが 0 なら読まれない)。
inline void FillShadowTilesCB(const RenderView& view, ShadowTileCB* dst)
{
    for (int t = 0; t < view.shadowTileCount && t < kMaxShadowTiles; ++t) {
        const ShadowTile& src = view.shadowTiles[t];
        DirectX::XMStoreFloat4x4(
            &dst[t].lightViewProj,
            DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&src.lightViewProj)));
        dst[t].uvScaleBias = { src.uvScale[0], src.uvScale[1], src.uvOffset[0], src.uvOffset[1] };
        dst[t].params = { src.depthBias, 0.0f, 0.0f, 0.0f };
    }
}

// GPU へ渡すライト 1 個 (定数バッファ配列要素、16 バイト境界に揃えた 64 バイト)。
// HLSL 側 common.hlsli の Light 構造体とレイアウト一致。
struct GpuLight {
    DirectX::XMFLOAT3 position = { 0, 0, 0 };    // Point/Spot: ワールド位置
    float range = 15.0f;                         // Point/Spot: 減衰半径
    DirectX::XMFLOAT3 direction = { 0, -1, 0 };  // 光の進行方向 (正規化、Dir/Spot)
    float intensity = 1.0f;
    DirectX::XMFLOAT3 color = { 1, 1, 1 };
    int32_t type = 0;      // 0=Directional 1=Point 2=Spot
    float cosInner = 0.9f; // Spot: cos(内角)
    float cosOuter = 0.8f; // Spot: cos(外角)
    // ---- M54c: シャドウアトラス (旧 pad0/pad1 の再利用。64 バイトのレイアウトは不変) ----
    // 統合契約 (plans/radiant-shimmering-lumen.md 付録 予約 2) が pad0/pad1 に予約した枠。
    // rt_common.hlsli の RtLight は同じ 8 バイトを _pad のまま持つ (RT は局所影を持たない)
    int32_t shadowTile = 0;  // アトラスのタイル index (先頭面)。shadowFaces==0 なら無意味
    int32_t shadowFaces = 0; // 面数: 0=影を投げない / 1=スポット (M54c) / 6=点光源 (M54d)
};
static_assert(sizeof(GpuLight) == 64, "GpuLight must match HLSL 16-byte packing");

// ライト配列長。**HLSL の MAX_LIGHTS (common.hlsli) / MYE_RT_MAX_LIGHTS (rt_common.hlsli) と
// 必ず一致させること** — 食い違うと定数バッファのレイアウト不一致として静かに壊れる。
// tools\check_rules.ps1 の規則 9 が静的に検査する (M55a で登録)
constexpr int kMaxLights = 16;

// シーンのライト一式 (アンビエント + ライト配列)。RenderSystem が構築し各パスへ渡す。
struct SceneLightData {
    DirectX::XMFLOAT3 ambient = { 0.15f, 0.16f, 0.18f };
    int32_t count = 0;
    GpuLight lights[kMaxLights] = {};
};

class RenderQueue {
public:
    std::vector<RenderItem> opaque;
    std::vector<RenderItem> transparent;

    void Clear()
    {
        opaque.clear();
        transparent.clear();
    }

    // 決定論的ソート (spec 11.2 規則 7: 明示キー + タイブレーク。ポインタ比較は禁止)。
    // opaque: material → mesh → 深度 (近い順) / transparent: 深度 (遠い順) → material → mesh
    void Sort();
};

// ---- M57a: フロクセル (視錐台に沿った 3D グリッド) の幾何 ----
//
// 視錐台を XY は画面タイル、Z は指数分布のスライスに割った 3D テクスチャへ散乱と消散を
// 積み、最後に手前から積分して「そのピクセルまでの inscatter / transmittance」を作る。
// ここに置いてあるのは**グリッドの幾何だけ** (パスの実体は M57b の FroxelPass)。
// 全部純関数なので RenderSelfTest が機械検査できる — GPU を起こさずに済む部分は
// 起こさずに検査するのがこのリポジトリの流儀 (テストは機能の隣に置く)。
namespace froxel {

// CS のスレッドグループ (XY のみ。Z はディスパッチ側でスライス数ぶん並べる)。
// XY だけをタイルにしているのは、注入も積分も「同じ (x,y) の Z 列」を扱うから —
// 積分パス (M57c) は 1 スレッドが 1 本の Z 列を手前から舐めるので Z を割れない。
// **HLSL の MYE_FROXEL_GROUP (assets\shaders\froxel_*.cs.hlsl) と必ず一致させること** —
// 食い違うとグリッドの一部が書かれないまま残り、前フレームの残骸を積分する形で
// 静かに壊れる。tools\check_rules.ps1 の規則 9 が一致を検査する
constexpr int kGroupSize = 8;

// 既定のグリッド解像度。**M57a の WARP 実測 (Editor.exe --froxel-probe) で決めた値**。
// 960x540 に対して 6x6 画素タイル x 64 スライス = 921,600 セル / 7.03MB。
//
//   グリッド        セル数    WARP clear   RTX3060 clear   1 枚の VRAM
//   160x90x64      921,600     0.95 ms       0.026 ms        7.03 MB   ← 既定
//   128x72x48      442,368     0.55 ms       0.014 ms        3.38 MB
//   80x45x32       115,200     0.27 ms       0.007 ms        0.88 MB
//
// 落とさなかった理由: WARP のスループットが 8 倍のセル数域でほぼ一定 (約 0.9 Gcell/s) =
// **コストがセル数に線形**で、固定費に食われていない。つまり解像度は後から素直に効く
// 品質/コストのつまみで、先に絞る必要が無い。注入 (M57b) が WARP で重すぎたら
// この表の下段へ落とす — 数字が線形なので予測が立つ。
// ★clear は「空の CS」= 下限であって、注入パスのコストではない
constexpr int kGridX = 160;
constexpr int kGridY = 90;
constexpr int kGridZ = 64;

// ディスパッチのグループ数 (切り上げ)。extent <= 0 でも 0 を返して Dispatch を空振りさせる
constexpr int DispatchGroups(int extent, int group)
{
    return (extent <= 0 || group <= 0) ? 0 : (extent + group - 1) / group;
}

// スライス境界の view 深度 (指数分布)。slice = 0 → nearZ、slice = sliceCount → farZ。
// 手前を厚く割るのは、フォグの見た目の情報量がカメラ近傍に集中しているから
// (等間隔だと近景が 1 スライスに潰れて縞が出る)。
// nearZ は正でなければならない — 0 だと log が発散するので下限で潰す
inline float SliceToViewDepth(float slice, int sliceCount, float nearZ, float farZ)
{
    const float n = (nearZ > 1e-4f) ? nearZ : 1e-4f;
    const float f = (farZ > n) ? farZ : (n * 2.0f);
    const int count = (sliceCount > 0) ? sliceCount : 1;
    return n * std::pow(f / n, slice / static_cast<float>(count));
}

// 上の逆関数。view 深度 → スライス座標 (小数)。範囲外もそのまま外挿して返す
// (クランプは呼び出し側の責任 — グリッド外を最遠スライスへ丸めると空が濁る)
inline float ViewDepthToSlice(float depth, int sliceCount, float nearZ, float farZ)
{
    const float n = (nearZ > 1e-4f) ? nearZ : 1e-4f;
    const float f = (farZ > n) ? farZ : (n * 2.0f);
    const int count = (sliceCount > 0) ? sliceCount : 1;
    const float d = (depth > 1e-6f) ? depth : 1e-6f;
    return static_cast<float>(count) * std::log(d / n) / std::log(f / n);
}

// ---- M57b: 注入パスの数式 (HLSL froxel_inject.cs.hlsl と同一式) ----
//
// GPU 側と CPU 側で式を二重に持つのは、位相関数の正規化やセル中心の取り方が
// 「絵はそれらしく出るのに物理的に間違っている」形で壊れるため。
// RenderSelfTest が CPU 版を全立体角で数値積分して 1 になることまで見ている。

// セル中心の view 深度。**境界ではなく中心**を代表点にする — 境界だと隣のセルと
// 同じ点を評価してしまい、1 スライスぶんの厚みが消える。
// M57c のジッタはこの 0.5 を [0,1) の擬似乱数で置き換える形で入る
inline float SliceCenterViewDepth(int slice, int sliceCount, float nearZ, float farZ)
{
    return SliceToViewDepth(static_cast<float>(slice) + 0.5f, sliceCount, nearZ, farZ);
}

// Henyey-Greenstein 位相関数。cosTheta = dot(光の進行方向, セル→カメラ方向)。
// g > 0 = 前方散乱 = 「光源のほうを向くと明るい」。全立体角の積分が 1 になる正規化つき
// (この 1/4π を落とすと霧の明るさが密度と一緒にしか調整できなくなる)
inline float HenyeyGreenstein(float cosTheta, float g)
{
    // ±1 は分母が 0 に落ちる特異点。CameraPostFx から ±1 が来る経路は無いが、
    // ここで inf を作るとグリッド全体が NaN で埋まる (積分結果が丸ごと消える)
    const float gg = (g < -0.95f) ? -0.95f : ((g > 0.95f) ? 0.95f : g);
    const float d = 1.0f + gg * gg - 2.0f * gg * cosTheta;
    const float dd = (d > 1e-4f) ? d : 1e-4f;
    // x^1.5 を pow で書かない — HLSL 側は WARP で pow が exp/log の 2 段になり、
    // セル × ライト本数ぶん効く。**両方を同じ形に揃えておく**のがこのミラーの意味
    return (1.0f - gg * gg) / (4.0f * 3.14159265f * (dd * std::sqrt(dd)));
}

// 高度による密度スケール。M43a のハイトフォグ ρ(y)=e^{-k(y-base)} と同じプロファイル
// (falloff == 0 なら厳密に 1 = 一様媒質)
inline float HeightDensityScale(float y, float baseHeight, float falloff)
{
    return std::exp(-falloff * (y - baseHeight));
}

} // namespace froxel

} // namespace mye
