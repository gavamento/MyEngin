#pragma once
#include <deque>
#include <string>
#include <unordered_set>

#include "Engine/Core/EntityID.h"
#include "Engine/Engine/Script/EngineApiTable.h"
#include "Engine/Platform/Input.h"
#include "Shared/ScriptTypes.h"

namespace mye {

class Scene;

// GameLogic.dll のホスト (engine_spec.md 5.2 / 8.4)。
// - スクリプト型ごとに動的 ECS コンポーネントを登録する
//   → 状態はアーキタイプカラムに常駐し、Inspector / シリアライズ / ハッシュに自動対応
// - DLL リロード時: レイアウト一致なら関数再バインドのみ、
//   不一致なら World::ReplaceComponentStorage でフィールド単位の移行
// - 実行順: スクリプト型の登録順 → アーキタイプ順 → 行順 (決定論、spec 5.3)
class ScriptHost {
public:
    void Init(Scene* scene);
    void Shutdown();

    // コピーされた DLL をロードして関数テーブルを再バインドする。
    // 旧モジュールは成功時のみ解放される (失敗時は旧ロジックで継続)
    bool LoadModule(const std::wstring& dllPath);
    bool IsLoaded() const { return module_ != nullptr; }
    uint32_t ScriptTypeCount() const { return static_cast<uint32_t>(types_.size()); }

    // v3 (M19) + v6 (M32f) + v7 (M37) + v8 (M45): 共有バッファを接続する
    void SetSharedServices(std::vector<ScriptAudioEvent>* audioQueue, std::wstring* pendingScene,
                           std::vector<EffectSpawnRequest>* effectQueue = nullptr,
                           std::vector<DebugLineCmd>* debugLines = nullptr,
                           uint64_t* audioHandleSeq = nullptr)
    {
        apiCtx_.audioQueue = audioQueue;
        apiCtx_.pendingScene = pendingScene;
        apiCtx_.effectQueue = effectQueue;
        apiCtx_.debugLines = debugLines;
        apiCtx_.audioHandleSeq = audioHandleSeq;
    }

    // シーン遷移 (M19.4): Start 済み記録をクリアして新シーンのエンティティで Start を再実行させる
    void ClearStarted() { started_.clear(); }

    // 毎 tick、フェーズ 3/5 で呼ぶ (Play 中のみ)
    void SetTickContext(const InputSnapshot& input, uint64_t tickIndex, float dt);
    void RunStartAndUpdate(); // フェーズ 3: 新規インスタンスの Start → 全 Update
    void RunLateUpdate();     // フェーズ 5

    // CollisionSystem からのトリガーイベント配信 (self のスクリプトへ)
    void DispatchTrigger(EntityID self, EntityID other, bool enter);
    // ソリッド衝突イベント配信 (M28c)。kind: 0=enter 1=stay 2=exit。
    // normal は「相手→自分」方向 (ワールド)。enter のみ normal 付きで届く
    void DispatchCollision(EntityID self, EntityID other, int kind, MyeVec3 normal);

private:
    struct ScriptType {
        std::string name; // エンジン側コピー (DLL 文字列に依存しない)
        ComponentTypeId componentId = kInvalidComponentType;
        uint64_t layoutHash = 0;
        // 現行 DLL 内の関数 (リロードで差し替わる)。orphan (新 DLL に型が無い) なら null
        void (*start)(void*, MyeUpdateContext*) = nullptr;
        void (*update)(void*, MyeUpdateContext*) = nullptr;
        void (*lateUpdate)(void*, MyeUpdateContext*) = nullptr;
        void (*onTriggerEnter)(void*, MyeUpdateContext*, MyeEntityId) = nullptr;
        void (*onTriggerExit)(void*, MyeUpdateContext*, MyeEntityId) = nullptr;
        void (*onCollisionEnter)(void*, MyeUpdateContext*, MyeEntityId, MyeVec3) = nullptr;
        void (*onCollisionStay)(void*, MyeUpdateContext*, MyeEntityId) = nullptr;
        void (*onCollisionExit)(void*, MyeUpdateContext*, MyeEntityId) = nullptr;
    };

    enum class Phase { StartAndUpdate, LateUpdate };
    void RunPhase(Phase phase);
    void BuildApiTable();
    ScriptType* FindType(const char* name);

    Scene* scene_ = nullptr;
    void* module_ = nullptr; // HMODULE (現行 DLL)
    ScriptApiContext apiCtx_ = {}; // api_ の engine が指すコンテキスト (安定アドレス)
    MyeEngineApi api_ = {};
    std::deque<ScriptType> types_; // deque: name の c_str() 安定性のため
    std::unordered_set<uint64_t> started_; // (index<<32|generation) — Start 済みインスタンス

    // tick コンテキスト
    InputSnapshot input_ = {};
    uint64_t tickIndex_ = 0;
    float dt_ = 1.0f / 60.0f;
};

} // namespace mye
