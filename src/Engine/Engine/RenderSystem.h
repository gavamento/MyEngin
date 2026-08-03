#pragma once
#include <d3d11.h>

#include <deque>
#include <vector>

#include "Engine/Core/EntityID.h"
#include "Engine/Engine/DebugDraw.h"
#include "Engine/Engine/RayTracing/RtScene.h"
#include "Engine/Renderer/EditorLinePass.h"
#include "Engine/Renderer/EnvMapBaker.h"
#include "Engine/Renderer/PostProcess.h"
#include "Engine/Renderer/RayTracing/RtPasses.h"
#include "Engine/Renderer/RenderTypes.h"
#include "Engine/Renderer/ShadowPass.h"

namespace mye {

class World;
class GraphicsDevice;
class IRenderPath;
class ShaderManager;
class ParticleSystem;
class VfxRenderer;
struct RenderResources;

// このフレームの描画先
struct FrameTarget {
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    int width = 0;
    int height = 0;
    float clearColor[4] = { 0.08f, 0.09f, 0.11f, 1.0f };
    // ---- M42a: シーン深度 SRV 基盤 (末尾 append) ----
    // depthSRV/dsvReadOnly は dsv と同一テクスチャのビュー。null = 深度読み系効果を無効化
    // (AssetPreview 等)。viewKey はビュー別の前フレーム状態のキー (M44d モーションブラー用
    // 先行予約): 0=保存なし 1=runtime バックバッファ 2=SceneView 3=GameView
    ID3D11ShaderResourceView* depthSRV = nullptr;
    ID3D11DepthStencilView* dsvReadOnly = nullptr;
    uint32_t viewKey = 0;
};

// 描画補間 (M36b): 前 tick 末のワールド行列スナップショット。EngineLoop が tick 頭に採取し、
// Render が interpAlpha (= accumulator/fixedDt) で現在値と成分 lerp する。
// render-only — sim / WorldHash / リプレイには一切干渉しない (verify 中は alpha=1 固定)。
struct PrevWorldStore {
    std::vector<DirectX::XMFLOAT4X4> world;   // entity.index キー
    std::vector<uint32_t> generation;         // entity.generation + 1 (0 = 無効スロット)
    const DirectX::XMFLOAT4X4* Get(EntityID e) const
    {
        if (e.index >= world.size() || generation[e.index] != e.generation + 1) {
            return nullptr; // 未採取 / 破棄後の index 再利用 → 補間せず現在値
        }
        return &world[e.index];
    }
};

// エディタカメラ等でシーンカメラを上書きするためのビュー指定
struct CameraOverride {
    DirectX::XMFLOAT4X4 view = {};
    DirectX::XMFLOAT3 position = { 0, 0, 0 };
    float fovYDeg = 60.0f;
    float nearZ = 0.1f;
    float farZ = 1000.0f;
    int32_t debugViewMode = 0; // SceneView 表示モード (M40b): 0=Lit 1=Unlit 2=Wireframe
};

// 環境コンポーネント収集 (M29d): 最初 (entity.index 最小) の active な Skybox/Fog を
// view に反映する。無ければ view の既定値 (-1 = 無効) のまま。ヘッドレス selftest 対象。
void CollectEnvironment(World& world, RenderView& view);

// ECS から描画アイテムを収集し、ソートして RenderPath に提出する (spec 5.1 システム層 / 6.3)。
// カメラ: isPrimary の CameraComponent (無ければ最初のカメラ)。override 指定時はそれを優先。
// ライト: 最初の LightComponent (向き = エンティティの +Z)
class RenderSystem {
public:
    // 戻り値: カメラが見つかった (または override があった) か。
    // particles を渡すとシーン描画後に Forward 後段としてパーティクルを重ねる
    // (M6.5 の Deferred でも共通の後段 — spec 7 章 / 6.1)。
    // vfx (M29c) は Sprite/Trail/TextMesh をメッシュ後・パーティクル前に重ねる
    bool Render(World& world, GraphicsDevice& device, IRenderPath& path, ShaderManager& shaders,
                RenderResources& resources, const FrameTarget& target,
                const CameraOverride* cameraOverride = nullptr,
                ParticleSystem* particles = nullptr, VfxRenderer* vfx = nullptr);

    // ポストプロセス設定 (M16)。config / エディタから書き換え可能。全ビューポート共通。
    PostProcess::Settings postFxSettings;
    bool enablePostFx = true; // false で HDR 配管を丸ごとバイパス (従来の直描き)
    bool enableShadows = true; // false で平行光シャドウを無効 (M17)
    bool enableSsao = true;    // SSAO (M38e、Deferred パスのみ効く)
    bool enableInstancing = true; // メッシュ GPU インスタンシング (M38f、A/B 比較用トグル)

