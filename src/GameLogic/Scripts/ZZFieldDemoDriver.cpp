//====================================================================================
//                          ZZFieldDemoDriver.cpp
//  MyEngin/ 秋田蓮音                                                       11/08/2026
//                                          かき乱しフィールド (M96) のデモの操作役 (--field-demo の歩く人)
//====================================================================================
// 台本 (シーン内の経過 tick で決める = 入力に依存しない = replay の field ジョブの被覆):
//   霧の中を一辺 5 m の四角い経路で歩く (CharacterMove)。足元の濃さを FieldSample で読み、濃い所では遅くなる
//   tick 45 から 90 tick ごと : 前方へ Cone の「斬撃」(MyeFieldCone)
//   tick 120 から 240 tick ごと: 経路から離れた一点で Radial の「爆発」(MyeFieldRadial、densityDelta = -1)
// ★スクリプト名は名前順ソートで TypeId が決まる。既存スクリプトの TypeId を動かさないよう、
//   現行末尾の ZTwoDMover より後ろに並ぶ 'ZZ' で始めてある
#include "Shared/ScriptAPI.h"

namespace {

constexpr float kWalkSpeed = 3.0f;           // [m/s]
constexpr int32_t kSegmentTicks = 100;       // 経路の一辺を歩く tick 数 (一辺 5 m)
constexpr float kDenseThreshold = 0.8f;      // 足元の濃さがこれを超えたら遅くなる
constexpr float kDenseSlowFactor = 0.5f;     // 濃い所での速さの倍率
constexpr int32_t kSlashFirstTick = 45;
constexpr int32_t kSlashIntervalTicks = 90;
constexpr float kSlashReach = 0.8f;          // 人の前方 [m]。扇の根元
constexpr float kSlashHeight = 0.3f;         // 層の中の高さ [m]
constexpr float kSlashRadius = 3.5f;
constexpr float kSlashSpeed = 6.0f;
constexpr float kSlashConeDeg = 35.0f;
constexpr float kSlashDensityDelta = -0.8f;
constexpr int32_t kBlastFirstTick = 120;
constexpr int32_t kBlastIntervalTicks = 240;
constexpr float kBlastRadius = 4.0f;
constexpr float kBlastSpeed = 6.0f;
constexpr float kBlastDensityDelta = -1.0f;
const MyeVec3 kBlastPos = { -7.0f, kSlashHeight, 1.0f };

// 経路の向き (+z → +x → -z → -x の順に一周)
MyeVec3 WalkDirection(int32_t ticks)
{
    switch ((ticks / kSegmentTicks) % 4) {
    case 0: return { 0.0f, 0.0f, 1.0f };
    case 1: return { 1.0f, 0.0f, 0.0f };
    case 2: return { 0.0f, 0.0f, -1.0f };
    default: return { -1.0f, 0.0f, 0.0f };
    }
}

} // namespace

struct ZZFieldDemoDriver : Script<ZZFieldDemoDriver> {
    int32_t ticks = 0;        // 台本の時計
    float footDensity = 0.0f; // 足元の濃さ (直近に読んだ値。Inspector で見る)
    int32_t slashCount = 0;   // 通算の斬撃の数
    int32_t blastCount = 0;   // 通算の爆発の数

    void Update(MyeUpdateContext& ctx)
    {
        const MyeVec3 pos = MyeSelf(ctx).GetWorldPosition();
        const MyeVec3 dir = WalkDirection(ticks);

        // 足元 (層の下端 y = 0) の濃さ。場の外なら 0 のまま = 遅くならない
        MyeFieldSample sample = {};
        footDensity = MyeFieldSampleAt(ctx, { pos.x, 0.0f, pos.z }, sample) ? sample.density : 0.0f;
        const float speed = footDensity > kDenseThreshold ? kWalkSpeed * kDenseSlowFactor : kWalkSpeed;
        ctx.api->CharacterMove(ctx.api->engine, ctx.self, { dir.x * speed, 0.0f, dir.z * speed });

        if (ticks >= kSlashFirstTick && (ticks - kSlashFirstTick) % kSlashIntervalTicks == 0) {
            const MyeVec3 root = { pos.x + dir.x * kSlashReach, kSlashHeight, pos.z + dir.z * kSlashReach };
            slashCount += MyeFieldCone(ctx, root, dir, kSlashRadius, kSlashSpeed, kSlashConeDeg, kSlashDensityDelta);
        }
        if (ticks >= kBlastFirstTick && (ticks - kBlastFirstTick) % kBlastIntervalTicks == 0) {
            blastCount += MyeFieldRadial(ctx, kBlastPos, kBlastRadius, kBlastSpeed, kBlastDensityDelta);
        }
        ++ticks;
    }
};
REGISTER_SCRIPT(ZZFieldDemoDriver,
                FIELDS(MYE_F_JP(ticks, "経過 tick"), MYE_F_JP(footDensity, "足元の濃さ"),
                       MYE_F_JP(slashCount, "斬撃の数"), MYE_F_JP(blastCount, "爆発の数")));
