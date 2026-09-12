#pragma once
#include <cstdint>
#include <vector>

#include "Engine/Platform/Input.h"
#include "Engine/Platform/InputActions.h"

namespace mye {

// 入力の上書き (M72f、What-if の「入力を変えて分岐する」)。
//
// ライブレーンの tick [fromTick, toTick) が走るとき、その tick の入力レーン lane に
// キー / ボタンを OR し、軸を置換する。置く場所は EngineLoop の入力置換チェーン
// (verify / net / synth) の**後ろ** = ここで入れた値がそのままリングの entry に記録されるので、
// 後からシークしても同じビットが再現する (合成入力と同じ扱い)。
// ★verify / net では絶対に適用しない (EngineLoop 側のゲート)。sim の外で ctx.inputs を確定させる
//   操作なので、ハッシュ / .rep / snapshot の版には影響しない。
// ★意味論は「ライブレーンの to-do」: レーンを切り替えて同じ tick をもう一度走らせると
//   また効く。消したければ Clear
// ★文字キュー (M75b の chars / charCount) とゲーム面 (mouseSurfX/Y, surfW/H) は上書きしない。
//   文字は「押し続ける」の意味論を持たない (毎 tick 同じ文字を積むと InputField に連打される) し、
//   面を上書きすると記録済みの当たり判定の土俵がその区間だけ別物になる
struct InputOverride {
    enum : uint32_t {
        kSetPadLX = 1u << 0,
        kSetPadLY = 1u << 1,
        kSetPadRX = 1u << 2,
        kSetPadRY = 1u << 3,
        kSetPadLT = 1u << 4,
        kSetPadRT = 1u << 5,
        kSetMouseDX = 1u << 6,
        kSetMouseDY = 1u << 7,
    };
    uint32_t lane = 0;
    uint64_t fromTick = 0;
    uint64_t toTick = 0;        // 含まない
    InputSnapshot orBits = {};  // keys / mouseButtons / padButtons / padConnected を OR
    uint32_t setMask = 0;       // kSet* — 軸は置換 (OR できないため)
    InputSnapshot setValues = {};
    char label[32] = {};

    bool Applies(uint64_t tick) const { return tick >= fromTick && tick < toTick; }
    void ApplyTo(InputSnapshot& in) const;

    // アクションを押し続ける。経路は keys[0] → padMask の最下位ビット → mouseMask の最下位ビット
    // の順で 1 本だけ選ぶ (全部立てると「キーとパッドを同時に押した」記録になる)
    static InputOverride HoldAction(const InputActionDef& def, uint32_t lane, uint64_t from,
                                    uint64_t to);
    // 軸を value (-1..+1) に倒し続ける。value > 0 なら posKey、< 0 なら negKey、
    // キーが無ければ padAxis を value × 32767 (LT/RT は |value| × 255) に置換
    static InputOverride HoldAxis(const InputAxisDef& def, float value, uint32_t lane, uint64_t from,
                                  uint64_t to);
};

class InputOverrideSet {
public:
    std::vector<InputOverride> items;
    void Apply(uint64_t tick, InputSnapshot* lanes, uint32_t playerCount) const;
    void Clear() { items.clear(); }
    bool Empty() const { return items.empty(); }
};

} // namespace mye
