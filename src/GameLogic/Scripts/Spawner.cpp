// 一定間隔でエンティティを生成し、古いものから破棄するデモスクリプト。
// リプレイ一貫性テスト (spec 11.3) に構造変更 (生成/破棄) と RNG 消費を含めるのが狙い
#include "Shared/ScriptAPI.h"

struct Spawner : Script<Spawner> {
    int32_t intervalTicks = 30;
    int32_t counter = 0;
    int32_t slot = 0;
    MyeEntityId spawned0 = {};
    MyeEntityId spawned1 = {};
    MyeEntityId spawned2 = {};
    MyeEntityId spawned3 = {};

    void Update(MyeUpdateContext& ctx)
    {
        if (++counter < intervalTicks) {
            return;
        }
        counter = 0;

        const MyeEngineApi* api = ctx.api;
        MyeEntityId* slots[4] = { &spawned0, &spawned1, &spawned2, &spawned3 };
        MyeEntityId* target = slots[slot & 3];
        slot = (slot + 1) & 3;

        // 一番古いスロットを破棄して新規生成 (エンジン RNG で位置決め — 決定論)
        if (!MyeEntityIdIsNull(*target) && api->IsAlive(api->engine, *target)) {
            api->DestroyGameObject(api->engine, *target);
        }
        const MyeEntityId e = api->CreateGameObject(api->engine, "Spawned");
        const MyeVec3 pos = { api->RandomRange(api->engine, -3.0f, 3.0f),
                              api->RandomRange(api->engine, 0.5f, 2.0f),
                              api->RandomRange(api->engine, -3.0f, 3.0f) };
        api->SetLocalPosition(api->engine, e, pos);
        api->SetLocalScale(api->engine, e, { 0.5f, 0.5f, 0.5f });
        // 見た目 (アセットキー名で解決) + コライダ (既定 = 球 r0.5 トリガー)
        api->SetMeshRenderer(api->engine, e, "builtin://cube", "mat_yellow");
        api->AddComponentByName(api->engine, e, "Collider");
        *target = e;
    }
};
REGISTER_SCRIPT(Spawner, FIELDS(intervalTicks, counter, slot, spawned0, spawned1, spawned2, spawned3));

