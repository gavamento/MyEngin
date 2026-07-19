#include "Engine/Engine/Script/ScriptHost.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include <Windows.h>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace {

// Shared 側 POD とエンジン型のバイナリ互換を保証 (DLL 境界の前提)
static_assert(sizeof(MyeEntityId) == sizeof(EntityID));
static_assert(offsetof(MyeEntityId, index) == offsetof(EntityID, index));
static_assert(offsetof(MyeEntityId, generation) == offsetof(EntityID, generation));

EntityID ToEngine(MyeEntityId id) { return { id.index, id.generation }; }
MyeEntityId ToShared(EntityID id) { return { id.index, id.generation }; }

FieldType ToFieldType(int32_t t)
{
    switch (t) {
    case MYE_FIELD_FLOAT:    return FieldType::Float;
    case MYE_FIELD_INT32:    return FieldType::Int32;
    case MYE_FIELD_UINT32:   return FieldType::UInt32;
    case MYE_FIELD_UINT64:   return FieldType::UInt64;
    case MYE_FIELD_BOOL:     return FieldType::Bool;
    case MYE_FIELD_FLOAT2:   return FieldType::Float2;
    case MYE_FIELD_FLOAT3:   return FieldType::Float3;
    case MYE_FIELD_FLOAT4:   return FieldType::Float4;
    case MYE_FIELD_QUAT:     return FieldType::Quat;
    case MYE_FIELD_COLOR:    return FieldType::Color;
    case MYE_FIELD_ENTITYREF: return FieldType::EntityRef;
    }
    return FieldType::Float;
}

uint64_t StartedKey(EntityID e)
{
    return (static_cast<uint64_t>(e.index) << 32) | e.generation;
}

ScriptHost* Host(void* engine) { return static_cast<ScriptHost*>(engine); }

} // namespace

