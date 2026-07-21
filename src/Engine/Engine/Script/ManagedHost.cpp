#include "Engine/Engine/Script/ManagedHost.h"

#include <filesystem>

#include <Windows.h>

// vendored .NET ホスティングヘッダ (external\nethost)。nethost.lib (import) + nethost.dll。
// 静的 libnethost は /MT ビルドで CRT が衝突するため import 版を使う。
#include "nethost/coreclr_delegates.h"
#include "nethost/hostfxr.h"
#include "nethost/nethost.h"

#include "Engine/Core/Archetype.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace {

hostfxr_initialize_for_runtime_config_fn g_init = nullptr;
hostfxr_get_runtime_delegate_fn g_getDelegate = nullptr;
hostfxr_close_fn g_close = nullptr;

MyeEntityId ToShared(EntityID id) { return { id.index, id.generation }; }

uint64_t StartedKey(EntityID e)
{
    return (static_cast<uint64_t>(e.index) << 32) | e.generation;
}

// MyeFieldType (managed から届く int) → エンジンの FieldType
FieldType ToFieldType(int32_t t)
{
    switch (t) {
    case 0: return FieldType::Float;
    case 1: return FieldType::Int32;
    case 2: return FieldType::UInt32;
    case 3: return FieldType::UInt64;
    case 4: return FieldType::Bool;
    case 5: return FieldType::Float2;
    case 6: return FieldType::Float3;
    case 7: return FieldType::Float4;
    case 8: return FieldType::Quat;
    case 9: return FieldType::Color;
    case 10: return FieldType::EntityRef;
    }
    return FieldType::Float;
}

// native → managed の起動引数 (Interop.cs の BootstrapArgs と一致)
struct MyeBootstrapArgs {
    const MyeEngineApi* api;
    MyeManagedVTable* outVtable;
};

// nethost で hostfxr.dll を探してロードし、必要なエントリを解決する
bool LoadHostfxr(HMODULE& outLib)
{
    wchar_t path[1024];
    size_t size = 1024;
    if (get_hostfxr_path(path, &size, nullptr) != 0) {
        MYE_LOG_WARN("[managed] hostfxr not found (.NET runtime not installed?)");
        return false;
    }
    HMODULE lib = ::LoadLibraryW(path);
    if (!lib) {
        MYE_LOG_WARN("[managed] failed to load hostfxr.dll");
        return false;
    }
    g_init = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(
        ::GetProcAddress(lib, "hostfxr_initialize_for_runtime_config"));
    g_getDelegate = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(
        ::GetProcAddress(lib, "hostfxr_get_runtime_delegate"));
    g_close = reinterpret_cast<hostfxr_close_fn>(::GetProcAddress(lib, "hostfxr_close"));
    if (!g_init || !g_getDelegate || !g_close) {
        MYE_LOG_WARN("[managed] hostfxr entrypoints missing");
        ::FreeLibrary(lib);
        return false;
    }
    outLib = lib;
    return true;
}

} // namespace

