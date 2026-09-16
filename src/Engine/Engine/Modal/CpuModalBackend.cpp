//====================================================================================
//                          CpuModalBackend.cpp
//  MyEngine/ 秋田蓮音                                                      09/16/2026
//                                          CPU 推論の実装 (im2col + ブロック GEMM、AVX2 + マルチスレッド)
//====================================================================================
#include "Engine/Engine/Modal/CpuModalBackend.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <immintrin.h>
#include <intrin.h>
#include <thread>

namespace mye {
namespace {

// ---- sub-09 (M76e2): マルチスレッド化の安全性について ----
// jobs::System() (JobSystem.h) の ParallelRanges は「同期呼び出しなので常に高々 1 バッチ」と
// 明記されている実装で、共有の batch_/cursor_ を「呼び出し元は 1 つだけ」という前提のまま
// mutex で保護している (JobSystem.cpp の DrainChunks コメント参照)。Deep-Modal の推論は
// ModalSoundLibrary の非同期焼きワーカースレッド上で走る一方、メインスレッドの
// TransformSystem::Update / RenderSystem も毎 tick/フレーム jobs::System().ParallelRanges を
// 呼ぶ (実地確認: src\Engine\Engine\TransformSystem.cpp / RenderSystem.cpp)。
// 2 つの呼び出し元が同時に同じ JobSystem インスタンスの batch_ / 単調増加する cursor_ を
// 書き換えると、片方のバッチの chunk をもう片方の呼び出しが横取りする形で壊れる
// (cursor_ はバッチをまたいで単調増加するだけで、どのバッチの chunk かは base/last の
// 範囲でしか区別しない — 2 つの呼び出し元が同時に batch_ を上書きすると、この前提が崩れる)。
// このため Modal 専用に、呼び出しのたびに使い捨てるだけの単純なスレッド分割 (ParallelSpan) を
// 持つ。スレッド生成コストは実測 (scratchpad の使い捨てベンチマーク) で ~0.08ms/スレッドで、
// Infer() 1 回あたり数十回呼んでも合計は数 ms — 目標の秒未満の推論時間に対して無視できる。
// 出力レンジだけを分割し、リダクション (K 次元の総和順) は 1 スレッドが 0..K-1 を固定順で
// 計算しきるので、maxThreads をいくつに変えても同じ入力からは同じビット列が出る
// (spec §5 受け入れ条件 23)。
//
// ★round 1 で見逃した罠 (spec §5 受け入れ条件 23 に追記): 「K を割らない」だけでは
//   GEMM の決定論には不十分だった。GemmBiasAddAvx2Range は 8 列ブロックを FMA (乗算+加算を
//   1 回で丸める) で処理し、8 に満たない端数だけを GemmBiasAddScalarRange (乗算→加算の
//   2 回丸め) へ回す。align=1 のままだとチャンク幅 ceil(total/nThreads) が 8 の倍数とは
//   限らず、「どの列が AVX2 でどの列が端数か」がスレッド数ごとに変わってしまう
//   (実測: T=4 vs T=3/5 で .msfm が不一致、T=4 vs T=2/8 は N が 8 の倍数なので偶然一致した
//   — 2 冪のスレッド数だけを検査すると原理的に検出できないバグだった)。
//   `align` (既定 1 = 従来どおり) にチャンク幅の量子化単位を渡せるようにし、GEMM の呼び出し
//   側だけ align=8 (AVX2 幅) を渡す。chunk を align の倍数に切り上げておけば、最初のチャンクの
//   境界は必ず align の倍数になる — その後の境界は「align の倍数」× t なので同じく align の
//   倍数、最後のチャンクの終端だけが total (align の倍数とは限らない) になるが、この端数は
//   total mod align 個で常に固定 (どの nThreads でも同じ絶対 index に落ちる)。
//   im2col の行分割・ReLU/Add (SIMD と scalar が同じ丸みで完全に同値) は align=1 のままでよい。
void ParallelSpan(size_t total, int maxThreads, const std::function<void(size_t, size_t)>& fn,
                  size_t align = 1)
{
    if (total == 0) {
        return;
    }
    const int nThreads = (std::max)(1, (std::min)(maxThreads, static_cast<int>(total)));
    if (nThreads <= 1) {
        fn(0, total);
        return;
    }
    size_t chunk = (total + static_cast<size_t>(nThreads) - 1) / static_cast<size_t>(nThreads);
    if (align > 1) {
        chunk = ((chunk + align - 1) / align) * align; // 境界を align の倍数へ切り上げる
    }
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(nThreads - 1));
    for (int t = 1; t < nThreads; ++t) {
        const size_t begin = static_cast<size_t>(t) * chunk;
        if (begin >= total) {
            break;
        }
        const size_t end = (std::min)(begin + chunk, total);
        workers.emplace_back([&fn, begin, end] { fn(begin, end); });
    }
    // 呼出スレッドも先頭チャンクを担当する (JobSystem の DrainChunks と同じ流儀 — 作る
    // スレッドを 1 本減らせる上に、nThreads==1 に丸まった場合との経路差を小さくできる)
    const size_t end0 = (std::min)(chunk, total);
    fn(0, end0);
    for (std::thread& th : workers) {
        th.join();
    }
}

// AVX2 + FMA3 の実行時検出。CPUID の機能ビットだけでなく、OS が XSAVE で YMM 状態を
// 保存する設定になっているか (XCR0) まで見る — CPU が AVX2 に対応していても OS が
// 無効化している構成 (まず無いが古い仮想化層等) を弾くため
bool DetectAvx2FmaRaw()
{
    int info[4] = { 0, 0, 0, 0 };
    __cpuid(info, 0);
    if (info[0] < 7) {
        return false;
    }
    __cpuidex(info, 1, 0);
    const bool osxsave = (info[2] & (1 << 27)) != 0;
    const bool avx = (info[2] & (1 << 28)) != 0;
    const bool fma = (info[2] & (1 << 12)) != 0;
    if (!osxsave || !avx || !fma) {
        return false;
    }
    const unsigned long long xcr0 = _xgetbv(0);
    if ((xcr0 & 0x6) != 0x6) { // bit1=XMM, bit2=YMM を OS が保存する
        return false;
    }
    __cpuidex(info, 7, 0);
    return (info[1] & (1 << 5)) != 0; // AVX2
}

// GemmBiasAddAvx2Range の SIMD 幅 (256bit = float8)。GemmBiasAddDispatch のチャンク量子化
// (ParallelSpan の align 引数) もこの値を使う — 2 箇所が食い違うと量子化の意味が無くなる
constexpr size_t kGemmAvx2Width = 8;

// C[M,N] = A[M,K] * B[K,N] + bias[M] (行ごとに加算)、列レンジ [nBegin,nEnd) だけを計算する
// スカラー版。4x4 ブロックで A/B を再利用し、素朴な 3 重ループより実効メモリ帯域を落とす
// (spec §4.2「im2col + 4x4 レジスタブロック GEMM」)。累算順は (i,j) 出力ごとに k=0..K-1 の
// 昇順で固定 — ブロック化は「どの順で足すか」ではなく「何度メモリを読み直すか」を変えるだけ
// なので、/fp:precise でも結果は変わらない。
// ★[nBegin,nEnd)=[0,n) で呼べば sub-05 時点の GemmBiasAdd と 1 命令も違わない同一計算になる
//   (ModalSelfTest の素朴参照実装照合 1e-6 はこの経路のまま)
void GemmBiasAddScalarRange(const float* a, const float* b, const float* bias, float* c, int m,
                            int n, int k, size_t nBegin, size_t nEnd)
{
    constexpr int kMb = 4;
    constexpr size_t kNb = 4;
    for (int m0 = 0; m0 < m; m0 += kMb) {
        const int mLim = (std::min)(kMb, m - m0);
        for (size_t n0 = nBegin; n0 < nEnd; n0 += kNb) {
            const size_t nLim = (std::min)(kNb, nEnd - n0);
            float acc[kMb][kNb] = {};
            for (int kk = 0; kk < k; ++kk) {
                float av[kMb];
                for (int i = 0; i < mLim; ++i) {
                    av[i] = a[static_cast<size_t>(m0 + i) * k + kk];
                }
                float bv[kNb];
                for (size_t j = 0; j < nLim; ++j) {
                    bv[j] = b[static_cast<size_t>(kk) * n + n0 + j];
                }
                for (int i = 0; i < mLim; ++i) {
                    for (size_t j = 0; j < nLim; ++j) {
                        acc[i][j] += av[i] * bv[j];
                    }
                }
            }
            for (int i = 0; i < mLim; ++i) {
                for (size_t j = 0; j < nLim; ++j) {
                    c[static_cast<size_t>(m0 + i) * n + n0 + j] = acc[i][j] + bias[m0 + i];
                }
            }
        }
    }
}

// AVX2/FMA 版: N 方向を 8 幅 (256bit = float8) で処理し、M 方向は 4 行を YMM アキュムレータに
// 保持する (4 本 + b のロード 1 本 = 5 レジスタ、16 本ある YMM に十分収まる)。
// 累算順はスカラー版と同じ k=0..K-1 の昇順 — 各 (m,n) は 1 回の FMA (乗算+加算を 1 命令、
// 1 回丸め) で更新するので、スカラー版 (乗算→加算の 2 回丸め) とは最終ビットが一致しない
// (fixture の許容 1e-3 で吸収する、spec §5 受け入れ条件 23)。
// 端数 (8 未満の列) はスカラー版へ委譲する — 同じ [nBegin,nEnd) 契約を保つため
void GemmBiasAddAvx2Range(const float* a, const float* b, const float* bias, float* c, int m,
                          int n, int k, size_t nBegin, size_t nEnd)
{
    constexpr int kMb = 4;
    size_t n0 = nBegin;
    for (; n0 + kGemmAvx2Width <= nEnd; n0 += kGemmAvx2Width) {
        for (int m0 = 0; m0 < m; m0 += kMb) {
            const int mLim = (std::min)(kMb, m - m0);
            __m256 acc[kMb] = { _mm256_setzero_ps(), _mm256_setzero_ps(), _mm256_setzero_ps(),
                                _mm256_setzero_ps() };
            for (int kk = 0; kk < k; ++kk) {
                const __m256 bv = _mm256_loadu_ps(b + static_cast<size_t>(kk) * n + n0);
                for (int i = 0; i < mLim; ++i) {
                    const __m256 av = _mm256_set1_ps(a[static_cast<size_t>(m0 + i) * k + kk]);
                    acc[i] = _mm256_fmadd_ps(av, bv, acc[i]);
                }
            }
            for (int i = 0; i < mLim; ++i) {
                const __m256 withBias = _mm256_add_ps(acc[i], _mm256_set1_ps(bias[m0 + i]));
                _mm256_storeu_ps(c + static_cast<size_t>(m0 + i) * n + n0, withBias);
            }
        }
    }
    if (n0 < nEnd) {
        GemmBiasAddScalarRange(a, b, bias, c, m, n, k, n0, nEnd);
    }
    _mm256_zeroupper(); // レガシー SSE 経路 (このファイル外) との遷移ペナルティを避ける
}

// GEMM の出力側 (N 次元) だけをスレッド分割する。K 次元 (リダクション) は分割しないので
// maxThreads を変えても結果はビット一致する… だけでは不十分だった (round 1 の欠陥、
// 上の ParallelSpan コメント参照)。AVX2 版は 8 列ブロックを FMA (1 回丸め) で処理し、
// 端数だけスカラー (2 回丸め) に回すので、チャンク境界がどこに来るかで「どの列が
// どちらの丸めで計算されるか」が変わってしまう。align=kGemmAvx2Width を渡してチャンク幅を
// 8 の倍数へ量子化することで、境界を必ず 8 列ブロックの切れ目に揃える (最後のレンジだけに
// 残る端数は total%8 個で、これは nThreads に依らず常に同じ絶対 index に落ちる)。
// avx2==false (スカラー強制) のときは端数の概念自体が無い (全域が同じ丸め) ので
// 量子化は無害な no-op — align を条件分岐せず常に渡してよい
void GemmBiasAddDispatch(const float* a, const float* b, const float* bias, float* c, int m, int n,
                         int k, bool useAvx2, int maxThreads)
{
    const bool avx2 = useAvx2 && ModalCpuDetectAvx2Fma();
    auto worker = [&](size_t nBegin, size_t nEnd) {
        if (avx2) {
            GemmBiasAddAvx2Range(a, b, bias, c, m, n, k, nBegin, nEnd);
        } else {
            GemmBiasAddScalarRange(a, b, bias, c, m, n, k, nBegin, nEnd);
        }
    };
    ParallelSpan(static_cast<size_t>(n), maxThreads, worker, kGemmAvx2Width);
}

// ReLU / Add (要素ごと、リダクション無し) — AVX2 でもスカラーでも同じ最終ビットになる
// (max/add は 1 回の演算で丸めも 1 回、SIMD と分岐しても IEEE754 上は等価)。
// スレッド分割・SIMD 幅の違いは「どの要素をどのスレッド/命令が計算するか」だけを変えるので
// 決定論に影響しない
void ReluRange(const float* src, float* dst, size_t begin, size_t end, bool useAvx2)
{
    size_t i = begin;
    if (useAvx2) {
        const __m256 zero = _mm256_setzero_ps();
        for (; i + 8 <= end; i += 8) {
            _mm256_storeu_ps(dst + i, _mm256_max_ps(zero, _mm256_loadu_ps(src + i)));
        }
        _mm256_zeroupper();
    }
    for (; i < end; ++i) {
        dst[i] = (std::max)(0.0f, src[i]);
    }
}

void AddRange(const float* a, const float* b, float* dst, size_t begin, size_t end, bool useAvx2)
{
    size_t i = begin;
    if (useAvx2) {
        for (; i + 8 <= end; i += 8) {
            _mm256_storeu_ps(dst + i, _mm256_add_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i)));
        }
        _mm256_zeroupper();
    }
    for (; i < end; ++i) {
        dst[i] = a[i] + b[i];
    }
}

} // namespace