// ---- C ABI テーブル構築 ----
// engine ポインタは常に ScriptHost*。キャプチャなしラムダ → 関数ポインタ変換で
// extern "C" スタイルのテーブルを埋める (メンバ関数内ラムダなので private に触れる)
void ScriptHost::BuildApiTable()
{
    api_ = {};
    api_.version = MYE_API_VERSION;
    api_.engine = this;

    api_.Log = [](void* engine, int level, const char* msg) {
        (void)engine;
        logging::Write(static_cast<LogLevel>(level & 3), "[script] %s", msg);
    };
    api_.KeyDown = [](void* engine, uint8_t vk) -> int {
        return Host(engine)->input_.KeyDown(vk) ? 1 : 0;
    };
    api_.MouseButton = [](void* engine, int button) -> int {
        return Host(engine)->input_.MouseDown(button) ? 1 : 0;
    };
    api_.MousePos = [](void* engine, int32_t* x, int32_t* y) {
        if (x) { *x = Host(engine)->input_.mouseX; }
        if (y) { *y = Host(engine)->input_.mouseY; }
    };
    api_.CreateGameObject = [](void* engine, const char* name) -> MyeEntityId {
        return ToShared(Host(engine)->scene_->GetWorld().CreateEntity(name ? name : "GameObject"));
    };
    api_.DestroyGameObject = [](void* engine, MyeEntityId id) {
        Host(engine)->scene_->GetWorld().DestroyEntity(ToEngine(id));
    };
    api_.IsAlive = [](void* engine, MyeEntityId id) -> int {
        return Host(engine)->scene_->GetWorld().IsAlive(ToEngine(id)) ? 1 : 0;
    };
    api_.FindByName = [](void* engine, const char* name) -> MyeEntityId {
        return ToShared(Host(engine)->scene_->Find(name ? name : "").Id());
    };
    api_.SetParent = [](void* engine, MyeEntityId child, MyeEntityId parent) {
        Host(engine)->scene_->GetWorld().SetParent(ToEngine(child), ToEngine(parent));
    };
    api_.GetLocalPosition = [](void* engine, MyeEntityId id, MyeVec3* out) -> int {
        auto* t = Host(engine)->scene_->GetWorld().GetComponent<LocalTransform>(ToEngine(id));
        if (!t || !out) { return 0; }
        *out = { t->position.x, t->position.y, t->position.z };
        return 1;
    };
    api_.SetLocalPosition = [](void* engine, MyeEntityId id, MyeVec3 v) -> int {
        auto* t = Host(engine)->scene_->GetWorld().GetComponent<LocalTransform>(ToEngine(id));
        if (!t) { return 0; }
        t->position = { v.x, v.y, v.z };
        return 1;
    };
    api_.GetLocalRotation = [](void* engine, MyeEntityId id, MyeQuat* out) -> int {
        auto* t = Host(engine)->scene_->GetWorld().GetComponent<LocalTransform>(ToEngine(id));
        if (!t || !out) { return 0; }
        *out = { t->rotation.x, t->rotation.y, t->rotation.z, t->rotation.w };
        return 1;
    };
    api_.SetLocalRotation = [](void* engine, MyeEntityId id, MyeQuat q) -> int {
        auto* t = Host(engine)->scene_->GetWorld().GetComponent<LocalTransform>(ToEngine(id));
        if (!t) { return 0; }
        t->rotation = { q.x, q.y, q.z, q.w };
        return 1;
    };
    api_.GetLocalScale = [](void* engine, MyeEntityId id, MyeVec3* out) -> int {
        auto* t = Host(engine)->scene_->GetWorld().GetComponent<LocalTransform>(ToEngine(id));
        if (!t || !out) { return 0; }
        *out = { t->scale.x, t->scale.y, t->scale.z };
        return 1;
    };
    api_.SetLocalScale = [](void* engine, MyeEntityId id, MyeVec3 v) -> int {
        auto* t = Host(engine)->scene_->GetWorld().GetComponent<LocalTransform>(ToEngine(id));
        if (!t) { return 0; }
        t->scale = { v.x, v.y, v.z };
        return 1;
    };
    api_.RandomFloat01 = [](void* engine) -> float {
        return Host(engine)->scene_->GetWorld().Rng().NextFloat01();
    };
    api_.RandomRange = [](void* engine, float lo, float hi) -> float {
        return Host(engine)->scene_->GetWorld().Rng().Range(lo, hi);
    };
    api_.AddComponentByName = [](void* engine, MyeEntityId id, const char* name) -> int {
        if (!name) {
            return 0;
        }
        const ComponentTypeId t = ComponentRegistry::Get().FindByName(name);
        if (t == kInvalidComponentType) {
            return 0;
        }
        return Host(engine)->scene_->GetWorld().AddComponentRaw(ToEngine(id), t) ? 1 : 0;
    };
    api_.SetMeshRenderer = [](void* engine, MyeEntityId id, const char* meshKey,
                              const char* materialKey) -> int {
        World& world = Host(engine)->scene_->GetWorld();
        auto* mr = static_cast<MeshRendererComponent*>(
            world.AddComponentRaw(ToEngine(id), MeshRendererComponent::sTypeId));
        if (!mr) {
            return 0;
        }
        // アセットキー名 → AssetID (ハッシュ)。実体解決は描画時にライブラリが行う
        if (meshKey) {
            mr->mesh = AssetID{ HashStr(meshKey) };
        }
        if (materialKey) {
            mr->material = AssetID{ HashStr(materialKey) };
        }
        return 1;
    };
}

void ScriptHost::DispatchTrigger(EntityID self, EntityID other, bool enter)
{
    World& world = scene_->GetWorld();
    for (ScriptType& type : types_) { // 登録順 (決定論)
        auto fn = enter ? type.onTriggerEnter : type.onTriggerExit;
        if (!fn) {
            continue;
        }
        void* state = world.GetComponentRaw(self, type.componentId);
        if (!state) {
            continue;
        }
        MyeUpdateContext ctx;
        ctx.dt = dt_;
        ctx.tickIndex = tickIndex_;
        ctx.self = ToShared(self);
        ctx.api = &api_;
        fn(state, &ctx, ToShared(other));
    }
}