bool ManagedHost::Init(const std::wstring& exeDir, Scene* scene)
{
    scene_ = scene;
    apiCtx_.scene = scene;
    BuildEngineApi(api_, &apiCtx_);

    const std::wstring runtimeConfig = exeDir + L"\\MyeScripting.runtimeconfig.json";
    const std::wstring assemblyPath = exeDir + L"\\MyeScripting.dll";
    if (!std::filesystem::exists(runtimeConfig) || !std::filesystem::exists(assemblyPath)) {
        MYE_LOG_WARN("[managed] MyeScripting.dll not found in %s — C# scripting disabled",
                     WideToUtf8(exeDir).c_str());
        return false;
    }

    // 自己完結配布: exe 隣に同梱された dotnet\ があれば DOTNET_ROOT をそこへ向ける。
    // nethost/hostfxr が同梱ランタイムを解決するため、配布先に .NET 未導入でも動く。
    {
        const std::wstring bundled = exeDir + L"\\dotnet";
        if (std::filesystem::exists(bundled + L"\\host\\fxr")) {
            ::SetEnvironmentVariableW(L"DOTNET_ROOT", bundled.c_str());
            MYE_LOG_INFO("[managed] using bundled .NET runtime: %s", WideToUtf8(bundled).c_str());
        }
    }

    HMODULE lib = nullptr;
    if (!LoadHostfxr(lib)) {
        return false;
    }
    hostfxrLib_ = lib;

    hostfxr_handle ctx = nullptr;
    int rc = g_init(runtimeConfig.c_str(), nullptr, &ctx);
    // 成功: 0 / 1 (HostAlreadyInitialized) / 2 (DifferentRuntimeProperties)
    if ((rc != 0 && rc != 1 && rc != 2) || ctx == nullptr) {
        MYE_LOG_WARN("[managed] hostfxr_initialize failed (rc=0x%x)", static_cast<unsigned>(rc));
        if (ctx) {
            g_close(ctx);
        }
        return false;
    }
    ctx_ = ctx;

    void* loadDelegate = nullptr;
    rc = g_getDelegate(ctx, hdt_load_assembly_and_get_function_pointer, &loadDelegate);
    if (rc != 0 || loadDelegate == nullptr) {
        MYE_LOG_WARN("[managed] get_runtime_delegate failed (rc=0x%x)", static_cast<unsigned>(rc));
        return false;
    }
    auto loadAssembly = reinterpret_cast<load_assembly_and_get_function_pointer_fn>(loadDelegate);

    // Bootstrap.Initialize (component_entry_point_fn: int(void*, int)) を取得
    component_entry_point_fn bootstrap = nullptr;
    rc = loadAssembly(assemblyPath.c_str(), L"MyeScripting.Bootstrap, MyeScripting", L"Initialize",
                      nullptr, nullptr, reinterpret_cast<void**>(&bootstrap));
    if (rc != 0 || bootstrap == nullptr) {
        MYE_LOG_WARN("[managed] load Bootstrap.Initialize failed (rc=0x%x)",
                     static_cast<unsigned>(rc));
        return false;
    }

    // api テーブルと vtable 出力先を渡して Bootstrap を実行
    MyeBootstrapArgs args{ &api_, &vt_ };
    const int mrc = bootstrap(&args, static_cast<int>(sizeof(args)));
    if (mrc != 0 || vt_.Compile == nullptr) {
        MYE_LOG_WARN("[managed] Bootstrap.Initialize returned %d (vtable not filled)", mrc);
        return false;
    }

    ready_ = true;
    MYE_LOG_INFO("[managed] CoreCLR host ready (.NET 8)");
    return true;
}

void ManagedHost::Shutdown()
{
    if (ctx_ && g_close) {
        g_close(static_cast<hostfxr_handle>(ctx_));
        ctx_ = nullptr;
    }
    ready_ = false; // hostfxr はプロセス終了で解放
}

ManagedHost::CsType* ManagedHost::FindType(const std::string& name)
{
    for (CsType& t : types_) {
        if (t.name == name) {
            return &t;
        }
    }
    return nullptr;
}

const ManagedHost::CsType* ManagedHost::FindByComponent(ComponentTypeId t) const
{
    for (const CsType& c : types_) {
        if (c.componentId == t) {
            return &c;
        }
    }
    return nullptr;
}

