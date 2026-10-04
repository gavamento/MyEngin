//====================================================================================
//                          NavBakeService.h
//  MyEngin/ 秋田蓮音                                                     10/03/2026
//                                          Inspector 用のナビメッシュ・ベイクの非同期ワーカー (M82b)
//====================================================================================
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "Engine/Engine/Navigation/NavBake.h"

namespace mye {

enum class NavBakeJobState : uint8_t {
    None,   // 要求なし、または結果を取り出し済み
    Baking, // キュー待ちまたはワーカーで処理中
    Ready,  // TakeResult で取り出せる
};

// 取り出した結果。inputs はベイクの入力 (保存名の元になるハッシュは output.data.inputHash)
struct NavBakeResult {
    NavBakeOutput output;
    double elapsedMs = 0.0;
};

// エディタ専用の非同期ベイクワーカー (FractureBakeService と同型: 単一ワーカー + mutex / cv のキュー)。
// ワーカーは NavBakeAsset だけを呼び、World にも ImGui にも触れない。入力の収集 (NavPrepareBakeInputs) は
// 呼び出し側がメインスレッドで済ませてから Request する。.mnav の保存と参照の設定は NavBakeCommit.h
class NavBakeService {
public:
    ~NavBakeService();

    // 同じ id が Baking 中なら無視 (二重投入防止)。Ready な未取得の結果があれば置き換える
    void Request(uint64_t id, NavBakeInputs inputs);

    NavBakeJobState GetState(uint64_t id) const;
    // Baking 中のタイル進捗。ジョブが無ければ false
    bool GetProgress(uint64_t id, int& done, int& total) const;
    // 打ち切りを要求する (有限時間で Ready になり、結果の status が Cancelled になる)
    void Cancel(uint64_t id);
    // Ready な結果を取り出す (呼ぶと None に戻る)
    bool TakeResult(uint64_t id, NavBakeResult& out);
    // Ready な結果を持つ id (昇順)。選択に関係なく結果を確定するための走査用
    std::vector<uint64_t> ReadyIds() const;

    void Shutdown();

private:
    struct Job {
        NavBakeInputs inputs;
        NavBakeControl control;
        NavBakeResult result;
        NavBakeJobState state = NavBakeJobState::Baking; // mutex_ の下で読み書きする
    };

    void EnsureWorker();
    void WorkerLoop();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::map<uint64_t, std::shared_ptr<Job>> jobs_;
    std::deque<uint64_t> queue_;
    std::thread worker_;
    bool workerStarted_ = false;
    bool workerStop_ = false;
};

} // namespace mye
