#pragma once
#include <string>
#include <vector>

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Engine/RenderSystem.h"
#include "Engine/Renderer/EnvMapBaker.h"

namespace mye {

class World;
class GraphicsDevice;
class ShaderManager;
class IRenderPath;
struct RenderResources;

// 反射プローブのシーンキャプチャ基盤 (M56e)。
//
// `EnvMapBaker` は「空」しか焼けない — ソースが cubemap SRV か gradient の解析式の 2 択で、
// **位置の概念が無い**。反射プローブは「この場所から見た景色」を焼きたいので、
// シーンを 6 面へ実描画した cubemap を作ってから EnvMapBaker のプリフィルタへ渡す
// (`EnvMapBaker::BakeFrom`)。プリフィルタと irradiance の式は 1 行も複製していない。
//
// ★ベイクは**明示指示のときだけ**走らせる (自動ベイクにしない)。「見えたらベイク」に
//   すると撮影ごとに焼き上がりが変わり、決定的撮影 (M52c) が根元から壊れる。
// ★`RenderSystem::Render` は再入不可 (queue_ / skinPalettes_ / viewSerial_ / prevVP_ を
//   インスタンスで持つ) なので、**専用の RenderSystem を 1 個持つ**。メインの
//   RenderSystem から 6 面を呼ぶと RT テンポラルと TAA の描画通番が 6 進んで履歴が全滅する
//   (AssetPreviewCache が同じ理由で専用インスタンスを持っているのが前例)。

// ---- 面の幾何 (純関数。D3D も World も要らない = ヘッドレスで機械検査できる) ----
// u,v は [0,1] のテクスチャ座標で **v は下向き正**。ibl_common.hlsli の `BakeDir` が
// ndc.y (上向き正) を受けるのと対で、t = 1 - 2v が両者をつなぐ。

// 面 face のテクスチャ座標 (u,v) が見ている方向 (正規化済み)
DirectX::XMFLOAT3 ProbeFaceDir(int face, float u, float v);
// 方向 → (面, u, v)。ProbeFaceDir の逆。零ベクトルなら false
bool ProbeDirToFaceUv(const DirectX::XMFLOAT3& dir, int& face, float& u, float& v);
// 面 face を position から撮るビュー行列 (LH)。上方向は CubeFace(face).up
DirectX::XMFLOAT4X4 ProbeFaceView(int face, const DirectX::XMFLOAT3& position);
// 全面共通の射影 (画角 90 度・アスペクト 1)。6 面がちょうど全方位を隙間なく覆う条件
DirectX::XMFLOAT4X4 ProbeFaceProj(float nearZ, float farZ);

// 焼き上がった 1 個ぶんのプローブ。**テクスチャを自分で所有する** —
// M56f がプローブを複数持つので、ベイカ側のキャッシュではなく呼び出し側の持ち物にする
struct BakedProbe {
    DirectX::XMFLOAT3 position = { 0, 0, 0 };
    Microsoft::WRL::ComPtr<ID3D11Texture2D> captureTex;          // 生キャプチャ (HDR リニア)
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> captureSrv; // TextureCube ビュー
    // 面 1 枚ずつの Texture2D ビュー。ImGui::Image はキューブを描けないので、
    // サムネイル表示にはこちらが要る (中身は captureTex と同じテクセル)
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> faceSrv[6];
    EnvMapBaker::BakedEnv env; // プリフィルタ済み + irradiance
    bool valid = false;
};

// 焼き上がったプローブ束 (M56f)。**テクスチャの所有者**で、`set` はそこへの非所有ビュー。
// RenderView::probes はこの `set` を指す = 束を破棄したら必ず指す側も外すこと。
//
// ★1 本の TextureCubeArray にまとめてあるのは、光パスが SRV スロットを **t14 の 1 本**
//   しか持っていないから (統合契約 予約 2)。プローブごとに 1 枚張る設計はスロットが尽きる。
// ★キャプチャ 1 個ずつ (BakedProbe) も残す — Inspector / プレビュー窓が 6 面のサムネイルを
//   出すのに要る (ImGui はキューブを描けないので面の 2D ビューが要る)
struct ReflectionProbeArray {
    std::vector<BakedProbe> probes; // 焼いた順 = set.probes の添字 = cube array のスライス順
    Microsoft::WRL::ComPtr<ID3D11Texture2D> arrayTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> arraySrv;
    ReflectionProbeSet set;
    void Clear();
};

class ProbeBaker {
public:
    // キャプチャ解像度。EnvMapBaker::kSpecSize と同じ 128 にしてある —
    // プリフィルタの出力より細かく撮っても mip 0 で潰れるだけで意味が無い
    static constexpr int kCaptureSize = 128;