void ManagedHost::RegisterTypes()
{
    const int32_t count = vt_.GetTypeCount();
    for (int32_t i = 0; i < count; ++i) {
        char nameBuf[256] = {};
        vt_.GetTypeName(i, nameBuf, sizeof(nameBuf));
        const std::string name = nameBuf;
        if (name.empty()) {
            continue;
        }

        CsType* type = FindType(name);
        const bool isNew = (type == nullptr);
        if (isNew) {
            types_.push_back({});
            type = &types_.back();
            type->name = name; // deque → c_str 安定
        }
        type->managedIndex = i;

        // フィールドメタデータをキャッシュ (Inspector 用)
        type->fields.clear();
        const int32_t fc = vt_.GetFieldCount(i);
        for (int32_t f = 0; f < fc; ++f) {
            char fname[128] = {};
            int32_t ftype = 0;
            vt_.GetFieldInfo(i, f, fname, sizeof(fname), &ftype);
            type->fields.push_back({ std::string(fname), ToFieldType(ftype) });
        }

        if (isNew) {
            // ECS カラムは handle (int32) のみ。実フィールドは managed オブジェクトが保持。
            ComponentDesc cd;
            cd.name = type->name.c_str();
            cd.nameHash = HashStr(type->name);
            cd.size = sizeof(int32_t);
            cd.align = alignof(int32_t);
            cd.flags = kComponentScriptState | kComponentNoHash;
            cd.construct = [](void* dst) { *static_cast<int32_t*>(dst) = 0; };
            type->componentId = ComponentRegistry::Get().Register(std::move(cd));
            MYE_LOG_INFO("[csharp] script registered: %s (%d fields)", type->name.c_str(),
                         static_cast<int>(fc));
        }
    }
}

void ManagedHost::ResetHandles()
{
    if (!scene_) {
        return;
    }
    World& world = scene_->GetWorld();
    for (CsType& type : types_) {
        if (type.componentId == kInvalidComponentType) {
            continue;
        }
        const ComponentTypeId req[] = { type.componentId };
        world.ForEachArchetype(req, [&](Archetype& arch) {
            const int ci = arch.FindTypeIndex(type.componentId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                *static_cast<int32_t*>(arch.GetPtr(ci, row)) = 0;
            }
        });
    }
    started_.clear();
}

bool ManagedHost::CompileScripts(const std::wstring& scriptsDir)
{
    if (!ready_) {
        return false;
    }
    const std::string dir = WideToUtf8(scriptsDir);
    const int32_t n = vt_.Compile(dir.c_str());
    if (n < 0) {
        return false; // 詳細エラーは managed 側がログ済み
    }
    RegisterTypes();
    ResetHandles(); // リロード: 既存コンポーネントを再インスタンス化させる
    MYE_LOG_INFO("[csharp] %d script type(s) available", static_cast<int>(n));
    return true;
}

void ManagedHost::SetTickContext(const InputSnapshot& input, uint64_t tickIndex, float dt)
{
    input_ = input;
    tickIndex_ = tickIndex;
    dt_ = dt;
    apiCtx_.input = input;
    apiCtx_.tickIndex = tickIndex;
    apiCtx_.dt = dt;
}

void ManagedHost::RunPhase(Phase phase)
{
    if (!ready_ || types_.empty()) {
        return;
    }
    World& world = scene_->GetWorld();
    for (CsType& type : types_) { // 登録順 (安定順序)
        if (type.componentId == kInvalidComponentType || type.managedIndex < 0) {
            continue;
        }
        const ComponentTypeId req[] = { type.componentId };
        world.ForEachArchetype(req, [&](Archetype& arch) {
            const int ci = arch.FindTypeIndex(type.componentId);
            const uint32_t count = arch.Count();
            for (uint32_t row = 0; row < count; ++row) {
                const EntityID e = arch.EntityAt(row);
                if (!IsEntityActive(world, e)) {
                    continue;
                }
                int32_t* handle = static_cast<int32_t*>(arch.GetPtr(ci, row));
                if (*handle == 0) {
                    // Add Component / シーンロードで付いた分をここでインスタンス化
                    *handle = vt_.CreateInstance(type.managedIndex, ToShared(e));
                    if (*handle == 0) {
                        continue;
                    }
                }
                if (phase == Phase::StartAndUpdate) {
                    const uint64_t key = StartedKey(e);
                    if (!started_.contains(key)) {
                        started_.insert(key);
                        vt_.Invoke(*handle, 0, dt_, tickIndex_); // Start
                    }
                    vt_.Invoke(*handle, 1, dt_, tickIndex_); // Update
                } else {
                    vt_.Invoke(*handle, 2, dt_, tickIndex_); // LateUpdate
                }
            }
        });
    }
}

