#pragma once
#include <cstdint>
#include <deque>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "Engine/Core/EntityID.h"
#include "Engine/Core/Reflection.h" // FieldType
#include "Engine/Engine/Script/EngineApiTable.h"
#include "Engine/Engine/Script/ScriptKeys.h" // ScriptStartedKey (C++ ホストと同じキー)
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
    // v22 (M80l) 末尾追加。piece = 分かれた塊のリーダー、point/impulse は荷重最大の破片
    void (*InvokeBreak)(int32_t handle, MyeEntityId piece, MyeVec3 point, float impulse);
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

    // v3 (M19) + v6 (M32f) + v7 (M37) + v8 (M45): 共有バッファを接続する
    void SetSharedServices(std::vector<ScriptAudioEvent>* audioQueue, std::wstring* pendingScene,
                           std::vector<EffectSpawnRequest>* effectQueue = nullptr,
                           std::vector<DebugLineCmd>* debugLines = nullptr,
                           uint64_t* audioHandleSeq = nullptr,
                           const InputActions* inputActions = nullptr,
                           int* pendingSaveSlot = nullptr, int* pendingLoadSlot = nullptr,
                           PadVibrationState* padVibration = nullptr,
                           const NetRuntimeInfo* net = nullptr,
                           CursorLockState* cursorLock = nullptr,
                           int* pendingLoadPersistSlot = nullptr,
                           WindowModeState* windowMode = nullptr)
    {
        apiCtx_.pendingLoadPersistSlot = pendingLoadPersistSlot; // v16 (M70c)
        apiCtx_.windowMode = windowMode;                         // v19。null = Set が no-op
        apiCtx_.audioQueue = audioQueue;
        apiCtx_.pendingScene = pendingScene;
        apiCtx_.effectQueue = effectQueue;
        apiCtx_.debugLines = debugLines;
        apiCtx_.audioHandleSeq = audioHandleSeq;
        apiCtx_.inputActions = inputActions;
        apiCtx_.pendingSaveSlot = pendingSaveSlot;
        apiCtx_.pendingLoadSlot = pendingLoadSlot;
        apiCtx_.padVibration = padVibration;
        // v13 (M52i)。★C# レーンはネット中は止まっている (TickServices::netLockstep) ので
        //   ここが読まれるのは非ネット時だけ。それでも配線しておくのは、Interop.cs が
        //   位置ミラーで全スロットを写す以上「片方だけ null」という状態を作らないため
        apiCtx_.net = net;
        apiCtx_.cursorLock = cursorLock; // v15 (M64a)。null = 該当スロットが no-op
    }

    // v21: コンピュート ABI の実体 (ScriptHost と同一インスタンスを共有する)
    void SetComputeAbi(ComputeAbiRunner* runner, GraphicsDevice* device, ShaderManager* shaders,
                       TextureLibrary* textures)
    {
        apiCtx_.computeAbi = runner;
        apiCtx_.graphicsDevice = device;
        apiCtx_.shaderManager = shaders;
        apiCtx_.textureLibrary = textures;
    }

    // v18: 開発中の実行か (ScriptHost と同じ規約)
    void SetDevelopmentRun(bool on) { apiCtx_.developmentRun = on ? 1 : 0; }

    // v14 (M59k): 今 tick の接触列を繋ぐ / 外す (ScriptHost と同じ規約)
    void SetTickContacts(const std::vector<SolidContact>* contacts) { apiCtx_.contacts = contacts; }

    // シーン遷移 (M19.4): C# インスタンスを managed 側の辞書ごと捨て、handle を 0 にして新シーンで再生成させる (非ハッシュ)。
    // ★ResetInstances を呼ばないと、旧シーンのインスタンスが辞書に残り続ける。
    //   ホットリロード (CompileScripts) はここを通らない — managed の Compile がフィールドを退避してから辞書を空にする
    void OnSceneReloaded()
    {
        if (ready_) {
            if (vt_.ResetInstances != nullptr) {
                vt_.ResetInstances();
            }
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
    // 破壊イベント (v22、M80l)。root にあるスクリプトへ、分かれた塊のリーダー (piece)・
    // その塊で荷重最大の破片の原点 (point)・その荷重 (impulse) を渡す
    void DispatchBreak(EntityID root, EntityID piece, MyeVec3 point, float impulse);

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
    // どの C# コンポーネントからも参照されなくなったインスタンスを managed 側から消す
    // (コンポーネント除去 / エンティティ破棄の後始末)。フェーズ 3 の頭で呼ぶ
    void ReleaseOrphanInstances();

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
    std::set<ScriptStartedKey> started_;    // Start 済みインスタンス (エンティティ + スクリプト型)
    // native が CreateInstance で作り、まだ DestroyInstance していない handle。
    // ★カラムに残っていてもここに無い handle は破棄済み (スナップショット復元で戻った値など) = 作り直す
    std::unordered_set<int32_t> liveHandles_;
    InputSnapshot input_ = {};
    uint64_t tickIndex_ = 0;
    float dt_ = 1.0f / 60.0f;
};

} // namespace mye
