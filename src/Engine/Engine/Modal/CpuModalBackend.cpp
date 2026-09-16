//====================================================================================
//                          CpuModalBackend.cpp
//  MyEngine/ 秋田蓮音                                                      09/16/2026
//                                          CPU 推論の実装 (im2col + ブロック GEMM)
//====================================================================================
#include "Engine/Engine/Modal/CpuModalBackend.h"

#include <algorithm>
#include <cstring>

namespace mye {
namespace {

// C[M,N] = A[M,K] * B[K,N] + bias[M] (行ごとに加算)。4x4 ブロックで A/B を再利用し、
// 素朴な 3 重ループより実効メモリ帯域を落とす (spec §4.2「im2col + 4x4 レジスタブロック GEMM」)。
// 累算順は (i,j) 出力ごとに k=0..K-1 の昇順で固定 — ブロック化は「どの順で足すか」ではなく
// 「何度メモリを読み直すか」を変えるだけなので、/fp:precise でも結果は変わらない
void GemmBiasAdd(const float* a, const float* b, const float* bias, float* c, int m, int n, int k)
{
    constexpr int kMb = 4;
    constexpr int kNb = 4;
    for (int m0 = 0; m0 < m; m0 += kMb) {
        const int mLim = (std::min)(kMb, m - m0);
        for (int n0 = 0; n0 < n; n0 += kNb) {
            const int nLim = (std::min)(kNb, n - n0);
            float acc[kMb][kNb] = {};
            for (int kk = 0; kk < k; ++kk) {
                float av[kMb];
                for (int i = 0; i < mLim; ++i) {
                    av[i] = a[static_cast<size_t>(m0 + i) * k + kk];
                }
                float bv[kNb];
                for (int j = 0; j < nLim; ++j) {
                    bv[j] = b[static_cast<size_t>(kk) * n + n0 + j];
                }
                for (int i = 0; i < mLim; ++i) {
                    for (int j = 0; j < nLim; ++j) {
                        acc[i][j] += av[i] * bv[j];
                    }
                }
            }
            for (int i = 0; i < mLim; ++i) {
                for (int j = 0; j < nLim; ++j) {
                    c[static_cast<size_t>(m0 + i) * n + n0 + j] = acc[i][j] + bias[m0 + i];
                }
            }
        }
    }
}

} // namespace

void Conv3dOutSize(int d, int h, int w, int k, int stride, int pad, int& outD, int& outH, int& outW)
{
    outD = (d + 2 * pad - k) / stride + 1;
    outH = (h + 2 * pad - k) / stride + 1;
    outW = (w + 2 * pad - k) / stride + 1;
}

void ConvTranspose3dOutSize(int d, int h, int w, int k, int stride, int pad, int outPad, int& outD,
                            int& outH, int& outW)
{
    outD = stride * (d - 1) - 2 * pad + k + outPad;
    outH = stride * (h - 1) - 2 * pad + k + outPad;
    outW = stride * (w - 1) - 2 * pad + k + outPad;
}

void Conv3dRaw(const float* src, int cin, int d, int h, int w, const float* weight,
              const float* bias, int cout, int k, int stride, int pad, float* dst, int outD,
              int outH, int outW)
{
    // im2col: cols[row][opos] = src[ci, id, ih, iw] (範囲外は 0)。
    // row = ((ci*k+kd)*k+kh)*k+kw は weight [cout,cin,k,k,k] の後半 4 軸と同じ並びなので、
    // weight をそのまま A[cout][K] として GEMM に渡せる (reshape 不要)
    const int kk = k;
    const size_t bigK = static_cast<size_t>(cin) * kk * kk * kk;
    const size_t bigN = static_cast<size_t>(outD) * outH * outW;
    std::vector<float> cols(bigK * bigN, 0.0f);

    for (int ci = 0; ci < cin; ++ci) {
        for (int kd = 0; kd < kk; ++kd) {
            for (int kh = 0; kh < kk; ++kh) {
                for (int kw = 0; kw < kk; ++kw) {
                    const size_t row = ((static_cast<size_t>(ci) * kk + kd) * kk + kh) * kk + kw;
                    float* colRow = cols.data() + row * bigN;
                    for (int od = 0; od < outD; ++od) {
                        const int id = od * stride - pad + kd;
                        if (id < 0 || id >= d) {
                            continue;
                        }
                        for (int oh = 0; oh < outH; ++oh) {
                            const int ih = oh * stride - pad + kh;
                            if (ih < 0 || ih >= h) {
                                continue;
                            }
                            const float* srcRow =
                                src + ((static_cast<size_t>(ci) * d + id) * h + ih) * w;
                            float* dstRow = colRow + (static_cast<size_t>(od) * outH + oh) * outW;
                            for (int ow = 0; ow < outW; ++ow) {
                                const int iw = ow * stride - pad + kw;
                                if (iw >= 0 && iw < w) {
                                    dstRow[ow] = srcRow[iw];
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    GemmBiasAdd(weight, cols.data(), bias, dst, cout, static_cast<int>(bigN), static_cast<int>(bigK));
}

void ConvTranspose3dRaw(const float* src, int cin, int d, int h, int w, const float* weight,
                        const float* bias, int cout, int k, int stride, int pad, int outPad,
                        float* dst, int outD, int outH, int outW)
{
    // 「stride 散布 + 反転カーネルの Conv」(spec §4.1 のヒント通りの実装):
    //  1. 入力を stride 個おきに間隔を空けて (dilate) 敷き、前後に (k-1-pad) / (k-1-pad+outPad)
    //     だけゼロ詰めする
    //  2. 重みを [cin,cout,k,k,k] → [cout,cin,反転k,反転k,反転k] へ組み替える
    //  3. stride=1, pad=0 の通常畳み込み (Conv3dRaw) を 1 回呼ぶ
    // (Do = s*(Din-1) - 2p + k + outPad の式に一致することは CpuModalBackend の
    //  コメント/ModalSelfTest の素朴実装照合で検証済み)
    const int padLow = k - 1 - pad;
    const int padHigh = k - 1 - pad + outPad;
    const int dilD = (d - 1) * stride + 1;
    const int dilH = (h - 1) * stride + 1;
    const int dilW = (w - 1) * stride + 1;
    const int pD = dilD + padLow + padHigh;
    const int pH = dilH + padLow + padHigh;
    const int pW = dilW + padLow + padHigh;

    std::vector<float> padded(static_cast<size_t>(cin) * pD * pH * pW, 0.0f);
    for (int ci = 0; ci < cin; ++ci) {
        for (int id = 0; id < d; ++id) {
            const int pd = padLow + id * stride;
            for (int ih = 0; ih < h; ++ih) {
                const int ph = padLow + ih * stride;
                const float* srcRow = src + ((static_cast<size_t>(ci) * d + id) * h + ih) * w;
                float* dstBase =
                    padded.data() + ((static_cast<size_t>(ci) * pD + pd) * pH + ph) * pW + padLow;
                for (int iw = 0; iw < w; ++iw) {
                    dstBase[static_cast<size_t>(iw) * stride] = srcRow[iw];
                }
            }
        }
    }

    std::vector<float> flipped(static_cast<size_t>(cout) * cin * k * k * k);
    for (int ci = 0; ci < cin; ++ci) {
        for (int co = 0; co < cout; ++co) {
            for (int kd = 0; kd < k; ++kd) {
                for (int kh = 0; kh < k; ++kh) {
                    for (int kw = 0; kw < k; ++kw) {
                        const size_t srcIdx =
                            ((((static_cast<size_t>(ci) * cout + co) * k + kd) * k + kh) * k) + kw;
                        const size_t dstIdx = ((((static_cast<size_t>(co) * cin + ci) * k
                                                 + (k - 1 - kd))
                                                    * k
                                                + (k - 1 - kh))
                                                   * k)
                            + (k - 1 - kw);
                        flipped[dstIdx] = weight[srcIdx];
                    }
                }
            }
        }
    }

    Conv3dRaw(padded.data(), cin, pD, pH, pW, flipped.data(), bias, cout, k, /*stride=*/1,
             /*pad=*/0, dst, outD, outH, outW);
}

bool CpuModalBackend::Prepare(const DmNet& net, std::string* err)
{
    ops_.clear();
    ops_.reserve(net.ops.size());
    for (const DmNetOp& op : net.ops) {
        PreparedOp p;
        p.op = op;
        if (op.type == kDmNetOpConv3d || op.type == kDmNetOpConvTranspose3d) {
            if (!DmNetOpWeightFloat(net, op, p.weight, p.bias, err)) {
                ops_.clear();
                return false;
            }
        }
        ops_.push_back(std::move(p));
    }
    header_ = net.header;
    bufferCount_ = static_cast<uint32_t>(net.header.bufferCount);
    if (bufferCount_ == 0 && !net.ops.empty()) {
        // ヘッダの bufferCount が未設定 (0) でも動けるように、ops から復元する保険
        int32_t maxIdx = 0;
        for (const DmNetOp& op : net.ops) {
            maxIdx = (std::max)({ maxIdx, op.out, op.in0, op.in1 });
        }
        bufferCount_ = static_cast<uint32_t>(maxIdx + 1);
    }
    return true;
}

bool CpuModalBackend::Infer(const modal::VoxelGrid& in, std::vector<float>& out192x4096,
                            std::string* err)
{
    if (ops_.empty()) {
        if (err) {
            *err = "backend not prepared (call Prepare first)";
        }
        return false;
    }

    struct Buf {
        int c = 0, d = 0, h = 0, w = 0;
        std::vector<float> data;
    };
    std::vector<Buf> bufs(bufferCount_);

    // buffer 0 = 入力 (占有 → float、チャンネル数は先頭 op の cin。VoxelGrid.occ は
    // x が最内 (VoxelIndexOf(x,y,z)=x+n*(y+n*z)) = このバックエンドの [C,D,H,W] 規約 (W=x 最内) と一致)
    const int n = kModalVoxelN;
    const int cin0 = ops_.front().op.cin;
    Buf& b0 = bufs[0];
    b0.c = cin0;
    b0.d = n;
    b0.h = n;
    b0.w = n;
    b0.data.assign(static_cast<size_t>(cin0) * n * n * n, 0.0f);
    if (cin0 == 1) {
        for (size_t i = 0; i < in.occ.size(); ++i) {
            b0.data[i] = in.occ[i] ? 1.0f : 0.0f;
        }
    } else {
        if (err) {
            *err = "unsupported input channel count (expected 1)";
        }
        return false;
    }

    for (const PreparedOp& p : ops_) {
        const DmNetOp& op = p.op;
        if (op.in0 < 0 || static_cast<uint32_t>(op.in0) >= bufferCount_ || op.out < 0
            || static_cast<uint32_t>(op.out) >= bufferCount_) {
            if (err) {
                *err = "op buffer index out of range";
            }
            return false;
        }
        const Buf& src = bufs[static_cast<size_t>(op.in0)];
        Buf& dst = bufs[static_cast<size_t>(op.out)];
        switch (op.type) {
        case kDmNetOpRelu:
            dst.c = src.c;
            dst.d = src.d;
            dst.h = src.h;
            dst.w = src.w;
            dst.data.resize(src.data.size());
            for (size_t i = 0; i < src.data.size(); ++i) {
                dst.data[i] = (std::max)(0.0f, src.data[i]);
            }
            break;
        case kDmNetOpAdd: {
            if (op.in1 < 0 || static_cast<uint32_t>(op.in1) >= bufferCount_) {
                if (err) {
                    *err = "add op missing in1";
                }
                return false;
            }
            const Buf& src1 = bufs[static_cast<size_t>(op.in1)];
            dst.c = src.c;
            dst.d = src.d;
            dst.h = src.h;
            dst.w = src.w;
            dst.data.resize(src.data.size());
            for (size_t i = 0; i < src.data.size(); ++i) {
                dst.data[i] = src.data[i] + src1.data[i];
            }
            break;
        }
        case kDmNetOpConv3d: {
            int outD = 0, outH = 0, outW = 0;
            Conv3dOutSize(src.d, src.h, src.w, op.k, op.stride, op.pad, outD, outH, outW);
            dst.c = op.cout;
            dst.d = outD;
            dst.h = outH;
            dst.w = outW;
            dst.data.assign(static_cast<size_t>(op.cout) * outD * outH * outW, 0.0f);
            Conv3dRaw(src.data.data(), src.c, src.d, src.h, src.w, p.weight.data(), p.bias.data(),
                     op.cout, op.k, op.stride, op.pad, dst.data.data(), outD, outH, outW);
            break;
        }
        case kDmNetOpConvTranspose3d: {
            int outD = 0, outH = 0, outW = 0;
            ConvTranspose3dOutSize(src.d, src.h, src.w, op.k, op.stride, op.pad, op.outPad, outD,
                                   outH, outW);
            dst.c = op.cout;
            dst.d = outD;
            dst.h = outH;
            dst.w = outW;
            dst.data.assign(static_cast<size_t>(op.cout) * outD * outH * outW, 0.0f);
            ConvTranspose3dRaw(src.data.data(), src.c, src.d, src.h, src.w, p.weight.data(),
                              p.bias.data(), op.cout, op.k, op.stride, op.pad, op.outPad,
                              dst.data.data(), outD, outH, outW);
            break;
        }
        default:
            if (err) {
                *err = "unknown op type";
            }
            return false;
        }
    }

    const Buf& last = bufs[static_cast<size_t>(ops_.back().op.out)];
    out192x4096.assign(last.data.begin(), last.data.end());
    return true;
}

} // namespace mye
