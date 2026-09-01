#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

namespace mye {

class GraphicsDevice;

// M57a: 3D テクスチャ (Texture3D + typed UAV + SRV) の所有者。
//
// なぜ RenderTexture に相乗りしないのか: RenderTexture は 2D 専用で、RTV / DSV /
// read-only DSV / 深度 SRV という「ラスタライズ用の 4 点セット」を抱えている。
// フロクセルは CS が書いて PS が読むだけ = RTV も DSV も一切要らないので、
// あちらへ depth3D / rtv3D を足すと **2D 経路の全呼び出し側にフラグが 1 本増える**。
// 別の型にしたほうが読む側も少ない。Create / Resize / Release の作法だけ揃えてある
// (毎フレーム Resize を呼び、同寸なら no-op で通過する = RenderTexture と同じ流儀)。
//
// ★フォーマットの制約: FL11_0 の typed UAV **ストア**はほぼ全フォーマットで通るが、
//   typed UAV **ロード** (RWTexture3D からの読み) は R32_FLOAT/UINT/SINT 限定。
//   froxel の各パスは「UAV へ書く / SRV で読む」しかしないのでこの制限には当たらない
//   (M57c のテンポラルも履歴は SRV 側で読む)。
//   WARP が本当に R16G16B16A16_FLOAT の 3D UAV ストアを通すかは机上では決まらないので、
//   下の RunFroxelVolumeProbe が実際に書いて読み戻して確かめる。
//
// ★M65d: `withUav = false` で **SRV だけ**の 3D テクスチャも作れるようにした。
//   R8_UNORM は FL11_0 の「typed UAV 必須」リストの外なので、UAV 込みで作ろうとすると
//   CreateUnorderedAccessView が落ちて Create ごと false を返す。音響の残光は CPU が
//   UpdateSubresource で流し込んで PS が読むだけ = UAV は 1 度も要らないので、
//   ビットを落とせば通る。引数作法は RenderTexture::Create に揃えてある。
class VolumeTexture {
public:
    bool Create(GraphicsDevice& device, int width, int height, int depth,
                DXGI_FORMAT format = DXGI_FORMAT_R16G16B16A16_FLOAT, bool withUav = true);
    // 同寸・同フォーマット・同 UAV 有無なら何もしない (RenderTexture::Resize と同じ契約)
    void Resize(GraphicsDevice& device, int width, int height, int depth,
                DXGI_FORMAT format = DXGI_FORMAT_R16G16B16A16_FLOAT, bool withUav = true);
    void Release();

    ID3D11ShaderResourceView* SRV() const { return srv_.Get(); }
    ID3D11UnorderedAccessView* UAV() const { return uav_.Get(); }
    // M65d: CPU から UpdateSubresource で流し込むための実体 (UAV 無しの経路が使う)
    ID3D11Texture3D* Texture() const { return tex_.Get(); }
    int Width() const { return width_; }
    int Height() const { return height_; }
    int Depth() const { return depth_; }
    DXGI_FORMAT Format() const { return format_; }
    bool IsValid() const { return srv_ != nullptr; }

    // 検証用の同期読み戻し。STAGING テクスチャを都度作って CopyResource + Map する =
    // GPU を完全に待たせる経路なので、**毎フレームの描画からは呼ばないこと**
    // (プローブと将来の selftest 専用)。対応フォーマットは R16G16B16A16_FLOAT /
    // R32G32B32A32_FLOAT / R8_UNORM (M65d) のみ — それ以外は false を返す。
    // R8_UNORM のときは out[0] だけ埋まり、out[1..3] は 0
    bool ReadbackTexel(GraphicsDevice& device, int x, int y, int z, float out[4]) const;

    // M65d: R8_UNORM 専用の全セル読み戻し (`--acoustic-dump`)。out は w*h*d バイトで、
    // **CPU 側の配列と memcmp できる並び** (x が最内、次に y、最後に z)。
    // ★これが `SysMemPitch` / `SysMemSlicePitch` の取り違えを捕まえる唯一の網 —
    //   取り違えると「Z がずれた絵」になり、しかも絵は普通に出るので目視では分からない
    bool ReadbackBytes(GraphicsDevice& device, std::vector<uint8_t>& out) const;

    // M57b: 全セルの同期読み戻し (`--froxel-dump`)。out は w*h*d*4 の float 列
    // (x が最内、次に y、最後に z)。**7MB のボリュームで float 換算 14.7MB を
    // 確保して GPU を完全に待たせる**ので、デバッグの一発勝負にしか使わないこと。
    // 対応フォーマットは ReadbackTexel と同じ 2 つ
    bool ReadbackAll(GraphicsDevice& device, std::vector<float>& out) const;

private:
    Microsoft::WRL::ComPtr<ID3D11Texture3D> tex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav_;
    int width_ = 0;
    int height_ = 0;
    int depth_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_R16G16B16A16_FLOAT;
    bool hasUav_ = true; // M65d: Resize の「同じ構成か」判定に要る (寸法だけでは足りない)
};

// ---- M57a: WARP 実測プローブ (`Editor.exe --froxel-probe`) ----
//
// なぜ「設計より先に計測」なのか: shot_verify.bat が「RT デモは WARP では重すぎる」と
// 明記していて、その RT GI は 960x540 の半解像度 = 約 130k ピクセル。フロクセル
// 160x90x64 は **セル数だけで 7 倍**の 921,600 ある。ここが CI 予算に載らないなら
// M57b 以降の設計 (解像度・パス数・golden を CI に入れるか) が全部変わるので、
// 実装を始める前に「空の CS を回した壁時計」と「typed 3D UAV が WARP で動くか」を
// 数字で確定させる。**この数字を出すこと自体が M57a の成果物**。
//
// ウィンドウも sim も作らない (GraphicsDevice + ShaderManager だけの裸経路)。
// 戻り値: 0 = 全候補で UAV ストアが正しく動いた / 1 = 書き込み結果が食い違った /
//         2 = デバイスかシェーダが用意できず計測に至らなかった (「速い」と混同しない)
struct FroxelProbeOptions {
    bool forceWarp = false;               // --warp
    int iterations = 64;                  // 計測ループの回数
    std::vector<std::wstring> shaderDirs; // ShaderManager::Init へそのまま渡す (優先度順)
};
int RunFroxelVolumeProbe(const FroxelProbeOptions& options);

} // namespace mye
