//====================================================================================
//                          BehaviorTreeSystem.h
//  MyEngin/ 秋田蓮音                                                     10/05/2026
//                                          ビヘイビアツリーの実行器 (M85、フェーズ 3.4a2)
//====================================================================================
#pragma once
#include <cstdint>
#include <memory>
#include <set>
#include <unordered_map>
#include <string_view>
#include <utility>
#include <vector>

#include "Engine/Core/Ecs/EntityID.h"
#include "Engine/Core/Util/ByteIo.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Engine/AI/BlackboardLibrary.h"
#include "Engine/Engine/AI/BtTaskRegistry.h"

namespace mye {

class World;
class NavSystem;
class ControllerLibrary;
class AnimationLibrary;
struct BehaviorTreeAsset;
struct BtExpansion;
struct DebugLineCmd;

// BtInstance::rootStatus。根が終わった tick の結果で、次の tick に根からやり直す
namespace btroot {
enum : uint8_t {
    kRunning = 0,
    kSucceeded = 1,
    kFailed = 2,
};
} // namespace btroot

// 実行状態の欄 1 つ。ノードごと・Decorator ごとに 1 欄 (BtDecoratorDef::slot)。種類ごとに使い方が違う (BehaviorTreeSystem.cpp が正本)。
// 固定長の POD で、BT 節とハッシュに入る。欄を足すときは kSimSnapshotVersion を上げる
struct BtNodeState {
    uint8_t active = 0;   // 入って、まだ終わっていない。子孫が 1 つでも active ならこれも 1。Decorator 欄は付いたノードが入っている間 1
    uint8_t phase = 0;    // SimpleParallel: bit0 = メインが終わった / BlackboardCondition: 最後に評価した結果 (1 = 真)
    int32_t child = 0;    // Selector / Sequence: 今の子 (BtNodeDef::children の添字)
    int32_t counter = 0;  // Wait: 残り tick / SimpleParallel: メインの結果 (1 = Success, 2 = Failure) / Cooldown: 入れるようになる tick / Repeat: 終えた回数 / Timeout: 打ち切る tick
};

// 直前の Abort (表示専用)。どのノードの Decorator が、どの実行中のタスクを止めたか。id は実行木 (BtInstance::tree) の id。
// sourceId < 0 = 記録なし。BT 節・ハッシュには入れない (巻き戻した後は空から)
struct BtAbortRecord {
    uint64_t tick = 0;
    int32_t sourceId = -1; // Decorator の付いたノード
    int32_t targetId = -1; // 止められた部分木の中で実行中だった一番深いノード
};

// 木を動かしているエンティティ 1 体ぶんの状態。BehaviorTreeComponent 1 個につき 1 つ
struct BtInstance {
    EntityID entity = kNullEntity;
    uint64_t treeGuid = 0;
    uint8_t rootStatus = btroot::kRunning;
    std::vector<BbValue> blackboard;  // 木の BB 定義のキーと同じ並び・同じ長さ
    std::vector<BtNodeState> nodes;   // 木の stateSlotCount と同じ長さ
    std::vector<uint8_t> extra;       // 種類別の追加状態 (BtNodeTypeInfo::extraStateBytes)。木の extraStateBytes と同じ長さ。ノードごとの位置は BtNodeDef::extraOffset

