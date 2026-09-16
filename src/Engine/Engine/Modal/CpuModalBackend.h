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

// 実行時 AVX2+FMA 検出 (__cpuid/__cpuidex/_xgetbv、結果はプロセス内でキャッシュ)。
// ★グローバルの /arch は SSE2 のまま (build\Common.props) — ここが true を返す機種だけが
//   Conv3dRaw/ConvTranspose3dRaw の useAvx2=true 経路を実際に使う。false の機種では
//   useAvx2=true を渡してもスカラー経路へ自動縮退する (呼び出し側は常に true を渡してよい)
bool ModalCpuDetectAvx2Fma();
// 既定のワーカースレッド数 (min(4, max(1, hardware_concurrency-1))。焼きはワーカースレッドの
// 裏で走るので全コアは使わない — ゲームの他のスレッドを止めないための上限)
int ModalCpuDefaultThreadCount();

// 生の Conv3d / ConvTranspose3d カーネル (im2col + 4x4 ブロック GEMM)。
// 入出力は [C, D, H, W] (W が最内。VoxelGrid / モデル出力と同じ x-最内規約) の平坦配列。
// weight は Conv3d が [cout,cin,k,k,k]、ConvTranspose3d が [cin,cout,k,k,k]
// (PyTorch のテンソル形状そのまま、C 順)。dst は呼び出し側が out*outD*outH*outW 分確保する
// (spec §5 受け入れ条件 11: 素朴 6 重ループとの照合に selftest が直接叩く低レベル API)。
//
// useAvx2/maxThreads (sub-09、M76e2) は末尾に追加した最適化スイッチ。
// ★既定値 (false, 0) は sub-05 時点の挙動 (単一スレッド・SIMD 無し) と**完全に同じ計算順**
//   になるようにしてある — ModalSelfTest の素朴 6 重ループ照合 (1e-6 の厳しい許容) は
//   この既定値のまま呼んでいるので影響を受けない。CpuModalBackend::Infer は明示的に
//   useAvx2=UsingAvx2(), maxThreads=EffectiveThreadCount() を渡して呼ぶ。
// ★分割は出力側 (GEMM の N 次元 / im2col の行) だけで、リダクション (K 次元の総和順) は
//   1 スレッドが 0..K-1 を固定順で計算しきる。maxThreads をいくつに変えても
//   同じ入力からは同じビット列が出る (spec §5 受け入れ条件 23)。
// ★useAvx2=true でもハードウェアが対応していなければ ModalCpuDetectAvx2Fma() が false を
//   返すのでスカラーへ自動縮退する。
void Conv3dRaw(const float* src, int cin, int d, int h, int w, const float* weight,
              const float* bias, int cout, int k, int stride, int pad, float* dst, int outD,
              int outH, int outW, bool useAvx2 = false, int maxThreads = 0);
void ConvTranspose3dRaw(const float* src, int cin, int d, int h, int w, const float* weight,
                        const float* bias, int cout, int k, int stride, int pad, int outPad,
                        float* dst, int outD, int outH, int outW, bool useAvx2 = false,
                        int maxThreads = 0);

// CPU 推論バックエンド。RunsOnWorkerThread() == true (ModalSoundLibrary の非同期焼き
// ワーカースレッドで Infer() が呼ばれる。--modal-bake の BakeSync は別に main スレッドから
// 直接呼ぶが、両者が同じインスタンスに対して同時に走ることは無い — ModalSoundLibrary の
// ジョブキューは 1 度に 1 件しか処理しない設計なので、このクラス自身は再入を想定しない
// [jobs::JobSystem と同じ「常に高々 1 呼び出し」前提。CpuModalBackend.cpp 冒頭のコメント参照])
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

    // 計測 / selftest 用: AVX2 対応機でもスカラー経路を強制する (両経路が腐っていないかを
    // selftest から両方通すためのスイッチ、spec §5 受け入れ条件 23)。既定 false = 実行時検出に従う。
    // 明示呼び出しが無ければ環境変数 `MYE_MODAL_FORCE_SCALAR=1` も見る (`--modal-bake` を
    // CLI から使い分けるための口。CLI フラグを増やさずに済ませる — 計測専用で結果の意味を
    // 変えない値なので project_settings.json 相当の永続化は不要と判断した)
    void SetForceScalar(bool force)
    {
        forceScalar_ = force;
    }
    // 計測用: ワーカースレッド数を上書きする (0 = 強制直列、負値 = 既定 (ModalCpuDefaultThreadCount()、
    // または環境変数 `MYE_MODAL_THREADS`) を使う)。出力側だけを分割する設計なので、
    // ここを変えても .msfm はバイト一致する (spec §5 受け入れ条件 23 の
    // 「スレッド数を変えても一致」を `--modal-bake` から検証する口)
    void SetThreadCountOverride(int n)
    {
        threadOverride_ = n;
    }
    int EffectiveThreadCount() const;
    // 実際に AVX2 経路を使うか (forceScalar_ が立っていない && ハードウェアが対応)
    bool UsingAvx2() const;

private:
    struct PreparedOp {
        DmNetOp op;
        std::vector<float> weight; // Conv3d/ConvTranspose3d のみ (ReLU/Add は空)
        std::vector<float> bias;
    };
    DmNetHeader header_;
    std::vector<PreparedOp> ops_;
    uint32_t bufferCount_ = 0;
    bool forceScalar_ = false;
    int threadOverride_ = -1;
};

} // namespace mye
