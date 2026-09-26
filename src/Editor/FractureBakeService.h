//====================================================================================
//                          FractureBakeService.h
//  MyEngin/ 秋田蓮音                                                     09/25/2026
//                                          Inspector 用の破片焼き非同期ワーカー (M80i)
//====================================================================================
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Engine/Engine/Physics/FractureBake.h"
#include "Engine/Engine/Physics/FractureMesh.h"
#include "Engine/Engine/Physics/FractureSkinBake.h" // M80j: スキンの骨割り当て入力

namespace mye {

// Destructible の「破片を生成」ボタンが積む焼き 1 件分の入力。sourceMesh は要求時点で
// メインスレッドがコピー済み (ワーカーは MeshLibrary/RenderResources に触れない、
// ModalSoundLibrary と同じ規約)
struct FractureBakeRequest {
    FractureMesh sourceMesh;
    uint64_t sourceMeshHash = 0; // .mfrac の FractureData::sourceMeshHash
    uint32_t seed = 1;
    int32_t pieceCount = 16;
    int32_t openMeshMode = 0;
    int32_t voxelResolution = 32;
    // M80j: スキン破壊。skinJoints が非空なら sourceMesh.verts と同じ並びのウェイトを渡す
    // (骨割り当て・骨空間への変換は WorkerLoop 内で焼きの後処理として行う)
    std::vector<FractureSkinVertex> skinVertices;
    std::vector<FractureSkinJoint> skinJoints;
};

// 要求 1 件の状態。id ごとに独立 (Destructible の tg.fid をキーに使う想定)
enum class FractureBakeJobState : uint8_t {
    None,   // 要求なし、または結果を取り出し済み
    Baking, // ワーカーで処理中 (キュー待ちも含む)
    Ready,  // 結果を取り出せる (TakeResult で消費すると None に戻る)
};

// エディタ専用の非同期焼きワーカー (ModalSoundLibrary.h と同型: 単一ワーカースレッド +
// mutex/cv の queue)。焼きは Engine 層の純関数 BakeFracture そのもの — ここは
// スレッド境界とキューの世話だけをする。`.mfrac` の書き出し・AssetDatabase 登録・
// FractureLibrary 登録・BuildFracturePieces は呼び出し側 (InspectorWindow、メインスレッド)
// の責務 (CookedCache 等と同じく非スレッドセーフな資産まわりはメインスレッドだけが触る)
class FractureBakeService {
public:
    ~FractureBakeService();

    // 同じ id が Baking 中なら無視 (二重投入防止)。Ready な未取得の結果があれば置き換える
    void Request(uint64_t id, FractureBakeRequest request);

    FractureBakeJobState GetState(uint64_t id) const;
    // Baking 中の現在の段階 (ワーカーは 1 度に 1 件しか処理しないので、id が「今処理中の
    // ジョブ」と一致するときだけ意味を持つ。一致しない = キュー待ち中とみなし ClosedCheck を返す)
    FractureBakeStage GetStage(uint64_t id) const;

    // 取り消し (M80p)。id が今処理中のジョブなら旗を立てて BakeFracture の段階の合間・
    // セル切断ループの合間で打ち切らせる (有限時間で Ready になり、result.cancelled==true)。
    // まだキュー待ちなら、始めてすらいないのでその場でキューから外して None に戻す。
    // どちらでもなければ何もしない (Baking 中でない id を渡しても安全)
    void Cancel(uint64_t id);

    // Ready な結果を取り出す (呼ぶと内部エントリは消え None に戻る)。Ready でなければ false。
    // boneNamesOut (M80j): スキン破壊のとき bake.pieces と同じ並びの割り当て骨名。非スキンは空
    bool TakeResult(uint64_t id, FractureBakeRequest& requestOut, FractureBakeResult& resultOut,
                    std::vector<std::string>& boneNamesOut);

    void Pump(); // メインスレッド: ワーカー結果を毎フレーム取り込む
    void Shutdown();

private:
    struct Job {
        uint64_t id = 0;
        FractureBakeRequest request;
    };
    struct JobResult {
        uint64_t id = 0;
        FractureBakeRequest request;
        FractureBakeResult result;
        std::vector<std::string> pieceBoneNames; // M80j。非スキンは空
    };
    struct Entry {
        FractureBakeJobState state = FractureBakeJobState::None;
        FractureBakeRequest request;
        FractureBakeResult result; // state==Ready のときだけ意味を持つ
        std::vector<std::string> pieceBoneNames; // M80j。result と同じ並び
    };

    void EnsureWorker();
    void WorkerLoop();
    // BakeFractureInput::progress から呼ばれる (ワーカースレッド上、同期呼び出し)
    static void OnBakeProgress(FractureBakeStage stage, void* userData);

    std::unordered_map<uint64_t, Entry> entries_; // メインスレッド専用

    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> jobQueue_;
    std::deque<JobResult> doneQueue_;
    bool workerStarted_ = false;
    bool workerStop_ = false;

    // 現在ワーカーが処理中のジョブの id と段階 (GetStage 用)。ワーカースレッドが書き、
    // メインスレッドが読むだけなので atomic にする (BakeFracture の progress コールバックは
    // ワーカースレッド上で同期的に呼ばれる)
    std::atomic<uint64_t> currentJobId_{ 0 };
    std::atomic<bool> hasCurrentJob_{ false };
    std::atomic<int32_t> currentStage_{ 0 };

    // 現在処理中のジョブへの取り消し要求 (M80p)。WorkerLoop が次のジョブを取り出すたびに
    // false へ戻す (前のジョブの取り消しが次のジョブへ持ち越されないように)
    std::atomic<bool> cancelRequested_{ false };
};

} // namespace mye