    // ---- 実行時だけ (BT 節に入れない。復元後は ApplySnapshot が引き直す) ----
    std::shared_ptr<const BehaviorTreeAsset> tree;
    std::shared_ptr<const BlackboardAsset> blackboardAsset; // 木が BB を使わなければ null
    bool stepLimitWarned = false;
    BtAbortRecord lastAbort;
};

constexpr int kBtMaxEventsPerTick = 256; // 配送待ちの上限 (溢れた分は捨てて 1 回だけ警告)

// 汎用イベント 1 件 (sim 状態。配送待ちは BT 節とハッシュに入る)。target が null = 全体宛て
struct BtEvent {
    uint64_t nameHash = 0;            // BtEventNameHash(名前)
    EntityID sender = kNullEntity;
    EntityID target = kNullEntity;
    float vec3[3] = {};
    float value = 0.0f;
    int32_t intValue = 0;
    uint32_t seq = 0;                 // 配送待ちの中の送信順 (0 から連番。配るときに送信元キーと組で並べ替える)
    uint64_t sentTick = 0;            // 送った tick。これより後の tick の BT フェーズ冒頭で配る
};

// イベント名のハッシュ (.bb.json の eventName・SendEvent の eventName と ABI が同じ値で突き合わせる)
inline uint64_t BtEventNameHash(std::string_view name)
{
    return HashStr(name);
}

// BT 節の読み値。木はまだ引いていない (World を差し替えた後に ApplySnapshot が引く)
struct BtSnapshot {
    std::vector<BtInstance> instances;
    std::vector<BtEvent> pending;
};

// BehaviorTreeComponent を持つエンティティの木を毎 tick 進める。
// 実行状態 (ブラックボード・ノードごとの状態) は ECS ではなくこの表が持つ — 木の大きさがアセットごとに違い、
// 固定長のコンポーネントに収まらないため (ADR-025)。sim 状態なので SimSnapshot の BT 節とワールドハッシュに入る (3 点セット契約)。
// 存在ゲート: BehaviorTreeComponent が 1 つも無く、表も空なら走査だけで何もしない (RNG もハッシュも触らない)
class BehaviorTreeSystem {
public:
    // tick ごとに呼ぶ (stepSim の中、知覚の後・ナビメッシュの前)。エンティティキー順に 1 体ずつ最後まで進める。
    // nav は FindRandomPoint / SearchArea の問い合わせ先 (読むだけ)。null の間はそれらのノードが Failure。
    // controllers / clips は PlayAnimation のステート名とクリップの長さの引き先 (読むだけ)。null の間は PlayAnimation が Failure
    void Update(World& world, uint64_t tick, const NavSystem* nav, const ControllerLibrary* controllers = nullptr,
                const AnimationLibrary* clips = nullptr);

    // 旧シーンの状態を捨てる (シーン遷移)
    void Reset();

    // ---- 汎用イベントキュー (spec 4.1.6) ----
    // tick に積んだ分は tick より後の Update の冒頭で配る (同じ tick のスクリプト層・BT のどこで積んでも次の tick)。
    // 上限 kBtMaxEventsPerTick を超えた分は捨てて false (警告は最初の 1 回だけ)。target が null = 全体宛て
    bool SendEvent(uint64_t tick, EntityID sender, EntityID target, uint64_t nameHash, const float (&vec3)[3], float value,
                   int32_t intValue);
    // tick の頭 (スクリプト層より前、stepSim の中) に 1 回呼ぶ。tick より前に積まれた分を配達済みへ移し、前の配達済みを捨てる。
    // 一時停止中は呼ばない (配送待ちは残る)。BT の Update は配達済みを BB へ反映するだけ
    void DeliverPending(uint64_t tick);
    // 配送待ちの件数 (検査用)
    int PendingEventCount() const { return static_cast<int>(pending_.size()); }
    // 直近の Update が配った分のうち self 宛て + 全体宛て (配送順 = 送信元キー → 送信順)。次の Update の冒頭で入れ替わる。
    // 配った分は BT 節に入らない (復元後は空)
    int EventCount(EntityID self) const;
    bool GetEvent(EntityID self, int index, BtEvent& out) const;

    // ---- ABI v27 (BtGetBlackboard / BtSetBlackboard / BtRestart) ----
    // キーは名前の HashStr (MyeNameHash と同じ)。BT のインスタンスが無い・キーが無いは false。
    // Update の中 (C++ タスクのコールバック) からも呼べる: 動いている最中のインスタンスも、処理済み / 未処理のインスタンスも引ける
    bool GetBlackboardValue(EntityID entity, uint64_t keyHash, BbType& type, BbValue& out) const;
    // 型が違う・非有限の Float / Vector は false (何も書かない)。isSet = 0 は未設定に戻す
    bool SetBlackboardValue(EntityID entity, uint64_t keyHash, BbType type, const BbValue& value);
    // 木を Abort して根からやり直す (BB は保つ)。entity の木のタスクの中から呼んだときは、そのタスクが返った後に行う。インスタンスが無ければ false
    bool Restart(World& world, uint64_t tick, EntityID entity);

    // C++ タスクの登録表 (ScriptHost が LoadModule のたびに更新する。BT エディタのパラメータ欄も引く)
    BtTaskRegistry& Tasks() { return tasks_; }
    const BtTaskRegistry& Tasks() const { return tasks_; }

    // Abort を受けたノードの id を Abort の順に積む先 (検査用。sim 状態ではない)。null = 記録しない
    void SetAbortTrace(std::vector<int32_t>* sink) { abortTrace_ = sink; }

