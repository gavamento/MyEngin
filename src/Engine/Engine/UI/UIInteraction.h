#pragma once
// ゲーム内 UI の対話状態 (M70c)。「押されたか」をエンジンが持ち、スクリプトは結果を読むだけ。
//
// M70b までは押下判定が **UIRenderer の中にだけ**あり (ハイライト表示のためだけに計算して
// 捨てていた)、ゲーム側で動く経路は「UIElement と同じ矩形をスクリプトにもう一度手書きして
// マウス座標と比べる」しか無かった (`UIButtonDemo`)。矩形が二重管理になるので、
// レイアウトを変えた瞬間に**絵と当たり判定が黙って食い違う**。
//
// ★状態は `Scene` が持つ sim 状態で **WorldHash 対象** (TimeControl の隣)。
//   UIElement 自体は kComponentNoHash なので、ここをハッシュに載せておかないと
//   「配線が壊れても replay_verify が緑のまま」になる — 唯一の防波堤がこれ。
// ★評価はキャンバス座標 (M70b) で行い、描画とヒットテストは `uilayout` の同じ関数を通る。
//   マウス位置も canvas 正規化済みの値を InputSnapshot から読むので、窓の大きさに依らない。
#include <cstdint>

#include "Engine/Core/EntityID.h"

namespace mye {

class World;
struct InputSnapshot;
class InputActions;

// エンジンが持つ UI 対話状態。EntityID 4 本だけの POD (Scene が所有)。
struct UIInteractionState {
    EntityID hovered = kNullEntity; // カーソルの下にある最前面の要素
    // 左ボタンを押した瞬間に掴んだ要素。離すまで保持する = **押したまま外へ出て戻ると
    // まだ押されている** (Unity / OS のボタン意味論)。押下中に別の要素へは移らない
    EntityID pressed = kNullEntity;
    // 「掴んだ要素の上で離した」tick だけ立つ。次の tick の頭で必ず消える派生値だが、
    // スクリプトはこの tick 中にしか読めないので状態として持つ (ハッシュにも載る)
    EntityID clicked = kNullEntity;
    EntityID focused = kNullEntity; // パッド/キーのフォーカス。UIElement.focused へ書き戻す
    // シーンが書いた UIElement.focused を**一度だけ**拾ったか (Unity の EventSystem の
    // "First Selected" 相当)。エンジンがフォーカスの正本になった以上、毎 tick ミラーを
    // 上書きするので、これが無いと「シーンで focused=1 と書いた要素」が起動直後に消える。
    // 毎 tick 拾い直す作りにはできない — ユーザーがフォーカスを外した次の tick に
    // 勝手に戻ってしまう。**ハッシュにもスナップショットにも載せる** (拾ったかどうかで
    // 以後の挙動が変わるので、巻き戻しで戻らないと再シムが割れる)
    uint32_t adoptedAuthored = 0;

    void Clear()
    {
        hovered = kNullEntity;
        pressed = kNullEntity;
        clicked = kNullEntity;
        focused = kNullEntity;
        adoptedAuthored = 0; // 新しいシーンの authored focus をまた拾う
    }
};

namespace uiinteract {

// フォーカス移動のアクション名 (assets\input\actions.json)。**この 5 本が正本** —
// 名前を変えるときはここと actions.json を同時に直すこと (未定義名は常に false を返すので、
// 綴り違いは「動かないだけ」で静かに壊れる)
inline constexpr const char* kActionNavUp = "UINavUp";
inline constexpr const char* kActionNavDown = "UINavDown";
inline constexpr const char* kActionNavLeft = "UINavLeft";
inline constexpr const char* kActionNavRight = "UINavRight";
inline constexpr const char* kActionSubmit = "UINavSubmit";

// キャンバス座標の点 (x,y) を含む最前面の active UIElement。無ヒットは kNullEntity。
// 最前面 = order 最大、同値は entity.index 最大 (UIRenderer の描画順で上のもの)。
// 祖先クリップで見えない部分には当たらない。
// ★**ABI の UIHitTest もこの関数を呼ぶ** — ヒットテストの規則を 2 本書かないため
EntityID HitTest(World& world, int canvasW, int canvasH, float x, float y);

// current から dir (uinav::NavDir) 方向の次のフォーカス候補。候補が無ければ current。
// current が候補に無ければ「最初の候補」へ吸着する (フォーカス不在からの入りぐち)
EntityID FindNextFocus(World& world, int canvasW, int canvasH, EntityID current, int dir);

// 1 tick 分の評価。**スクリプト層より前**に 1 回だけ呼ぶ (TickRunner)。
//   1. clicked を落とす (1 tick だけ立つ値)
//   2. hovered / pressed / clicked をマウスから更新
//   3. UINav* アクションで focused を動かし、UIElement.focused へ書き戻す
// in はレーン 0 の入力 (キャンバス座標のマウスを持つ唯一のレーン)。
// actions は評価済みのアクションマップ (null 可 = フォーカス移動なし)
void Evaluate(World& world, const InputSnapshot& in, const InputActions* actions,
              UIInteractionState& state);

// UIButtonState (ABI v16) のビット。**ScriptAPI.h の MyeUIButton* と同じ値**
enum StateBits : uint32_t {
    kHovered = 1u << 0,
    kPressed = 1u << 1,
    kClicked = 1u << 2,
    kFocused = 1u << 3,
};
uint32_t BitsFor(const UIInteractionState& state, EntityID e);

} // namespace uiinteract
} // namespace mye
