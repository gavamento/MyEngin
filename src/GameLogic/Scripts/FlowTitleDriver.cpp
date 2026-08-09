// flow 統合デモ (M51j) のタイトル/リザルト画面ドライバ。
//
// --flow-demo のタイトルシーン (assets\scenes\flow_title.scene.json) に付き、
// リプレイ検証 (replay_verify 3 ペア目) で毎回走る。M51 のゲームフロー系を
// **リプレイ被覆に入れる**ための恒久 probe を兼ねる:
//   - PersistStore (v12): BEST/LAST/RUNS を読んで登録フィールドへ書き戻す
//     (フィールドは hash 対象 — シーンを跨いだ持ち越し値が構成間でズレたら即 divergence)
//   - アクションマップ (M51d): "Jump" pressed で開始 (ヘッドレス記録では未押下 = 純 tick 進行)
//   - LoadScene: tick 決定の自動開始 (90 tick) → 記録/検証とも同一 tick で遷移する
// UI 文字列の書き込み (SetUIText) は演出レーン (UIElement は NoHash) なので何を書いても
// リプレイ不変 — 表示とハッシュ被覆を分けるのがこのデモの流儀。
#include "Shared/ScriptAPI.h"

struct FlowTitleDriver : Script<FlowTitleDriver> {
    int32_t ticksInScene = 0; // シーン内経過 tick (LoadScene でファイル値 0 に戻る)
    int32_t lastBest = 0;     // persist "flow.best" のミラー (hash 被覆の本体)
    int32_t lastScore = 0;    // persist "flow.last" のミラー
    int32_t lastRuns = 0;     // persist "flow.runs" のミラー

    void Update(MyeUpdateContext& ctx)
    {
        ++ticksInScene;

        // ---- persist → 登録フィールド (sim 状態への書き戻し = リプレイ被覆) ----
        lastBest = MyePersistGetInt(ctx, "flow.best", 0);
        lastScore = MyePersistGetInt(ctx, "flow.last", 0);
        lastRuns = MyePersistGetInt(ctx, "flow.runs", 0);

        // ---- リザルト表示 (演出レーン。ヘッドレスでは UI 実体が無くても no-op で安全) ----
        const MyeEngineApi* api = ctx.api;
        char buf[128];
        snprintf(buf, sizeof(buf), "BEST %d   LAST %d   RUNS %d", lastBest, lastScore, lastRuns);
        api->SetUIText(api->engine, api->FindByName(api->engine, "TitleBest"), buf);

        // ---- 開始: "Jump" アクション (対話) または 90 tick (自動デモ = 決定論) ----
        if (MyeActionPressed(ctx, "Jump") || ticksInScene == 90) {
            MyePersistSetValue(ctx, "flow.runs", lastRuns + 1);
            MyePersistSetValue(ctx, "flow.resume", 0); // タイトル経由の開始はスコア 0 から
            api->LoadScene(api->engine, "scenes/flow_game.scene.json");
        }
    }
};
REGISTER_SCRIPT(FlowTitleDriver, FIELDS(ticksInScene, lastBest, lastScore, lastRuns));
