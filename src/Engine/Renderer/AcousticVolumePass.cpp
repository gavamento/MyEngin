//====================================================================================
//                          AcousticVolumePass.cpp
//  MyEngine/ 秋田蓮音                                                      09/01/2026
//                                          音響の残光ボリュームを 3D テクスチャへ転送する実装
//====================================================================================
#include "Engine/Renderer/AcousticVolumePass.h"

#include <chrono>

#include "Engine/Core/Log.h"
#include "Engine/Renderer/GraphicsDevice.h"

namespace mye {

bool AcousticVolumePass::Upload(GraphicsDevice& device, const AcousticVolumeUpload& src)
{
    uploadMs_ = 0.0f;
    if (src.cells == nullptr || src.dimX <= 0 || src.dimY <= 0 || src.dimZ <= 0) {
        return false;
    }
    const bool sameSize = volume_.Width() == src.dimX && volume_.Height() == src.dimY
        && volume_.Depth() == src.dimZ;
    if (createFailed_ && sameSize) {
        return false; // 同じ寸法で一度落ちている = 再試行しても同じ (ログを溢れさせない)
    }
    if (!volume_.IsValid() || !sameSize) {
        // ★UAV 無しで作る。R8_UNORM は FL11_0 の typed UAV 必須リストの外なので、
        //   ここで withUav=true にすると CreateUnorderedAccessView が落ちて丸ごと失敗する
        if (!volume_.Create(device, src.dimX, src.dimY, src.dimZ, DXGI_FORMAT_R8_UNORM, false)) {
            MYE_LOG_ERROR("AcousticVolumePass: R8_UNORM の Texture3D (%dx%dx%d) を作れなかった "
                          "— 縮退は R16_FLOAT -> R8G8B8A8_UNORM の順",
                          src.dimX, src.dimY, src.dimZ);
            createFailed_ = true;
            return false;
        }
        createFailed_ = false;
        everUploaded_ = false; // 作り直したら通番に関係なく 1 回は流す
    }

    // ★通番が同じフレームは転送しない。sim は 60Hz でしか進まないのに Runtime は
    //   vsync 無効で数千 fps 回るので、素直に毎フレーム流すと 130KB のコピーが
    //   ほとんど無駄打ちになる (froxel が viewSerial で履歴を判定しているのと同じ発想)
    if (everUploaded_ && uploadedSerial_ == src.serial) {
        return true;
    }

    const auto t0 = std::chrono::steady_clock::now();
    // ★RowPitch は「1 行のバイト数」= dimX、DepthPitch は「1 スライスのバイト数」=
    //   dimX*dimY。**この 2 つを取り違えると Z がずれた絵になり、しかも絵は普通に出る**
    //   ので目視では絶対に見つからない — `--acoustic-dump` の読み戻しが唯一の網
    device.Context()->UpdateSubresource(volume_.Texture(), 0, nullptr, src.cells,
                                        static_cast<UINT>(src.dimX),
                                        static_cast<UINT>(src.dimX) * static_cast<UINT>(src.dimY));
    const auto t1 = std::chrono::steady_clock::now();
    uploadMs_ = static_cast<float>(std::chrono::duration<double, std::milli>(t1 - t0).count());
    uploadedSerial_ = src.serial;
    everUploaded_ = true;
    return true;
}

void AcousticVolumePass::Release()
{
    volume_.Release();
    uploadedSerial_ = 0;
    everUploaded_ = false;
    createFailed_ = false;
    uploadMs_ = 0.0f;
}

} // namespace mye