    // 描画補間 (M36b)。EngineLoop が毎フレーム設定する。1.0 = 補間なし (従来描画)。
    // 対象はカメラ + メッシュ収集のワールド行列 (パーティクル/スプライト/UI は対象外)
    float interpAlpha = 1.0f;
    const PrevWorldStore* prevWorld = nullptr;

    // スクリプトの DebugDrawLine (v7、M37)。EngineLoop が接続。非 null かつ非空で
    // シーン描画後 (ポスプロ解決前) に深度テスト付きの線として重ねる
    const std::vector<DebugLineCmd>* debugLines = nullptr;

    // M46b: レイトレのデバッグ表示 (Deferred のみ)。
    // 0=off 1=BVH ヒート 2=法線 3=インスタンス ID 4=生 GI (M46c)。
    // 0 なら BVH の構築も転送も走らない = 従来と完全に同じ経路
    int rtDebugMode = 0;
    // M46c: GI を撃つ内部解像度の倍率 / 二次光線のバウンス数 /
    // 乱数をフレームで進めない (リプレイ・スクリーンショットの決定性を保つため)
    float rtResolutionScale = 0.5f;
    int rtBounces = 1;
    bool rtFreezeSeed = false;
    // M46d: テンポラル蓄積 (1spp のノイズを前フレームの再投影で均す)。
    // false で 1spp 生のまま = A/B 比較用
    bool rtTemporal = true;
    // M46e: SVGF 空間フィルタ (分散推定 + エッジ停止 A-Trous)。
    // 幾何バッファがテンポラルパスの副産物なので rtTemporal=false では動かない
    bool rtSvgf = true;

    // M44d: ポストプロセス解決の GPU 時間 (直近の Resolve、ProfilerWindow 表示用)
    float PostFxGpuMs() const { return postFx_.ResolveGpuMs(); }
    // M46b: レイトレの統計 (ProfilerWindow 表示用)
    float RtDebugGpuMs() const { return rtPasses_.DebugGpuMs(); }
    float RtGiGpuMs() const { return rtPasses_.GiGpuMs(); }
    float RtTemporalGpuMs() const { return rtPasses_.TemporalGpuMs(); }
    float RtSvgfGpuMs() const { return rtPasses_.SvgfGpuMs(); }
    float RtBuildCpuMs() const { return rtScene_.BuildCpuMs(); }
    int RtInstanceCount() const { return rtScene_.InstanceCount(); }
    int RtTriangleCount() const { return rtScene_.TriangleCount(); }

private:
    RenderQueue queue_;     // フレーム毎に再利用 (アロケーション回避)
    PostProcess postFx_;    // HDR 中間 + トーンマップ (遅延 Init)
    ShadowPass shadowPass_; // 平行光シャドウマップ (遅延 Init)
    EditorLinePass linePass_; // DebugDrawLine 用 (v7、遅延 Init)
    EnvMapBaker envBaker_;    // IBL 環境マップ (M38c、lazy ベイク + キャッシュ)
    // スキンメッシュのボーンパレット (M18)。フレーム毎に再構築。deque = push_back で
    // 既存要素の .data() ポインタが無効化されない (RenderItem.bones が参照する)
    std::deque<std::vector<DirectX::XMFLOAT4X4>> skinPalettes_;
    // kMaxBones 超過の切り捨てを警告済みのスキンモデル AssetID (毎フレーム WARN を出さないため)
    std::vector<uint64_t> boneOverflowWarned_;
    // M44d: viewKey (1=runtime/2=SceneView/3=GameView) 毎の前フレーム viewProj。
    // 初フレーム/リサイズは valid=false → モーションブラー 0 (viewKey=0 は保存しない)
    struct PrevViewProj {
        DirectX::XMFLOAT4X4 m = {};
        DirectX::XMFLOAT3 pos = { 0, 0, 0 }; // M46d: 再投影の深度照合に使うカメラ位置
        int w = 0;
        int h = 0;
        bool valid = false;
    };
    PrevViewProj prevVP_[4];
    // M46d: viewKey 毎の描画通番 (Render 1 回で 1 進む)。テンポラル蓄積が
    // 「前フレームも同じビューを描いたか」を判定するのに使う (RT の on/off で履歴が混ざらない)
    uint32_t viewSerial_[4] = {};
    // M46b: レイトレ (遅延 Init)。rtDebugMode == 0 のあいだは一切触らない
    RtScene rtScene_;
    RtPasses rtPasses_;
    std::vector<RtScene::InstanceDesc> rtInstances_; // フレーム毎に再構築
    uint32_t rtFrameCounter_ = 0; // 乱数列をフレームでずらすための描画専用カウンタ
};

} // namespace mye