bool ModalCpuDetectAvx2Fma()
{
    static const bool has = DetectAvx2FmaRaw(); // プロセス内で 1 回だけ CPUID を叩く
    return has;
}

int ModalCpuDefaultThreadCount()
{
    const unsigned hw = std::thread::hardware_concurrency();
    int n = hw > 1 ? static_cast<int>(hw) - 1 : 1; // 1 コアはメイン/OS 用に残す
    n = (std::min)(n, 4);                          // 焼きは裏で走るので全コアは使わない
    n = (std::max)(n, 1);
    return n;
}

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
              int outH, int outW, bool useAvx2, int maxThreads)
{
    // im2col: cols[row][opos] = src[ci, id, ih, iw] (範囲外は 0)。
    // row = ((ci*k+kd)*k+kh)*k+kw は weight [cout,cin,k,k,k] の後半 4 軸と同じ並びなので、
    // weight をそのまま A[cout][K] として GEMM に渡せる (reshape 不要)
    const int kk = k;
    const size_t bigK = static_cast<size_t>(cin) * kk * kk * kk;
    const size_t bigN = static_cast<size_t>(outD) * outH * outW;
    std::vector<float> cols(bigK * bigN, 0.0f);

    // 行 (= (ci,kd,kh,kw) を 1 本の index に潰したもの) ごとに書き込み先が独立なので、
    // 行のレンジで分割するだけでリダクション無しに並列化できる (sub-09)。
    // row=0..bigK-1 を昇順に辿る限り (ci,kd,kh,kw) の出現順は元の 4 重ループと 1 対 1 で
    // 同じなので、maxThreads<=1 (既定) のときは sub-05 時点と 1 命令も違わない
    auto fillRows = [&](size_t rowBegin, size_t rowEnd) {
        for (size_t row = rowBegin; row < rowEnd; ++row) {
            const int kw = static_cast<int>(row % static_cast<size_t>(kk));
            const int kh = static_cast<int>((row / static_cast<size_t>(kk)) % static_cast<size_t>(kk));
            const int kd = static_cast<int>((row / (static_cast<size_t>(kk) * kk)) % static_cast<size_t>(kk));
            const int ci = static_cast<int>(row / (static_cast<size_t>(kk) * kk * kk));
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
                    const float* srcRow = src + ((static_cast<size_t>(ci) * d + id) * h + ih) * w;
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
    };
    ParallelSpan(bigK, maxThreads, fillRows);

    GemmBiasAddDispatch(weight, cols.data(), bias, dst, cout, static_cast<int>(bigN),
                        static_cast<int>(bigK), useAvx2, maxThreads);
}

void ConvTranspose3dRaw(const float* src, int cin, int d, int h, int w, const float* weight,
                        const float* bias, int cout, int k, int stride, int pad, int outPad,
                        float* dst, int outD, int outH, int outW, bool useAvx2, int maxThreads)
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
    // ci ごとに読み書き先が完全に独立 (padded は [cin,pD,pH,pW] の C 順) なので、
    // ci のレンジで分割するだけでリダクション無しに並列化できる
    auto fillPadded = [&](size_t ciBegin, size_t ciEnd) {
        for (size_t ciz = ciBegin; ciz < ciEnd; ++ciz) {
            const int ci = static_cast<int>(ciz);
            for (int id = 0; id < d; ++id) {
                const int pd = padLow + id * stride;
                for (int ih = 0; ih < h; ++ih) {
                    const int ph = padLow + ih * stride;
                    const float* srcRow = src + ((static_cast<size_t>(ci) * d + id) * h + ih) * w;
                    float* dstBase = padded.data()
                        + ((static_cast<size_t>(ci) * pD + pd) * pH + ph) * pW + padLow;
                    for (int iw = 0; iw < w; ++iw) {
                        dstBase[static_cast<size_t>(iw) * stride] = srcRow[iw];
                    }
                }
            }
        }
    };
    ParallelSpan(static_cast<size_t>(cin), maxThreads, fillPadded);

    // 重みの反転並べ替えは O(cin*cout*k^3) で、対応する畳み込み本体 (この並べ替えられた
    // 重みを outD*outH*outW 回使い回す GEMM) に比べて無視できるほど小さい
    // (k^3 要素の並べ替え 1 回 vs 同じ k^3 要素を N=outD*outH*outW 回読む GEMM) ので、
    // 並列化・キャッシュのしがいが無いと判断しシングルスレッドのまま据え置く
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
             /*pad=*/0, dst, outD, outH, outW, useAvx2, maxThreads);
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

