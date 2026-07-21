#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

#include "Engine/Core/EntityID.h"
#include "Engine/Core/Reflection.h" // FieldType
#include "Engine/Engine/Script/EngineApiTable.h"
#include "Engine/Platform/Input.h"
#include "Shared/MathPod.h" // MyeEntityId

namespace mye {

class Scene;

// native ↔ managed の関数ポインタ表。Bootstrap.Initialize がここに書き込む。
// MyeScripting の ManagedVTable (Interop.cs) とフィールド順・シグネチャを一致させること。
struct MyeManagedVTable {
    int32_t (*Compile)(const char* scriptsDirUtf8);
    int32_t (*GetTypeCount)();
    int32_t (*GetTypeName)(int32_t typeIndex, char* buf, int32_t bufLen);
    int32_t (*GetFieldCount)(int32_t typeIndex);
    int32_t (*GetFieldInfo)(int32_t typeIndex, int32_t fieldIndex, char* nameBuf, int32_t bufLen,
                            int32_t* outType);
    int32_t (*CreateInstance)(int32_t typeIndex, MyeEntityId self);
    void (*DestroyInstance)(int32_t handle);
    void (*Invoke)(int32_t handle, int32_t phase, float dt, uint64_t tick);
    void (*InvokeTrigger)(int32_t handle, MyeEntityId other, int32_t enter);
    int32_t (*GetFieldValue)(int32_t handle, int32_t fieldIndex, void* buf, int32_t bufLen);
    int32_t (*SetFieldValue)(int32_t handle, int32_t fieldIndex, const void* buf, int32_t bufLen);
    int32_t (*Serialize)(int32_t handle, char* buf, int32_t bufLen);
    void (*Deserialize)(int32_t handle, const char* json);
    void (*ResetInstances)();
    // M28c 末尾追加 (Interop.cs 側も同順で追加すること)。kind: 0=enter 1=stay 2=exit
    void (*InvokeCollision)(int32_t handle, MyeEntityId other, int32_t kind, MyeVec3 normal);
};

// CoreCLR (.NET 8) をホストし、C# スクリプト (MyeScripting.dll + Roslyn) を駆動する。
// 既存の C++ ScriptHost とは独立。C# は決定論 sim から分離した別レーンで動く
// (リプレイ記録/検証中は走らせない。ワールドハッシュ対象外 = kComponentNoHash)。
class ManagedHost {
public:
    // exeDir\MyeScripting.dll と .runtimeconfig.json を探してホスト起動。
    // 成功で true。失敗してもエンジンは継続する (C# スクリプトが使えないだけ)。
    bool Init(const std::wstring& exeDir, Scene* scene);
    void Shutdown();
    bool IsReady() const { return ready_; }

    // scriptsDir 内の *.cs を Roslyn でコンパイルし、各 C# 型を ECS コンポーネント登録する。
    // 成功で true。リロード時は既存インスタンスの handle をリセットして再生成させる。
    bool CompileScripts(const std::wstring& scriptsDir);
    uint32_t ScriptTypeCount() const { return static_cast<uint32_t>(types_.size()); }

    // v3 (M19): audioQueue / pendingScene の共有バッファを context に接続する (Init 後に一度)
    void SetSharedServices(std::vector<ScriptAudioEvent>* audioQueue, std::wstring* pendingScene)
    {
        apiCtx_.audioQueue = audioQueue;
        apiCtx_.pendingScene = pendingScene;
    }

    // シーン遷移 (M19.4): C# インスタンス handle をリセットして新シーンで再生成させる (非ハッシュ)
    void OnSceneReloaded()
    {
        if (ready_) {
            ResetHandles();
        }
    }

    // 毎 tick、フェーズ 3/5 で呼ぶ (Play 中かつ非リプレイ時のみ)
    void SetTickContext(const InputSnapshot& input, uint64_t tickIndex, float dt);
    void RunStartAndUpdate(); // フェーズ 3: 新規インスタンスの Start → 全 Update
    void RunLateUpdate();     // フェーズ 5
    void DispatchTrigger(EntityID self, EntityID other, bool enter);
    // ソリッド衝突イベント (M28c)。kind: 0=enter 1=stay 2=exit。normal は相手→自分 (ワールド)
    void DispatchCollision(EntityID self, EntityID other, int kind, MyeVec3 normal);

    // ---- Inspector / シリアライズ連携 ----
    struct ManagedFieldInfo {
        std::string name;
        FieldType type;
    };
    bool IsManagedComponent(ComponentTypeId t) const;
    const std::vector<ManagedFieldInfo>* FieldsForComponent(ComponentTypeId t) const;
    bool GetFieldValue(int32_t handle, int fieldIndex, void* buf, int bufLen);
    bool SetFieldValue(int32_t handle, int fieldIndex, const void* buf, int bufLen);
    // 編集モードでも Inspector がフィールドを読めるよう、instance を必要時に生成する。
    // payload (ECS の {int32 handle}) を読み書きし、handle を返す (0 = 失敗)
    int32_t EnsureInstance(ComponentTypeId t, EntityID e, void* payload);
    // シーン保存/復元用 (Phase 3)。handle は当該コンポーネント先頭 int32
    std::string SerializeInstance(int32_t handle);
    void DeserializeInstance(int32_t handle, const std::string& json);
    // payload (ECS の {int32 handle}) からインスタンスを用意してフィールドを JSON 化/復元する。
    // SceneSerializer が C# コンポーネントのフィールド永続に使う
    std::string SerializeComponent(ComponentTypeId t, EntityID e, void* payload);
    void DeserializeComponent(ComponentTypeId t, EntityID e, void* payload, const std::string& json);

private:
    enum class Phase { StartAndUpdate, LateUpdate };
    void RunPhase(Phase phase);
    void RegisterTypes(); // Compile 後に呼ぶ
    void ResetHandles();  // 全 C# コンポーネントの handle を 0 に (リロード後の再生成用)

    struct CsType {
        std::string name; // C# 型の FullName (エンジン側コピー)
        ComponentTypeId componentId = kInvalidComponentType;
        int32_t managedIndex = -1; // リロードで変わり得る managed 側の型インデックス
        std::vector<ManagedFieldInfo> fields;
    };
    CsType* FindType(const std::string& name);
    const CsType* FindByComponent(ComponentTypeId t) const;

    bool ready_ = false;
    void* hostfxrLib_ = nullptr; // HMODULE
    void* ctx_ = nullptr;        // hostfxr_handle
    Scene* scene_ = nullptr;
    ScriptApiContext apiCtx_ = {}; // api_ の engine が指すコンテキスト
    MyeEngineApi api_ = {};
    MyeManagedVTable vt_ = {};
    std::deque<CsType> types_;              // deque: name の c_str() 安定性のため
    std::unordered_set<uint64_t> started_;  // Start 済みインスタンス
    InputSnapshot input_ = {};
    uint64_t tickIndex_ = 0;
    float dt_ = 1.0f / 60.0f;
};

} // namespace mye