void ScriptHost::Init(Scene* scene)
{
    scene_ = scene;
    BuildApiTable();
}

void ScriptHost::Shutdown()
{
    if (module_) {
        FreeLibrary(static_cast<HMODULE>(module_));
        module_ = nullptr;
    }
}

ScriptHost::ScriptType* ScriptHost::FindType(const char* name)
{
    for (ScriptType& t : types_) {
        if (t.name == name) {
            return &t;
        }
    }
    return nullptr;
}

bool ScriptHost::LoadModule(const std::wstring& dllPath)
{
    HMODULE fresh = LoadLibraryW(dllPath.c_str());
    if (!fresh) {
        MYE_LOG_ERROR("[dll] LoadLibrary failed: %s (%lu)", WideToUtf8(dllPath).c_str(),
                      GetLastError());
        return false;
    }
    auto getModule =
        reinterpret_cast<MyeGetModuleFn>(GetProcAddress(fresh, "GameLogic_GetModule"));
    if (!getModule) {
        MYE_LOG_ERROR("[dll] GameLogic_GetModule not found");
        FreeLibrary(fresh);
        return false;
    }
    const MyeScriptModule* mod = getModule(&api_);
    if (!mod || mod->apiVersion != MYE_API_VERSION) {
        MYE_LOG_ERROR("[dll] API version mismatch (dll=%u, engine=%u) - rebuild GameLogic",
                      mod ? mod->apiVersion : 0, MYE_API_VERSION);
        FreeLibrary(fresh);
        return false;
    }

    World& world = scene_->GetWorld();

    // 名前順に登録する。DLL 内の静的初期化順 (= TU のリンク順) は Debug/Release で
    // 異なり得るため、実行順と TypeId 割り当てを構成非依存にする (spec 11 章)
    std::vector<const MyeScriptDesc*> sorted;
    sorted.reserve(mod->scriptCount);
    for (uint32_t i = 0; i < mod->scriptCount; ++i) {
        sorted.push_back(&mod->scripts[i]);
    }
    std::sort(sorted.begin(), sorted.end(), [](const MyeScriptDesc* a, const MyeScriptDesc* b) {
        return strcmp(a->name, b->name) < 0;
    });

    // 新 DLL に存在する型を反映
    std::unordered_set<std::string> present;
    for (const MyeScriptDesc* sdp : sorted) {
        const MyeScriptDesc& sd = *sdp;
        present.insert(sd.name);

        ScriptType* type = FindType(sd.name);
        const bool isNew = (type == nullptr);
        if (isNew) {
            types_.push_back({});
            type = &types_.back();
            type->name = sd.name; // コピー (旧 DLL 解放後も有効)
        }

        // ComponentDesc を構築 (名前は ScriptType::name — deque なので c_str 安定)
        ComponentDesc cd;
        cd.name = type->name.c_str();
        cd.nameHash = HashStr(type->name);
        cd.size = sd.stateSize;
        cd.align = sd.stateAlign;
        cd.flags = kComponentScriptState;
        cd.construct = sd.construct;
        cd.fields.reserve(sd.fieldCount);
        for (uint32_t f = 0; f < sd.fieldCount; ++f) {
            FieldDesc fd;
            fd.name = _strdup(sd.fields[f].name); // 永続コピー (小規模なので解放しない)
            fd.type = ToFieldType(sd.fields[f].type);
            fd.offset = sd.fields[f].offset;
            fd.flags = kFieldNone;
            cd.fields.push_back(fd);
        }

        if (isNew) {
            type->componentId = ComponentRegistry::Get().Register(std::move(cd));
            MYE_LOG_INFO("[dll] script registered: %s (%u bytes, %u fields)", type->name.c_str(),
                         sd.stateSize, sd.fieldCount);
        } else if (type->layoutHash != sd.layoutHash
                   || ComponentRegistry::Get().Desc(type->componentId).size != sd.stateSize) {
            // レイアウト変更 → フィールド単位の移行 (名前+型一致のみ保持)
            world.ReplaceComponentStorage(type->componentId, std::move(cd));
            MYE_LOG_INFO("[dll] script layout migrated: %s", type->name.c_str());
        } else {
            // レイアウト同一でも関数ポインタ (construct) は新 DLL に差し替える
            ComponentRegistry::Get().UpdateDesc(type->componentId, std::move(cd));
        }

        type->layoutHash = sd.layoutHash;
        type->start = sd.start;
        type->update = sd.update;
        type->lateUpdate = sd.lateUpdate;
        type->onTriggerEnter = sd.onTriggerEnter;
        type->onTriggerExit = sd.onTriggerExit;
    }

    // 新 DLL に無くなった型: ロジックを外す (状態は残す — 復活したら再バインドされる)
    for (ScriptType& t : types_) {
        if (!present.contains(t.name)) {
            if (t.update || t.start || t.lateUpdate) {
                MYE_LOG_WARN("[dll] script '%s' removed from module (state kept, logic detached)",
                             t.name.c_str());
            }
            t.start = nullptr;
            t.update = nullptr;
            t.lateUpdate = nullptr;
            t.onTriggerEnter = nullptr;
            t.onTriggerExit = nullptr;
        }
    }

    // 旧 DLL を解放 (関数テーブルは全て差し替え済み。フェーズ 2 = スクリプト非実行中)
    if (module_) {
        FreeLibrary(static_cast<HMODULE>(module_));
    }
    module_ = fresh;
    MYE_LOG_INFO("[dll] GameLogic loaded: %s (%u scripts)", WideToUtf8(dllPath).c_str(),
                 mod->scriptCount);
    return true;
}