    // 6 面を実描画してプリフィルタまで通す。失敗しても out は触りかけで返る (valid=false)
    bool Bake(World& world, GraphicsDevice& device, IRenderPath& path, ShaderManager& shaders,
              RenderResources& resources, const DirectX::XMFLOAT3& position, float nearZ,
              float farZ, BakedProbe& out);
    // M56f: シーン中の ReflectionProbeComponent を**全部**焼いて 1 本の TextureCubeArray に
    // まとめる。out は毎回作り直す (差分ベイクはしない — 「焼いた面と箱の対応」が
    // 途中で入れ替わると原因を追えない形で壊れるため)。
    // ★呼べるのは「描いてよい場所」だけ (EditorApp::OnRenderViews / EngineLoop の
    //   スクショ保存後)。RTV / ビューポート / ラスタライザを総取り替えするので、
    //   ImGui のコールバックや UI 提出の途中から呼ぶと画面が壊れる (M56e で踏んだ)
    bool BakeAll(World& world, GraphicsDevice& device, IRenderPath& path, ShaderManager& shaders,
                 RenderResources& resources, ReflectionProbeArray& out);
    // BakedProbe → シェーダへ渡す非所有ポインタ束 (BRDF LUT はプローブ間で 1 枚共有)
    EnvMaps MapsFor(const BakedProbe& p) { return env_.MapsFor(p.env); }
    void Shutdown();

    // 直近 Bake の CPU 壁時計 (ms)。GPU の完了待ちはしていないので「投げ終わるまで」の値
    float LastBakeCpuMs() const { return lastBakeMs_; }

    // 背景色 (authored sRGB)。**postFx を切って直描きするので、リニア変換はこちらでやる** —
    // メイン描画は HDR 中間があるときだけ変換する規約なので、そのままでは色がずれる
    float clearColor[4] = { 0.08f, 0.09f, 0.11f, 1.0f };
    // 地形の解決に使う assets\ の絶対パス。**空 = 地形を 1 枚も写さない**
    // (RenderSystem::assetsRoot と同じ規約。専用 RenderSystem なのでチャンクの
    //  キャッシュもメイン描画とは別に積まれる)
    std::wstring assetsRoot;

private:
    bool EnsureDepth(GraphicsDevice& device);

    RenderSystem render_; // ★専用インスタンス (再入不可の回避。上のコメント参照)
    EnvMapBaker env_;     // プリフィルタ + irradiance + BRDF LUT
    Microsoft::WRL::ComPtr<ID3D11Texture2D> depthTex_; // 6 面で使い回す (面ごとにクリア)
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depthDsv_;
    float lastBakeMs_ = 0.0f;
};

// ---- 診断 (CLI `--probe-bake` が使う。CPU 側の純処理なので自己テストからも呼べる) ----

// キャプチャを CPU へ読み戻す。rgb は face-major (6 * size * size * 3、リニア)
bool ProbeReadFaces(GraphicsDevice& device, const BakedProbe& probe, std::vector<float>& rgb,
                    int& size);

// 面の継ぎ目の食い違い。**面の向き (回転 / 反転 / 画角) が壊れているかを絵抜きで検出する
// 唯一の手段**: 継ぎ目をまたいで隣り合うテクセルは「ほぼ同じ方向」を見ているので、
// 面が 90 度回っていれば差が跳ねる。
// ★分母を「シーンの明るさ」にしてはいけない — 暗いシーンでは正しい絵でも比が跳ねて、
//   しきい値がシーン依存になる (実際に踏んだ: 平均輝度 0.09 の --render-demo で 0.53)。
//   **面の内側で 1 テクセル隣へ進んだときの差**を分母にすると「継ぎ目は面の中と同じ
//   滑らかさか」という**自己校正された**問いになり、絵の内容にほとんど依存しなくなる。
struct ProbeSeamStats {
    float meanSeamDiff = 0.0f;     // 継ぎ目をまたぐ隣接テクセルの平均差 (チャンネル絶対値)
    float meanInteriorDiff = 0.0f; // 面の内側の隣接テクセルの平均差 (同じ 1 テクセルの歩幅)
    float seamRatio = 0.0f;        // seam / interior。**1 前後 = 継ぎ目が面の中と同じ**
    float maxSeamDiff = 0.0f;      // 参考値 (輪郭がまたぐと跳ねるので判定には使わない)
    float meanLuma = 0.0f;         // 参考値 (絵の明るさ)
    int samples = 0;               // 継ぎ目のサンプル数
};
bool ProbeSeamCheck(const std::vector<float>& rgb, int size, ProbeSeamStats& out);

// 継ぎ目比の上限 (CLI `--probe-bake` の合否判定)。実測値と「壊れたときの値」の
// **幾何平均**に置いてある: `--render-demo` を WARP で焼いて 正常 2.66 / +Z を 90 度
// 回した変異 6.06 (実走で測った値)。合成の滑らかなキューブでは 0.68 / 7.70。
// ★正常と変異が 2.3 倍しか離れていない = **シーン依存が残る判定**。とがった絵ばかりの
//   シーンでは正常でも上がりうるので、最終的な根拠は PNG の目視のほう
inline constexpr float kProbeSeamRatioLimit = 4.0f;

// 6 面を十字 (`+Y` / `-X +Z +X -Z` / `-Y`) に並べた PNG。この並びは**隣り合う面が
// 画像上でも隣り合う**ので、面の向きが壊れていれば繋ぎ目の段差として目で分かる。
// 露出は Reinhard + sRGB 符号化 (HDR をそのまま 8bit へ落とすと真っ白になる)
bool ProbeWriteFacesPng(const std::vector<float>& rgb, int size, const std::wstring& path);

} // namespace mye
