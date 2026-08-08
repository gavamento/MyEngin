// 汎用フィールドアクセス (v11、M50d) の恒久 probe。
//
// 既定デモの Spinner に Health (assets\schemas 由来のスキーマ型) と対で付き、
// リプレイ検証で毎回走る。
// ★GetComponentField → 減衰 → SetComponentField の結果を sim 状態 (Health.current +
//   登録フィールド) に**書き戻す**のが本体 — 呼ぶだけでは汎用 ABI の Debug/Release
//   divergence を replay_verify が検知できない (M49 PartRaycastDemo と同じ流儀)。
//
// 読むのは自分の Health コンポーネント (シーンデータ) だけ、減衰量は定数なので決定論。
#include "Shared/ScriptAPI.h"

struct SchemaHealthDemo : Script<SchemaHealthDemo> {
    int32_t writes = 0;    // 書き戻し回数 (被覆の本体 — 構成間でズレたら即 divergence)
    float mirrored = 0.0f; // 最後に書いた Health.current のミラー

    void Update(MyeUpdateContext& ctx)
    {
        // nameHash は Shared 再掲の FNV (HashStr と同一定数 — SchemaSelfTest が機械検査)
        constexpr uint64_t comp = MyeNameHash("Health");
        constexpr uint64_t fCur = MyeNameHash("current");
        constexpr uint64_t fMax = MyeNameHash("max");
        float cur = 0.0f;
        float max = 0.0f;
        if (!MyeGetField(ctx, ctx.self, comp, fCur, cur)
            || !MyeGetField(ctx, ctx.self, comp, fMax, max)) {
            return; // Health 非所持 (スキーマ未登録の起動) は何もしない
        }
        uint8_t invulnerable = 0; // Bool フィールドは 1 バイト (Get 経路の Bool 被覆)
        MyeGetField(ctx, ctx.self, comp, MyeNameHash("invulnerable"), invulnerable);
        if (invulnerable != 0) {
            return;
        }
        // のこぎり波で max→0 を掃引 — 値が毎 tick 変わり続けることで Set の実走を保証する
        cur -= 0.25f;
        if (cur < 0.0f) {
            cur = max;
        }
        if (MyeSetField(ctx, ctx.self, comp, fCur, cur)) {
            ++writes;
            mirrored = cur;
        }
    }
};
REGISTER_SCRIPT(SchemaHealthDemo, FIELDS(writes, mirrored));