void ManagedHost::RunStartAndUpdate() { RunPhase(Phase::StartAndUpdate); }
void ManagedHost::RunLateUpdate() { RunPhase(Phase::LateUpdate); }

void ManagedHost::DispatchTrigger(EntityID self, EntityID other, bool enter)
{
    if (!ready_ || types_.empty()) {
        return;
    }
    World& world = scene_->GetWorld();
    for (CsType& type : types_) {
        void* payload = world.GetComponentRaw(self, type.componentId);
        if (!payload) {
            continue;
        }
        const int32_t handle = *static_cast<int32_t*>(payload);
        if (handle == 0) {
            continue; // まだインスタンス化されていない
        }
        vt_.InvokeTrigger(handle, ToShared(other), enter ? 1 : 0);
    }
}

bool ManagedHost::IsManagedComponent(ComponentTypeId t) const { return FindByComponent(t) != nullptr; }

const std::vector<ManagedHost::ManagedFieldInfo>*
ManagedHost::FieldsForComponent(ComponentTypeId t) const
{
    const CsType* c = FindByComponent(t);
    return c ? &c->fields : nullptr;
}

bool ManagedHost::GetFieldValue(int32_t handle, int fieldIndex, void* buf, int bufLen)
{
    return ready_ && handle != 0 && vt_.GetFieldValue(handle, fieldIndex, buf, bufLen) != 0;
}

bool ManagedHost::SetFieldValue(int32_t handle, int fieldIndex, const void* buf, int bufLen)
{
    return ready_ && handle != 0 && vt_.SetFieldValue(handle, fieldIndex, buf, bufLen) != 0;
}

int32_t ManagedHost::EnsureInstance(ComponentTypeId t, EntityID e, void* payload)
{
    if (!ready_ || !payload) {
        return 0;
    }
    int32_t* h = static_cast<int32_t*>(payload);
    if (*h != 0) {
        return *h;
    }
    const CsType* c = FindByComponent(t);
    if (!c || c->managedIndex < 0) {
        return 0;
    }
    *h = vt_.CreateInstance(c->managedIndex, ToShared(e));
    return *h;
}

std::string ManagedHost::SerializeInstance(int32_t handle)
{
    if (!ready_ || handle == 0) {
        return {};
    }
    char buf[4096] = {};
    const int32_t need = vt_.Serialize(handle, buf, sizeof(buf));
    if (need <= 0) {
        return {};
    }
    if (need < static_cast<int32_t>(sizeof(buf))) {
        return std::string(buf, need);
    }
    // バッファ不足: 必要長で再取得
    std::string big(static_cast<size_t>(need) + 1, '\0');
    vt_.Serialize(handle, big.data(), need + 1);
    big.resize(static_cast<size_t>(need));
    return big;
}

void ManagedHost::DeserializeInstance(int32_t handle, const std::string& json)
{
    if (ready_ && handle != 0) {
        vt_.Deserialize(handle, json.c_str());
    }
}

std::string ManagedHost::SerializeComponent(ComponentTypeId t, EntityID e, void* payload)
{
    const int32_t handle = EnsureInstance(t, e, payload);
    if (handle == 0) {
        return {};
    }
    return SerializeInstance(handle);
}

void ManagedHost::DeserializeComponent(ComponentTypeId t, EntityID e, void* payload,
                                       const std::string& json)
{
    const int32_t handle = EnsureInstance(t, e, payload);
    if (handle == 0) {
        return;
    }
    DeserializeInstance(handle, json);
}

} // namespace mye
