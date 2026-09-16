//====================================================================================
//                          CpuModalBackend.h
//  MyEngine/ 秋田蓮音                                                      09/16/2026
//                                          Deep-Modal 推論の CPU 実装 (im2col + ブロック GEMM)
//====================================================================================
#pragma once
#include <vector>

#include "Engine/Engine/Modal/ModalInferenceBackend.h"

namespace mye {

// Conv3d/ConvTranspose3d の出力空間サイズ (PyTorch の F.conv3d / F.conv_transpose3d と同じ式)。
// selftest が「素朴な参照実装」を組み立てるときにも使う
void Conv3dOutSize(int d, int h, int w, int k, int stride, int pad, int& outD, int& outH, int& outW);
void ConvTranspose3dOutSize(int d, int h, int w, int k, int stride, int pad, int outPad, int& outD,
                            int& outH, int& outW);

// 生の Conv3d / ConvTranspose3d カーネル (im2col + 4x4 ブロック GEMM、単一スレッド)。
// 入出力は [C, D, H, W] (W が最内。VoxelGrid / モデル出力と同じ x-最内規約) の平坦配列。
// weight は Conv3d が [cout,cin,k,k,k]、ConvTranspose3d が [cin,cout,k,k,k]
// (PyTorch のテンソル形状そのまま、C 順)。dst は呼び出し側が out*outD*outH*outW 分確保する
// (spec §5 受け入れ条件 11: 素朴 6 重ループとの照合に selftest が直接叩く低レベル API)。
void Conv3dRaw(const float* src, int cin, int d, int h, int w, const float* weight,
              const float* bias, int cout, int k, int stride, int pad, float* dst, int outD,
              int outH, int outW);
void ConvTranspose3dRaw(const float* src, int cin, int d, int h, int w, const float* weight,
                        const float* bias, int cout, int k, int stride, int pad, int outPad,
                        float* dst, int outD, int outH, int outW);

// CPU 推論バックエンド (単一スレッド、SIMD intrinsics なし。/fp:precise のまま
// ベクトル化の余地を残す形の直接畳み込み実装)。RunsOnWorkerThread() == true
class CpuModalBackend : public ModalInferenceBackend {
public:
    const char* Name() const override
    {
        return "cpu";
    }
    bool RunsOnWorkerThread() const override
    {
        return true;
    }
    bool Prepare(const DmNet& net, std::string* err) override;
    bool Infer(const modal::VoxelGrid& in, std::vector<float>& out192x4096, std::string* err) override;

private:
    struct PreparedOp {
        DmNetOp op;
        std::vector<float> weight; // Conv3d/ConvTranspose3d のみ (ReLU/Add は空)
        std::vector<float> bias;
    };
    DmNetHeader header_;
    std::vector<PreparedOp> ops_;
    uint32_t bufferCount_ = 0;
};

} // namespace mye