    // ---- ライブ表示 (BT 窓・SceneView。読むだけ) ----
    // 入っていて終わっていないノード (Decorator だけが動いている間も含む) の inst.tree->nodes の添字を、添字順に out へ足す
    static void ActiveNodeIndices(const BtInstance& inst, std::vector<int32_t>& out);
    // BehaviorTreeComponent.drawDebug が立ったエンティティについて、実行中の MoveTo / SearchArea / Patrol の目的地などを out へ足す
    // (出力レーン。sim 状態には触れない)
    void AppendDebugLines(World& world, std::vector<DebugLineCmd>& out) const;

    // ---- SimSnapshot の BT 節 ----
    void SaveSnapshot(ByteWriter& w) const;
    // 検証つきで読む。壊れた blob (件数・範囲・キー順の不正) は false で out は不定
    static bool ReadSnapshot(ByteReader& r, BtSnapshot& out);
    // World を差し替えた後に呼ぶ。World に BehaviorTreeComponent が残っていて木が引けるものだけを復元する。
    // 木の形が保存時と違う (ノード数・BB のキー数) ものは初期状態からやり直す
    void ApplySnapshot(World& world, BtSnapshot&& snapshot);

    // ---- ワールドハッシュ ----
    // 表に 1 件でもあれば true。false なら WorldHasher は BT を何も畳まない
    // (配送待ちのイベントだけがあるときも true)
    bool HasHashableState() const { return !instances_.empty() || !pending_.empty(); }
    uint64_t StateHash() const;
    int InstanceCount() const { return static_cast<int>(instances_.size()); }

    // エンティティキー昇順
    const std::vector<BtInstance>& Instances() const { return instances_; }
    const BtInstance* FindInstance(EntityID entity) const;

private:
    void SaveInstances(ByteWriter& w) const;
    void SavePending(ByteWriter& w) const;

    // owner 1 体の同期と実行。表に残すなら true。実行中の間は inst を ABI から引ける (current_)
    bool StepOwner(World& world, uint64_t tick, const NavSystem* nav, const ControllerLibrary* controllers, const AnimationLibrary* clips,
                   EntityID owner, BtInstance& inst);
    bool StepOwnerBody(World& world, uint64_t tick, const NavSystem* nav, const ControllerLibrary* controllers,
                       const AnimationLibrary* clips, EntityID owner, BtInstance& inst);
    // entity のインスタンス (書き込める)。Update の最中は、動いている最中のもの・処理済み (next) ・未処理の表の残りの順に探す
    BtInstance* Locate(EntityID entity) const;

    // 登録された木 (GUID) を実行用に展開した結果 (SubTree の取り込み済み)。作り直しは IsCurrent で決める。木の中身だけで決まるキャッシュで、sim 状態ではない
    std::shared_ptr<const BehaviorTreeAsset> ResolveTree(uint64_t guid);

    std::vector<BtInstance> instances_; // エンティティキー昇順
    std::vector<BtEvent> pending_;      // 配送待ち (送信順 = seq 順)。sim 状態
    std::vector<BtEvent> delivered_;    // 直近の Update が配った分 (配送順)。次の Update で捨てる。sim 状態ではない
    bool eventOverflowWarned_ = false;  // ログだけ。sim 状態ではない
    std::vector<int32_t>* abortTrace_ = nullptr;
    BtTaskRegistry tasks_;              // 実行に使う登録表。sim 状態ではない (中身は GameLogic.dll の記述子)
    // ---- Update の最中だけ有効 (ABI から in-flight のインスタンスを引くための印。sim 状態ではない) ----
    BtInstance* current_ = nullptr;                  // StepOwner が動かしているインスタンス
    std::vector<BtInstance>* updateNext_ = nullptr;  // 処理済みのインスタンス (キー昇順)
    const size_t* updateOldAt_ = nullptr;            // instances_ の未処理の先頭
    bool restartRequested_ = false;                  // current_ の木のタスクが BtRestart を呼んだ
    std::set<std::string> warnedTasks_;              // 「C++ タスクが登録に無い」を警告済みの名前 (ログだけ)
    uint64_t warnedTasksGeneration_ = 0;
    std::unordered_map<uint64_t, std::shared_ptr<BtExpansion>> expansions_; // 引くだけ (走査しない)
    std::set<uint64_t> warnedMissing_;  // 「木が見つからない」を警告済みの GUID (ログだけ。sim 状態ではない)
    std::set<std::pair<uint64_t, int>> warnedEntityInit_; // 無効な Entity キーの初期値を警告済みの (エンティティ, 組の添字) (ログだけ)
};

} // namespace mye
