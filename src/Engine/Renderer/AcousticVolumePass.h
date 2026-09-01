//====================================================================================
//                          AcousticVolumePass.h
//  MyEngine/ 秋田蓮音                                                      09/01/2026
//                                          音響の残光ボリュームを 3D テクスチャへ転送する
//====================================================================================
#pragma once
#include <cstdint>

#include <d3d11.h>

#include "Engine/Renderer/VolumeTexture.h"

namespace mye {

class GraphicsDevice;

// CPU 側の残光グリッド 1 枚ぶんの転送指示 (M65d)。
//
// ★**AcousticField を直接受け取らない**のが要点。AcousticField は Engine 層に居るので、
//   Renderer 層 (ここ) から参照すると層の依存が逆流する
//   (Editor → GameLogic → Engine → Renderer → Core → Platform)。
//   詰め替えは RenderSystem (Engine 層) の仕事で、こちらは POD だけを見る。
struct AcousticVolumeUpload {
    const uint8_t* cells = nullptr;  // dimX*dimY*dimZ バイト。x が最内、次に y、最後に z
    int32_t dimX = 0, dimY = 0, dimZ = 0;
    uint32_t serial = 0; // 内容の通番。**前回と同じなら転送しない**
};

// 残光ボリュームの所有者 (M65d)。
//
// ★フォーマットは **R8_UNORM (SRV のみ / UAV 無し)**。GPU は 1 度も書かない —
//   波の伝播は決定論レーン (CPU) から動かせない (AI と絵が同じ場から出ることが
//   企画の要件) ので、GPU 側は「CPU が焼いた場を読むだけ」で足りる。
//   R8_UNORM は FL11_0 の typed UAV 必須リストの外なので、UAV 込みで作ると
//   ビュー作成が落ちる → VolumeTexture::Create(..., withUav=false) を使う。
// ★万一 R8_UNORM の 3D SRV が通らない環境が出たら、縮退は R16_FLOAT →
//   R8G8B8A8_UNORM の順 (どちらも CPU 側で詰め替えが要る)。現状は WARP / ハードとも
//   通ることを `--acoustic-dump` の読み戻しで実測してある。
//
// 転送は `UpdateSubresource` の全域 1 発。部分更新 (D3D11_BOX) にしないのは、
// **残光は毎 tick 全域で減衰する**ので非ゼロ領域が丸ごと dirty になるから —
// 汚れ AABB を追跡しない限り部分更新は効かない (必要になったら追跡から入れる)。
class AcousticVolumePass {
public:
    // 転送する (寸法が変わっていれば作り直す)。
    // 戻り値: このフレームに SRV が有効か。false = 供給しない = 消費側は従来どおりの絵
    bool Upload(GraphicsDevice& device, const AcousticVolumeUpload& src);
    void Release();

    ID3D11ShaderResourceView* SRV() const { return volume_.SRV(); }
    const VolumeTexture& Volume() const { return volume_; }
    // 直近の転送にかかった CPU 時間 [ms] (ProfilerWindow 表示用)。
    // 転送を省いたフレームは 0 になる = 「速い」ではなく「していない」
    float LastUploadMs() const { return uploadMs_; }
    int CellCount() const { return volume_.Width() * volume_.Height() * volume_.Depth(); }

private:
    VolumeTexture volume_;
    uint32_t uploadedSerial_ = 0;
    bool everUploaded_ = false;
    bool createFailed_ = false; // 一度落ちたら毎フレーム再試行してログを溢れさせない
    float uploadMs_ = 0.0f;
};

} // namespace mye
