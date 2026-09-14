// flow 統合デモ (M51j) のタイトル/リザルト画面ドライバ。
//
// --flow-demo のタイトルシーン (assets\scenes\flow_title.scene.json) に付き、
// リプレイ検証 (replay_verify 3 ペア目) で毎回走る。M51 のゲームフロー系を
// **リプレイ被覆に入れる**ための恒久 probe を兼ねる:
//   - PersistStore (v12): BEST/LAST/RUNS を読んで登録フィールドへ書き戻す
//     (フィールドは hash 対象 — シーンを跨いだ持ち越し値が構成間でズレたら即 divergence)
//   - LoadScene: tick 決定の自動開始 (90 tick) → 記録/検証とも同一 tick で遷移する
//   - **UI の対話 (M70c)**: START / CLEAR BEST の 2 ボタンを MyeUIClicked で読む。
//     矩形はエンジンが解決し、押下もフォーカスもエンジンが持つ = スクリプト側に
//     矩形の写しが 1 つも無い。合成入力の D-Pad + A がここを毎回通る
//     (replay_verify の flow ペアが「エンジンがフォーカスを持つ」配線の唯一の検査)
// UI 文字列の書き込み (SetUIText) は演出レーン (UIElement は NoHash) なので何を書いても
// リプレイ不変 — 表示とハッシュ被覆を分けるのがこのデモの流儀。
#include "Shared/ScriptAPI.h"

struct FlowTitleDriver : Script<FlowTitleDriver> {
    int32_t ticksInScene = 0; // シーン内経過 tick (LoadScene でファイル値 0 に戻る)
    int32_t lastBest = 0;     // persist "flow.best" のミラー (hash 被覆の本体)
    int32_t lastScore = 0;    // persist "flow.last" のミラー
    int32_t lastRuns = 0;     // persist "flow.runs" のミラー
    // M70c: CLEAR BEST を押した回数。**登録フィールド = ハッシュ対象**なので、
    // 「エンジンのクリック判定がスクリプトへ届いたか」がリプレイの照合対象になる
    int32_t clearClicks = 0;
    // v17 (M71a): 今いるシーン名のハッシュ下位 31bit。**登録フィールド = ハッシュ対象**
    // なので、GetSceneName が記録と検証で同じ値を返すことがリプレイの照合対象になる
    // (persist を登録フィールドへ書き戻しているのと同じ作法)。flow ペアは 2 シーンを
    // 行き来する唯一の検査なので、遷移の前後で名前が入れ替わることもここに載る
    int32_t sceneTag = 0;

    void Update(MyeUpdateContext& ctx)
    {
        ++ticksInScene;
        {
            char sceneName[64] = {};
            MyeGetSceneName(ctx, sceneName, static_cast<int32_t>(sizeof(sceneName)));
            sceneTag = static_cast<int32_t>(MyeNameHash(sceneName) & 0x7FFFFFFFull);
        }

        // ---- persist → 登録フィールド (sim 状態への書き戻し = リプレイ被覆) ----
        lastBest = MyePersistGetInt(ctx, "flow.best", 0);
        lastScore = MyePersistGetInt(ctx, "flow.last", 0);
        lastRuns = MyePersistGetInt(ctx, "flow.runs", 0);

        // ---- リザルト表示 (演出レーン。ヘッドレスでは UI 実体が無くても no-op で安全) ----
        const MyeEngineApi* api = ctx.api;
        char buf[128];
        snprintf(buf, sizeof(buf), "BEST %d   LAST %d   RUNS %d", lastBest, lastScore, lastRuns);
        api->SetUIText(api->engine, api->FindByName(api->engine, "TitleBest"), buf);

        // ---- 入ったら START にフォーカスを置く ----
        // ★フォーカス不在だと UINavSubmit が何も指さないので、パッドだけでは 1 手も
        //   進めない。「メニューに入ったら既定の項目を選んでおく」は UI の作法そのもの
        const MyeEntityId startBtn = api->FindByName(api->engine, "TitleStart");
        if (ticksInScene == 1) {
            MyeUISetFocused(ctx, startBtn);
        }

        // ---- CLEAR BEST: ハイスコアだけ消す (シーンは動かない) ----
        const MyeEntityId clearBtn = api->FindByName(api->engine, "TitleClearBest");
        if (MyeUIClicked(ctx, clearBtn)) {
            ++clearClicks;
            MyePersistSetValue(ctx, "flow.best", 0);
        }

        // ---- 開始: START のクリック / 90 tick (自動デモ = 決定論) ----
        // ★"Jump" (Space / パッド A) を直接見ないこと — Space と A は
        //   UINavSubmit にも割り当ててあるので、CLEAR BEST を選んで決定したときに
        //   「消えると同時にゲームも始まる」二重発火になる。
        //   決定は 1 本 (フォーカス + Submit) に寄せる
        if (MyeUIClicked(ctx, startBtn) || ticksInScene == 90) {
            MyePersistSetValue(ctx, "flow.runs", lastRuns + 1);
            MyePersistSetValue(ctx, "flow.resume", 0); // タイトル経由の開始はスコア 0 から
            api->LoadScene(api->engine, "scenes/flow_game.scene.json");
        }
    }
};
REGISTER_SCRIPT(FlowTitleDriver,
                FIELDS(MYE_F_JP(ticksInScene, "シーン内の経過 tick"),
                       MYE_F_JP(lastBest, "最高記録"), MYE_F_JP(lastScore, "前回のスコア"),
                       MYE_F_JP(lastRuns, "プレイ回数"),
                       MYE_F_JP(clearClicks, "記録消去の押下数"),
                       MYE_F_JP(sceneTag, "シーン名のハッシュ")));
