#pragma once
#include "Engine/Core/Diagnostics/Profiler.h"
#include "Engine/Engine/Loop/EngineLoop.h"

namespace mye {

struct EditorSettings;
class ToastCenter;

// プロファイラ (engine_spec.md 9 章、M102 で作り直し)。
// 上の帯 (フレーム時間の要約 + 一時停止 + 表示の設定) は常に出し、その下をタブで分ける:
//   概要 = 積み上げフレームグラフと重い順の一覧 / CPU = スコープの木 / GPU = パス別 /
//   描画 = ドローコール・カリング・LOD・影 / システム = 機能別の現在値 /
//   記録 = しきい値を超えた重いフレームの控え (選ぶと全タブがそのフレームの内容になる)。
// 表示するのは prof の「確定済みフレーム」だけ (フレームの途中の値を混ぜない)。
// 一時停止は prof の履歴そのものを止めるので、グラフ・平均・選んだフレームが同じ時点で揃う
class ProfilerWindow {
public:
    bool open = true; // 閉じる / 再表示 (タブ [x] と Window メニューに連動)
    // 窓を閉じていても、重いフレームの記録と予算超過の通知は続ける (見ていないときにこそ要る)
    void OnImGui(EngineContext& ctx, EditorSettings& settings, ToastCenter& toasts);

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
    void DrawCpu(EditorSettings& settings);
    void DrawCpuChildren(int parent, EditorSettings& settings);
    // 名前の列より右 (今回 / 平均 / 最大 / 自身 / 回数 / 予算 / 割合)
    void DrawCpuRowValues(int scope, const EditorSettings& settings);
    void DrawGpu(EditorSettings& settings);
    void DrawRender(EngineContext& ctx);
    void DrawSystems(EngineContext& ctx);
    void DrawHistory(EditorSettings& settings);
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

    PhaseStats phases_;
    RowStats scopeStats_[prof::kMaxFrameScopes];
    float scopeSelfAvg_[prof::kMaxFrameScopes] = {}; // 平均から子の平均を引いた「自身」の時間
    RowStats gpuStats_[prof::kMaxGpuPasses];
};

} // namespace mye
