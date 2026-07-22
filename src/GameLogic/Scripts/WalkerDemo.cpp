// WASD + Space でキャラクターコントローラを操作するデモ (M29b)。
// CharacterController を持つエンティティにアタッチして使う。
// 注: スクリプト名は名前順ソートで TypeId が決まるため、既存 (UIButtonDemo 等) の後に
// 並ぶ 'W' 始まりにしている (既存シーンのスクリプト TypeId を変えない)
#include "Shared/ScriptAPI.h"

namespace {
// <Windows.h> を引き込まないため VK コードを直接定義
constexpr uint8_t kVkSpace = 0x20;
constexpr uint8_t kVkA = 0x41;
constexpr uint8_t kVkD = 0x44;
constexpr uint8_t kVkS = 0x53;
constexpr uint8_t kVkW = 0x57;
} // namespace

struct WalkerDemo : Script<WalkerDemo> {
    float moveSpeed = 4.0f;
    float jumpSpeed = 5.0f;
    int32_t prevJump = 0; // エッジ検出用 (登録フィールドなのでリロードでも保持)

    void Update(MyeUpdateContext& ctx)
    {
        const MyeEngineApi* api = ctx.api;
        float dx = 0.0f;
        float dz = 0.0f;
        if (api->KeyDown(api->engine, kVkD)) { dx += 1.0f; }
        if (api->KeyDown(api->engine, kVkA)) { dx -= 1.0f; }
        if (api->KeyDown(api->engine, kVkW)) { dz += 1.0f; }
        if (api->KeyDown(api->engine, kVkS)) { dz -= 1.0f; }

        // gamepad 左スティックでも移動 (未接続時は 0 = 挙動不変)
        MyeVec2 ls = {}, rs = {};
        api->PadSticks(api->engine, &ls, &rs);
        if (ls.x > 0.3f) { dx += 1.0f; } else if (ls.x < -0.3f) { dx -= 1.0f; }
        if (ls.y > 0.3f) { dz += 1.0f; } else if (ls.y < -0.3f) { dz -= 1.0f; }

        // 斜め移動を正規化 (dx/dz は ±1 の組合せなので分岐は決定論)
        if (dx != 0.0f && dz != 0.0f) {
            dx *= 0.7071068f;
            dz *= 0.7071068f;
        }
        api->CharacterMove(api->engine, ctx.self, { dx * moveSpeed, 0.0f, dz * moveSpeed });

        // ジャンプ = Space または gamepad A (接地時のみ、押した瞬間)
        const int jump =
            (api->KeyDown(api->engine, kVkSpace) || api->PadButton(api->engine, MYE_PAD_A)) ? 1 : 0;
        if (jump && !prevJump && api->CharacterIsGrounded(api->engine, ctx.self)) {
            api->CharacterJump(api->engine, ctx.self, jumpSpeed);
        }
        prevJump = jump;
    }
};
REGISTER_SCRIPT(WalkerDemo, FIELDS(moveSpeed, jumpSpeed, prevJump));