int CpuModalBackend::EffectiveThreadCount() const
{
    if (threadOverride_ >= 0) {
        return threadOverride_; // 0 = 強制直列 (テストで「スレッド数を変えても一致」を確認する口)
    }
    // `--modal-bake` を 2 回、スレッド数だけ変えて再入するための計測用の口 (CLI フラグは
    // 増やさない。ModalSoundLibrary は ForceScalar/ThreadCountOverride を配線していないので、
    // ここが唯一の外部からの調整点)
    if (const char* env = std::getenv("MYE_MODAL_THREADS")) {
        const int n = std::atoi(env);
        if (n >= 0) {
            return n;
        }
    }
    return ModalCpuDefaultThreadCount();
}

bool CpuModalBackend::UsingAvx2() const
{
    if (forceScalar_) {
        return false;
    }
    if (const char* env = std::getenv("MYE_MODAL_FORCE_SCALAR")) {
        if (env[0] == '1') {
            return false;
        }
    }
    return ModalCpuDetectAvx2Fma();
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

    const bool useAvx2 = UsingAvx2();
    const int threads = EffectiveThreadCount();

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
            ParallelSpan(src.data.size(), threads, [&](size_t b, size_t e) {
                ReluRange(src.data.data(), dst.data.data(), b, e, useAvx2);
            });
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
            ParallelSpan(src.data.size(), threads, [&](size_t b, size_t e) {
                AddRange(src.data.data(), src1.data.data(), dst.data.data(), b, e, useAvx2);
            });
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
                     op.cout, op.k, op.stride, op.pad, dst.data.data(), outD, outH, outW, useAvx2,
                     threads);
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
                              dst.data.data(), outD, outH, outW, useAvx2, threads);
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
