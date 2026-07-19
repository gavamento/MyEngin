// エンティティを Y 軸回転させるデモスクリプト
#include <math.h>

#include "Shared/ScriptAPI.h"

struct Rotator : Script<Rotator> {
    // 状態はエンジン側 ECS に保持され、Inspector で編集でき、DLL リロードを跨いで保存される
    float speedDegPerSec = 30.0f;
    float angleDeg = 0.0f;

    void Start(MyeUpdateContext& ctx)
    {
        MyeLogf(ctx, "Rotator started (speed=%.1f deg/s)", speedDegPerSec);
    }

    void Update(MyeUpdateContext& ctx)
    {
        angleDeg += speedDegPerSec * ctx.dt;
        const float half = angleDeg * (3.14159265358979323846f / 180.0f) * 0.5f;
        const MyeQuat q = { 0.0f, sinf(half), 0.0f, cosf(half) }; // Y 軸回転
        MyeSelf(ctx).SetLocalRotation(q);
    }
};
REGISTER_SCRIPT(Rotator, FIELDS(speedDegPerSec, angleDeg));

