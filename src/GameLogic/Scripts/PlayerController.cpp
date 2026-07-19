// 矢印キーで移動するデモスクリプト (engine_spec.md 5.2 のサンプルに相当)
#include "Shared/ScriptAPI.h"

namespace {
// <Windows.h> を引き込まないため VK コードを直接定義
constexpr uint8_t kVkSpace = 0x20;
constexpr uint8_t kVkLeft = 0x25;
constexpr uint8_t kVkUp = 0x26;
constexpr uint8_t kVkRight = 0x27;
constexpr uint8_t kVkDown = 0x28;
} // namespace

struct PlayerController : Script<PlayerController> {
    float moveSpeed = 5.0f;
    int32_t jumpCount = 0;
    int32_t prevSpace = 0; // エッジ検出用 (登録フィールドなのでリロードでも保持)

    void Start(MyeUpdateContext& ctx)
    {
        MyeLogf(ctx, "PlayerController started (moveSpeed=%.1f)", moveSpeed);
    }

    void Update(MyeUpdateContext& ctx)
    {
        const MyeEngineApi* api = ctx.api;
        float dx = 0.0f;
        float dz = 0.0f;
        if (api->KeyDown(api->engine, kVkRight)) { dx += 1.0f; }
        if (api->KeyDown(api->engine, kVkLeft)) { dx -= 1.0f; }
        if (api->KeyDown(api->engine, kVkUp)) { dz += 1.0f; }
        if (api->KeyDown(api->engine, kVkDown)) { dz -= 1.0f; }

        if (dx != 0.0f || dz != 0.0f) {
            MyeGameObject self = MyeSelf(ctx);
            MyeVec3 pos = self.GetLocalPosition();
            pos.x += dx * moveSpeed * ctx.dt;
            pos.z += dz * moveSpeed * ctx.dt;
            self.SetLocalPosition(pos);
        }

        const int space = api->KeyDown(api->engine, kVkSpace);
        if (space && !prevSpace) {
            ++jumpCount;
            MyeLogf(ctx, "jump #%d", jumpCount);
        }
        prevSpace = space;
    }
};
REGISTER_SCRIPT(PlayerController, FIELDS(moveSpeed, jumpCount, prevSpace));