void ScriptHost::SetTickContext(const InputSnapshot& input, uint64_t tickIndex, float dt)
{
    input_ = input;
    tickIndex_ = tickIndex;
    dt_ = dt;
}

void ScriptHost::RunPhase(Phase phase)
{
    World& world = scene_->GetWorld();
    for (ScriptType& type : types_) { // 登録順 (決定論)
        const bool wantStart = (phase == Phase::StartAndUpdate) && type.start != nullptr;
        auto fn = (phase == Phase::StartAndUpdate) ? type.update : type.lateUpdate;
        if (!fn && !wantStart) {
            continue;
        }
        const ComponentTypeId req[] = { type.componentId };
        world.ForEachArchetype(req, [&](Archetype& arch) {
            const int ci = arch.FindTypeIndex(type.componentId);
            const uint32_t count = arch.Count(); // 開始時の行数で固定
            for (uint32_t row = 0; row < count; ++row) {
                if (!IsEntityActive(world, arch.EntityAt(row))) {
                    continue; // 無効エンティティのスクリプトは走らせない (M10)
                }
                MyeUpdateContext ctx;
                ctx.dt = dt_;
                ctx.tickIndex = tickIndex_;
                ctx.self = ToShared(arch.EntityAt(row));
                ctx.api = &api_;
                void* state = arch.GetPtr(ci, row);
                if (wantStart) {
                    const uint64_t key = StartedKey(arch.EntityAt(row));
                    if (!started_.contains(key)) {
                        started_.insert(key);
                        type.start(state, &ctx);
                    }
                }
                if (fn) {
                    fn(state, &ctx);
                }
            }
        });
    }
}

void ScriptHost::RunStartAndUpdate()
{
    RunPhase(Phase::StartAndUpdate);
}

void ScriptHost::RunLateUpdate()
{
    RunPhase(Phase::LateUpdate);
}

} // namespace mye
