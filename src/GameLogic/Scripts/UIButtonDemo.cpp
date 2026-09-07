// M21 デモ: ゲーム内 UI ボタンのクリックに反応する。
//
// ★M70c で作りが変わった。それまでは「UIElement と同じピクセル矩形をスクリプト側にも
//   手書きして、マウス座標と自分で比べる」形だった (btnX/btnY/btnW/btnH の 4 フィールド)。
//   矩形が二重管理なので、UIElement 側のレイアウトを動かすと**絵と当たり判定が黙って
//   食い違う** — アンカーを 0 (左上) 以外にした瞬間に成立しなくなるうえ、M70b で
//   数値がキャンバス単位になってからは実 px との対応も崩れていた。
//   今はエンジンが矩形の解決も押下判定もフォーカスも持っているので、スクリプトは
//   「そのエンティティが押されたか」を聞くだけでよい (MyeUIClicked)。
//
// 使い方: **押したい UIElement (kind=2) と同じエンティティ**にこのスクリプトを付ける。
//   別のエンティティを押したいときは Inspector で target に D&D で放り込む
//   (未設定 = 自分自身)。名前引きではなく EntityRef にしてあるのは、名前の打ち間違いが
//   「何も起きない」という一番読めない壊れ方になるため (dogfooding #9)。
//   マウスでもパッドでも動く — フォーカス中の要素で UINavSubmit を押した tick にも
//   clicked が立つので、ゲーム側で入力デバイスごとの分岐を書かなくてよい。
#include "Shared/ScriptAPI.h"

struct UIButtonDemo : Script<UIButtonDemo> {
    MyeEntityId target = {}; // 押す対象。未設定 (null) = このスクリプトが付いているもの
    int32_t clicks = 0;

    void Update(MyeUpdateContext& ctx)
    {
        const MyeEntityId id = MyeEntityIdIsNull(target) ? ctx.self : target;
        if (!MyeUIClicked(ctx, id)) {
            return;
        }
        ++clicks;
        MyeLogf(ctx, "UI button clicked (#%d)", clicks);
        ctx.api->PlaySound(ctx.api->engine, "beep", 0.8f);
        // シーン遷移する例 (M19 の LoadScene と連携):
        // ctx.api->LoadScene(ctx.api->engine, "scenes/scene_b.scene.json");
    }
};
REGISTER_SCRIPT(UIButtonDemo,
                FIELDS(MYE_F_JP(target, "対象のボタン"), MYE_F_JP(clicks, "クリック回数")));
