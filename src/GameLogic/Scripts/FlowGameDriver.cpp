// flow 統合デモ (M51j) のゲーム画面ドライバ。Ball (Rigidbody 持ち) に付く。
//
// M51 のゲームフロー一式を 1 本の tick タイムラインで実走する恒久 probe:
//   tick 120-180 : ポーズ (M51g tick ゲート — 物理/アニメの凍結が hash に載る)
//   tick 240-320 : タイムスケール 50% (1 tick おきに物理が進む)
//   tick 360     : persist へスコア確定 + SaveGame(0) (書出は出力レーン = record 中も走る)
//   tick 430     : BEST/LAST を persist へ → タイトルへ LoadScene (シーン跨ぎ持ち越し)
// スコアは衝突コールバック (バウンド +10) と 12 tick 毎の +1 で決定論的に増える。
// 登録フィールドは hash 対象 — 構成間 (Debug/Release) でズレたら即 divergence。
//
// 対話系 (リプレイでは入力ゼロ = no-op):
//   MoveX 軸でパドル移動 / "Pause" でポーズ切替 / "Fire" で LoadGame(0)
//   (LoadGame は record/verify 中 no-op が仕様 — リプレイはセーブ読込を跨がない)
#include "Shared/ScriptAPI.h"

struct FlowGameDriver : Script<FlowGameDriver> {
    int32_t ticksInScene = 0; // シーン内経過 tick (ポーズ中も進む — スクリプトは非ゲート)
    int32_t score = 0;        // hash 被覆の本体
    int32_t bounces = 0;      // 衝突コールバック経由の加点回数

    void Start(MyeUpdateContext& ctx)
    {
        // セーブからの再開 (LoadGame 後の再ロード時のみ resume=1)。タイトル経由は 0
        if (MyePersistGetInt(ctx, "flow.resume", 0) != 0) {
            score = MyePersistGetInt(ctx, "flow.score", 0);
            MyePersistSetValue(ctx, "flow.resume", 0);
        }
    }

    void OnCollisionEnter(MyeUpdateContext& ctx, MyeEntityId other, MyeVec3 normal)
    {
        (void)ctx;
        (void)other;
        (void)normal;
        score += 10; // バウンド加点 (物理イベント → sim 状態への決定論的な反映)
        ++bounces;
    }

    void Update(MyeUpdateContext& ctx)
    {
        ++ticksInScene;
        const MyeEngineApi* api = ctx.api;

        // ---- パドル移動 (対話のみ。リプレイでは軸 0 = 完全 no-op) ----
        const float ax = MyeAxis(ctx, "MoveX");
        if (ax != 0.0f) {
            const MyeEntityId paddle = api->FindByName(api->engine, "Player");
            if (!MyeEntityIdIsNull(paddle)) {
                MyeVec3 p;
                api->GetLocalPosition(api->engine, paddle, &p);
                p.x += ax * 6.0f * ctx.dt;
                p.x = p.x < -5.0f ? -5.0f : (p.x > 5.0f ? 5.0f : p.x);
                api->SetLocalPosition(api->engine, paddle, p);
            }
        }

        // ---- 加点 (トリクル): ポーズ中は sim が止まって見えるのでスコアも止める ----
        if (!MyeIsPaused(ctx) && (ticksInScene % 12) == 0) {
            ++score;
        }

        // ---- 自動デモのタイムライン (tick 決定 = 記録/検証で同一) ----
        if (ticksInScene == 120) {
            MyeSetPaused(ctx, true);
        }
        if (ticksInScene == 180) {
            MyeSetPaused(ctx, false);
        }
        if (ticksInScene == 240) {
            MyeSetTimeScale(ctx, 50);
        }
        if (ticksInScene == 320) {
            MyeSetTimeScale(ctx, 100);
        }
        if (ticksInScene == 360) {
            MyePersistSetValue(ctx, "flow.score", score);
            MyePersistSetValue(ctx, "flow.resume", 1); // このセーブを読んだ実行は続きから
            MyeSaveGame(ctx, 0);
            MyePersistSetValue(ctx, "flow.resume", 0); // ライブ側はタイトル経由に戻す
        }
        if (ticksInScene == 430) {
            const int32_t best = MyePersistGetInt(ctx, "flow.best", 0);
            if (score > best) {
                MyePersistSetValue(ctx, "flow.best", score);
            }
            MyePersistSetValue(ctx, "flow.last", score);
            api->LoadScene(api->engine, "scenes/flow_title.scene.json");
        }

        // ---- 対話: ポーズ切替 / セーブ読込 (リプレイでは未押下/no-op) ----
        if (MyeActionPressed(ctx, "Pause")) {
            MyeSetPaused(ctx, !MyeIsPaused(ctx));
        }
        if (MyeActionPressed(ctx, "Fire")) {
            MyeLoadGame(ctx, 0);
        }

        // ---- HUD (演出レーン — UIElement は NoHash なので何を書いてもリプレイ不変) ----
        char buf[64];
        snprintf(buf, sizeof(buf), "SCORE %d", score);
        api->SetUIText(api->engine, api->FindByName(api->engine, "GameScore"), buf);
        const float fill = score >= 300 ? 1.0f : static_cast<float>(score) / 300.0f;
        api->SetUIFill(api->engine, api->FindByName(api->engine, "GameScoreBar"), fill);
        const MyeEntityId pauseUi = api->FindByName(api->engine, "GamePause");
        api->SetUIColor(api->engine, pauseUi,
                        MyeColor{ 1.0f, 0.85f, 0.30f, MyeIsPaused(ctx) ? 1.0f : 0.0f });
    }
};
REGISTER_SCRIPT(FlowGameDriver, FIELDS(ticksInScene, score, bounces));
