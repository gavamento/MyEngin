#include "Engine/Engine/PlayerInputSystem.h"

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Platform/Input.h"
#include "Engine/Platform/InputActions.h"

namespace mye {

void UpdatePlayerInputMirror(World& world, const InputActions& actions, const InputSnapshot* inputs,
                             uint32_t playerCount)
{
    const ComponentTypeId req[] = { PlayerInputComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int pi = arch.FindTypeIndex(PlayerInputComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            auto* c = static_cast<PlayerInputComponent*>(arch.GetPtr(pi, row));
            // 範囲外のレーン番号は「未接続レーン」に落とす。0 へ丸めない —
            // 丸めると打ち間違いが「2 人目が 1 人目と同じ操作で動く」形で出て、
            // 一番気づきにくい壊れ方になる
            const bool valid = c->playerIndex >= 0
                && static_cast<uint32_t>(c->playerIndex) < playerCount;
            if (!valid) {
                *c = PlayerInputComponent{ c->playerIndex };
                continue;
            }
            const uint32_t lane = static_cast<uint32_t>(c->playerIndex);
            // レーン 0 はキーボード/マウスが常にあるので接続扱い、レーン n>0 は
            // XInput スロット n の接続状態そのもの (Input.h のレーン規約)。
            // ★padConnected は**記録済みスナップショット由来**なので verify でも再現する
            //   (ライブの XInputGetState をここで叩いたら決定論が壊れる)
            c->connected = (lane == 0) || (inputs != nullptr && inputs[lane].padConnected != 0);

            // 軸: actions.json の定義順 index 0..3。定義が無い index は 0 のまま
            float axes[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            const size_t axisCount = actions.Axes().size();
            for (size_t i = 0; i < 4 && i < axisCount; ++i) {
                axes[i] = actions.AxisValueAt(i, lane);
            }
            c->axes = DirectX::XMFLOAT4(axes[0], axes[1], axes[2], axes[3]);

            // アクション: 同じく定義順 index 0..31 をビットへ詰める
            uint32_t held = 0, pressed = 0, released = 0;
            const size_t actionCount = actions.Actions().size();
            for (size_t i = 0; i < 32 && i < actionCount; ++i) {
                const uint32_t s = actions.ActionStateAt(i, lane);
                const uint32_t bit = 1u << i;
                if (s & kActionHeld) { held |= bit; }
                if (s & kActionPressed) { pressed |= bit; }
                if (s & kActionReleased) { released |= bit; }
            }
            c->heldBits = held;
            c->pressedBits = pressed;
            c->releasedBits = released;
        }
    });
}

} // namespace mye
