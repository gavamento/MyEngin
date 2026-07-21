#include "Engine/Engine/Script/EngineApiTable.h"

#include <cstddef>

#include "Engine/Core/Components.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Physics/PhysicsSystem.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace {

// Shared 側 POD とエンジン型のバイナリ互換 (DLL / CLR 境界の前提)
static_assert(sizeof(MyeEntityId) == sizeof(EntityID));
static_assert(offsetof(MyeEntityId, index) == offsetof(EntityID, index));
static_assert(offsetof(MyeEntityId, generation) == offsetof(EntityID, generation));

EntityID ToEngine(MyeEntityId id) { return { id.index, id.generation }; }
MyeEntityId ToShared(EntityID id) { return { id.index, id.generation }; }

ScriptApiContext* Ctx(void* engine) { return static_cast<ScriptApiContext*>(engine); }
Scene* Sc(void* engine) { return Ctx(engine)->scene; }

} // namespace

// engine ポインタは常に ScriptApiContext*。キャプチャなしラムダ → 関数ポインタ変換で
// extern "C" スタイルのテーブルを埋める (C++ ScriptHost / C# ManagedHost が共用)
void BuildEngineApi(MyeEngineApi& out, ScriptApiContext* ctx)
{
    out = {};
    out.version = MYE_API_VERSION;
    out.engine = ctx;

    out.Log = [](void* engine, int level, const char* msg) {
        (void)engine;
        logging::Write(static_cast<LogLevel>(level & 3), "[script] %s", msg);
    };
    out.KeyDown = [](void* engine, uint8_t vk) -> int {
        return Ctx(engine)->input.KeyDown(vk) ? 1 : 0;
    };
    out.MouseButton = [](void* engine, int button) -> int {
        return Ctx(engine)->input.MouseDown(button) ? 1 : 0;
    };
    out.MousePos = [](void* engine, int32_t* x, int32_t* y) {
        if (x) { *x = Ctx(engine)->input.mouseX; }
        if (y) { *y = Ctx(engine)->input.mouseY; }
    };
    out.CreateGameObject = [](void* engine, const char* name) -> MyeEntityId {
        return ToShared(Sc(engine)->GetWorld().CreateEntity(name ? name : "GameObject"));
    };
    out.DestroyGameObject = [](void* engine, MyeEntityId id) {
        Sc(engine)->GetWorld().DestroyEntity(ToEngine(id));
    };
    out.IsAlive = [](void* engine, MyeEntityId id) -> int {
        return Sc(engine)->GetWorld().IsAlive(ToEngine(id)) ? 1 : 0;
    };
    out.FindByName = [](void* engine, const char* name) -> MyeEntityId {
        return ToShared(Sc(engine)->Find(name ? name : "").Id());
    };
    out.SetParent = [](void* engine, MyeEntityId child, MyeEntityId parent) {
        Sc(engine)->GetWorld().SetParent(ToEngine(child), ToEngine(parent));
    };
    out.GetLocalPosition = [](void* engine, MyeEntityId id, MyeVec3* o) -> int {
        auto* t = Sc(engine)->GetWorld().GetComponent<LocalTransform>(ToEngine(id));
        if (!t || !o) { return 0; }
        *o = { t->position.x, t->position.y, t->position.z };
        return 1;
    };
    out.SetLocalPosition = [](void* engine, MyeEntityId id, MyeVec3 v) -> int {
        auto* t = Sc(engine)->GetWorld().GetComponent<LocalTransform>(ToEngine(id));
        if (!t) { return 0; }
        t->position = { v.x, v.y, v.z };
        return 1;
    };
    out.GetLocalRotation = [](void* engine, MyeEntityId id, MyeQuat* o) -> int {
        auto* t = Sc(engine)->GetWorld().GetComponent<LocalTransform>(ToEngine(id));
        if (!t || !o) { return 0; }
        *o = { t->rotation.x, t->rotation.y, t->rotation.z, t->rotation.w };
        return 1;
    };
    out.SetLocalRotation = [](void* engine, MyeEntityId id, MyeQuat q) -> int {
        auto* t = Sc(engine)->GetWorld().GetComponent<LocalTransform>(ToEngine(id));
        if (!t) { return 0; }
        t->rotation = { q.x, q.y, q.z, q.w };
        return 1;
    };
    out.GetLocalScale = [](void* engine, MyeEntityId id, MyeVec3* o) -> int {
        auto* t = Sc(engine)->GetWorld().GetComponent<LocalTransform>(ToEngine(id));
        if (!t || !o) { return 0; }
        *o = { t->scale.x, t->scale.y, t->scale.z };
        return 1;
    };
    out.SetLocalScale = [](void* engine, MyeEntityId id, MyeVec3 v) -> int {
        auto* t = Sc(engine)->GetWorld().GetComponent<LocalTransform>(ToEngine(id));
        if (!t) { return 0; }
        t->scale = { v.x, v.y, v.z };
        return 1;
    };
    out.RandomFloat01 = [](void* engine) -> float {
        return Sc(engine)->GetWorld().Rng().NextFloat01();
    };
    out.RandomRange = [](void* engine, float lo, float hi) -> float {
        return Sc(engine)->GetWorld().Rng().Range(lo, hi);
    };
    out.AddComponentByName = [](void* engine, MyeEntityId id, const char* name) -> int {
        if (!name) { return 0; }
        const ComponentTypeId t = ComponentRegistry::Get().FindByName(name);
        if (t == kInvalidComponentType) { return 0; }
        return Sc(engine)->GetWorld().AddComponentRaw(ToEngine(id), t) ? 1 : 0;
    };
    out.SetMeshRenderer = [](void* engine, MyeEntityId id, const char* meshKey,
                             const char* materialKey) -> int {
        World& world = Sc(engine)->GetWorld();
        auto* mr = static_cast<MeshRendererComponent*>(
            world.AddComponentRaw(ToEngine(id), MeshRendererComponent::sTypeId));
        if (!mr) { return 0; }
        if (meshKey) { mr->mesh = AssetID{ HashStr(meshKey) }; }
        if (materialKey) { mr->material = AssetID{ HashStr(materialKey) }; }
        return 1;
    };

    // ---- gamepad (v3、M19)。context.input の gamepad フィールドから読む ----
    out.PadConnected = [](void* engine) -> int {
        return Ctx(engine)->input.padConnected ? 1 : 0;
    };
    out.PadButton = [](void* engine, uint16_t mask) -> int {
        return Ctx(engine)->input.PadButton(mask) ? 1 : 0;
    };
    out.PadSticks = [](void* engine, MyeVec2* left, MyeVec2* right) {
        const InputSnapshot& in = Ctx(engine)->input;
        constexpr float kInv = 1.0f / 32767.0f;
        if (left) { *left = { in.padLX * kInv, in.padLY * kInv }; }
        if (right) { *right = { in.padRX * kInv, in.padRY * kInv }; }
    };
    out.PadTriggers = [](void* engine, float* left, float* right) {
        const InputSnapshot& in = Ctx(engine)->input;
        if (left) { *left = in.padLeftTrigger / 255.0f; }
        if (right) { *right = in.padRightTrigger / 255.0f; }
    };

    // ---- 物理 Raycast (v3 で予約、M20 で実装)。ワールドの全コライダーに対する最近ヒット ----
    out.Raycast = [](void* engine, MyeVec3 origin, MyeVec3 dir, float maxDist,
                     MyeRaycastHit* outHit) -> int {
        return RaycastWorld(Sc(engine)->GetWorld(), origin, dir, maxDist, outHit);
    };

    // ---- オーディオ (v3 で予約、M19.3 で drain 実装)。tick 内で決定論順に積む ----
    out.PlaySound = [](void* engine, const char* soundKey, float volume) -> int {
        auto* q = Ctx(engine)->audioQueue;
        if (!q || !soundKey) { return 0; }
        q->push_back({ soundKey, volume });
        return static_cast<int>(q->size()); // 疑似 voice ハンドル (1 始まり)
    };
    out.StopSound = [](void* engine, int voice) {
        (void)engine;
        (void)voice; // M19.3 で voice 管理を実装
    };

    // ---- シーン遷移 (v3 で予約、M19.4 で消費)。tick 末に遅延ロード ----
    out.LoadScene = [](void* engine, const char* scenePath) {
        auto* pending = Ctx(engine)->pendingScene;
        if (pending && scenePath) {
            *pending = Utf8ToWide(scenePath); // tick 末に EngineLoop が消費する (M19.4)
        }
    };

    // ---- 剛体操作 (v4、M28a)。velocity は hash 対象の sim 状態だが、スクリプト実行順は
    // 決定論なので直接更新してよい (SetLocalPosition と同格)。蓄積フィールドは持たない ----
    out.AddForce = [](void* engine, MyeEntityId id, MyeVec3 f) -> int {
        auto* rb = Sc(engine)->GetWorld().GetComponent<RigidbodyComponent>(ToEngine(id));
        if (!rb || rb->isKinematic) { return 0; }
        // 固定 tick (EngineLoop kFixedDt = 1/60) 前提の 1 tick 分加速。毎 tick 呼べば連続力
        constexpr float kFixedDt = 1.0f / 60.0f;
        const float mass = (rb->mass > 0.0f) ? rb->mass : 1.0f;
        const float s = kFixedDt / mass;
        rb->velocity.x += f.x * s;
        rb->velocity.y += f.y * s;
        rb->velocity.z += f.z * s;
        return 1;
    };
    out.AddImpulse = [](void* engine, MyeEntityId id, MyeVec3 imp) -> int {
        auto* rb = Sc(engine)->GetWorld().GetComponent<RigidbodyComponent>(ToEngine(id));
        if (!rb || rb->isKinematic) { return 0; }
        const float mass = (rb->mass > 0.0f) ? rb->mass : 1.0f;
        rb->velocity.x += imp.x / mass;
        rb->velocity.y += imp.y / mass;
        rb->velocity.z += imp.z / mass;
        return 1;
    };
    out.AddTorque = [](void* engine, MyeEntityId id, MyeVec3 torque) -> int {
        // ω += I⁻¹·τ·dt を即時適用 (M28b)。AddForce と同じ「毎 tick 呼べば連続トルク」方式
        constexpr float kFixedDt = 1.0f / 60.0f;
        return ApplyTorqueWorld(Sc(engine)->GetWorld(), ToEngine(id), torque, kFixedDt);
    };
    out.GetVelocity = [](void* engine, MyeEntityId id, MyeVec3* o) -> int {
        auto* rb = Sc(engine)->GetWorld().GetComponent<RigidbodyComponent>(ToEngine(id));
        if (!rb || !o) { return 0; }
        *o = { rb->velocity.x, rb->velocity.y, rb->velocity.z };
        return 1;
    };
    out.SetVelocity = [](void* engine, MyeEntityId id, MyeVec3 v) -> int {
        auto* rb = Sc(engine)->GetWorld().GetComponent<RigidbodyComponent>(ToEngine(id));
        if (!rb) { return 0; }
        rb->velocity = { v.x, v.y, v.z };
        return 1;
    };

    // ---- 空間クエリ (v4 で予約、M28c で実装)。実装本体は PhysicsQueries.cpp ----
    out.OverlapSphere = [](void* engine, MyeVec3 center, float radius, MyeEntityId* outEntities,
                           int maxCount) -> int {
        return OverlapSphereWorld(Sc(engine)->GetWorld(), center, radius, outEntities, maxCount);
    };
    out.OverlapBox = [](void* engine, MyeVec3 center, MyeVec3 half, MyeQuat rot,
                        MyeEntityId* outEntities, int maxCount) -> int {
        return OverlapBoxWorld(Sc(engine)->GetWorld(), center, half, rot, outEntities, maxCount);
    };
    out.SphereCast = [](void* engine, MyeVec3 origin, MyeVec3 dir, float radius, float maxDist,
                        MyeRaycastHit* outHit) -> int {
        return SphereCastWorld(Sc(engine)->GetWorld(), origin, dir, radius, maxDist, outHit);
    };
}

} // namespace mye
