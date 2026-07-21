// M21 デモ: ゲーム内 UI ボタンのクリックを InputSnapshot マウスで判定して反応する。
// UI の見た目はエンジンが UIElementComponent (kind=2, anchor=0) を描画し、このスクリプトが
// 同じピクセル矩形をヒットテストする (ABI 追加なし = bump 不要、verify で replay 一致)。
// 使い方: 任意エンティティにこのスクリプトを付け、btn* を対象ボタンの矩形に合わせる。
#include "Shared/ScriptAPI.h"

struct UIButtonDemo : Script<UIButtonDemo> {
    float btnX = 20.0f; // ボタン矩形 (anchor=0 左上の UIElement と一致させる)
    float btnY = 20.0f;
    float btnW = 180.0f;
    float btnH = 48.0f;
    int32_t clicks = 0;
    int32_t prevDown = 0; // クリックのエッジ検出 (登録フィールドなのでリロードでも保持)

    void Update(MyeUpdateContext& ctx)
    {
        if (MyeButtonClicked(ctx, { btnX, btnY, btnW, btnH }, prevDown)) {
            ++clicks;
            MyeLogf(ctx, "UI button clicked (#%d)", clicks);
            ctx.api->PlaySound(ctx.api->engine, "beep", 0.8f);
            // シーン遷移する例 (M19 の LoadScene と連携):
            // ctx.api->LoadScene(ctx.api->engine, "scenes/scene_b.scene.json");
        }
    }
};
REGISTER_SCRIPT(UIButtonDemo, FIELDS(btnX, btnY, btnW, btnH, clicks, prevDown));
