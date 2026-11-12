#pragma once
#include "Engine/Core/Diagnostics/Profiler.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Engine/Loop/EngineLoop.h"

namespace mye {

struct EditorSettings;
struct Selection;
class ToastCenter;

// プロファイラ (engine_spec.md 9 章、M102 で作り直し)。
// 上の帯 (フレーム時間の要約 + 一時停止 + 表示の設定) は常に出し、その下をタブで分ける:
//   概要 = 積み上げフレームグラフと重い順の一覧 / CPU = スコープの木 / GPU = パス別 /
//   描画 = ドローコール・カリング・LOD・影 / メモリ = 分類別の確保と GPU メモリ /
//   アセット = 読み込みの所要時間と待ち行列 / ジョブ = ワーカー別の稼働 / システム = 機能別の現在値 /
//   記録 = しきい値を超えた重いフレームの控え (選ぶと全タブがそのフレームの内容になる)。
// 表示するのは prof の「確定済みフレーム」だけ (フレームの途中の値を混ぜない)。
// 一時停止は prof の履歴そのものを止めるので、グラフ・平均・選んだフレームが同じ時点で揃う
class ProfilerWindow {
public:
    bool open = true; // 閉じる / 再表示 (タブ [x] と Window メニューに連動)
    // 窓を閉じていても、重いフレームの記録と予算超過の通知は続ける (見ていないときにこそ要る)
    void OnImGui(EngineContext& ctx, EditorSettings& settings, ToastCenter& toasts, Selection& selection);

private:
    // 行ごとの「今回 / 平均 / 最大」。平均と最大は表示中フレームから過去へ window フレームぶん
    struct RowStats {
        float now = 0.0f;
        float avg = 0.0f;
        float max = 0.0f;
    };
    // フレームの壁時計の内訳 (平均)。other = frame - (他の和)
    struct PhaseStats {
        RowStats frame;
        RowStats reload;
        RowStats tick;
        RowStats render;
        RowStats imgui;
        RowStats present;
        RowStats gpuFrame;
    };

    void BuildStats(int window);
    void DrawHeader(EditorSettings& settings);
    void DrawFrameGraph();
    void DrawOverview(const EditorSettings& settings);
    void DrawCpu(EngineContext& ctx, EditorSettings& settings, Selection& selection);
    void DrawScopes(EditorSettings& settings); // CPU タブの上段: スコープの木
    // CPU タブの下段: スクリプト・システム別の合計と、エンティティ別の重い順
    void DrawItems(EngineContext& ctx, EditorSettings& settings, Selection& selection);
    void DrawCpuChildren(int parent, EditorSettings& settings);
    // 名前の列より右 (今回 / 平均 / 最大 / 自身 / 回数 / 予算 / 割合)
    void DrawCpuRowValues(int scope, const EditorSettings& settings);
    void DrawGpu(EditorSettings& settings);
    void DrawRender(EngineContext& ctx);
    void DrawSystems(EngineContext& ctx);
    void DrawHistory(EditorSettings& settings);
    void DrawMemory(EngineContext& ctx, const EditorSettings& settings);
    void DrawGpuMemory(EngineContext& ctx); // メモリタブの下段
    void DrawAssets(EngineContext& ctx, const EditorSettings& settings);
    void DrawJobs(const EditorSettings& settings);
    // ジョブの累計統計の前フレームとの差を積む (毎フレーム 1 回)
    void UpdateJobHistory();
    // 累計カウンタの前フレームとの差を取り、フレームあたりの確保として積む (毎フレーム 1 回)
    void UpdateMemoryHistory();
    void CheckBudgets(const EditorSettings& settings, ToastCenter& toasts);
    // 予算 [ms]。kind は "cpu" / "gpu"。未設定は 0
    static float BudgetMs(const EditorSettings& settings, const char* kind, const char* name);
    // 直前に描いた行の右クリックで予算を編集するポップアップ (行ごとに PushID 済みで呼ぶ)
    static void BudgetPopup(EditorSettings& settings, const char* kind, const char* name);
    bool PassesFilter(const char* name) const;
    const prof::FrameRecord& Shown() const
    {
        return (captureIndex_ >= 0) ? prof::Capture(captureIndex_) : prof::History(selectedAge_);
    }

    int selectedAge_ = 0;     // 表示中のフレーム (0 = 直近)。一時停止中だけ 0 以外になる
    // 表示中の控え (-1 = 履歴を表示)。控えを見ている間は履歴を止める — 止めないと新しい控えが
    // 入ってリングの添字がずれる
    int captureIndex_ = -1;
    double lastBudgetToast_ = -1.0e9; // 予算超過を最後に通知した ImGui 時刻 [s]
    bool restoreTab_ = true;  // 初回だけ、保存してあるタブを選び直す
    int renderView_ = 0;      // 描画タブのビュー (0 = 全ビュー、1.. = viewKey)
    char filter_[64] = {};    // 名前の絞り込み (保存しない)

    // フレームあたりの確保 (分類別)。prof の履歴と同じ長さのリング
    struct MemFrame {
        uint32_t allocs[prof::kMemTagCount] = {};
        uint64_t bytes[prof::kMemTagCount] = {};
    };
    MemFrame memHistory_[prof::kFrameHistory];
    int memHead_ = 0;  // 次に書く位置
    int memCount_ = 0;
    prof::MemStats memPrevious_;
    bool memPreviousValid_ = false;

    // フレームあたりのジョブの時間 [ms]。行 = ワーカー 0..15、呼出スレッドの実行、呼出スレッドの待ち
    static constexpr int kJobRows = jobs::JobSystem::kMaxWorkers + 2;
    static constexpr int kJobRowCallerBusy = jobs::JobSystem::kMaxWorkers;
    static constexpr int kJobRowCallerWait = jobs::JobSystem::kMaxWorkers + 1;
    struct JobFrame {
        float ms[kJobRows] = {};
        uint32_t batches = 0;
        uint32_t workerChunks = 0;
    };
    JobFrame jobHistory_[prof::kFrameHistory];
    int jobHead_ = 0; // 次に書く位置
    int jobCount_ = 0;
    jobs::JobSystem::TimingStats jobPreviousTiming_;
    jobs::JobSystem::Stats jobPreviousStats_;
    bool jobPreviousValid_ = false;

    PhaseStats phases_;
    RowStats scopeStats_[prof::kMaxFrameScopes];
    float scopeSelfAvg_[prof::kMaxFrameScopes] = {}; // 平均から子の平均を引いた「自身」の時間
    RowStats gpuStats_[prof::kMaxGpuPasses];
};

} // namespace mye
