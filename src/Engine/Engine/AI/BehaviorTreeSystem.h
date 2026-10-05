//====================================================================================
//                          BehaviorTreeSystem.h
//  MyEngin/ 秋田蓮音                                                     10/05/2026
//                                          ビヘイビアツリーの実行器 (M85、フェーズ 3.4a2)
//====================================================================================
#pragma once
#include <cstdint>
#include <memory>
#include <set>
#include <vector>

#include "Engine/Core/Ecs/EntityID.h"
#include "Engine/Core/Util/ByteIo.h"
#include "Engine/Engine/AI/BlackboardLibrary.h"

namespace mye {

class World;
struct BehaviorTreeAsset;

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
};

// BT 節の読み値。木はまだ引いていない (World を差し替えた後に ApplySnapshot が引く)
struct BtSnapshot {
    std::vector<BtInstance> instances;
};

// BehaviorTreeComponent を持つエンティティの木を毎 tick 進める。
// 実行状態 (ブラックボード・ノードごとの状態) は ECS ではなくこの表が持つ — 木の大きさがアセットごとに違い、
// 固定長のコンポーネントに収まらないため (ADR-025)。sim 状態なので SimSnapshot の BT 節とワールドハッシュに入る (3 点セット契約)。
// 存在ゲート: BehaviorTreeComponent が 1 つも無く、表も空なら走査だけで何もしない (RNG もハッシュも触らない)
class BehaviorTreeSystem {
public:
    // tick ごとに呼ぶ (stepSim の中、知覚の後・ナビメッシュの前)。エンティティキー順に 1 体ずつ最後まで進める
    void Update(World& world, uint64_t tick);

    // 旧シーンの状態を捨てる (シーン遷移)
    void Reset();

    // Abort を受けたノードの id を Abort の順に積む先 (検査用。sim 状態ではない)。null = 記録しない
    void SetAbortTrace(std::vector<int32_t>* sink) { abortTrace_ = sink; }

    // ---- SimSnapshot の BT 節 ----
    void SaveSnapshot(ByteWriter& w) const;
    // 検証つきで読む。壊れた blob (件数・範囲・キー順の不正) は false で out は不定
    static bool ReadSnapshot(ByteReader& r, BtSnapshot& out);
    // World を差し替えた後に呼ぶ。World に BehaviorTreeComponent が残っていて木が引けるものだけを復元する。
    // 木の形が保存時と違う (ノード数・BB のキー数) ものは初期状態からやり直す
    void ApplySnapshot(World& world, BtSnapshot&& snapshot);

    // ---- ワールドハッシュ ----
    // 表に 1 件でもあれば true。false なら WorldHasher は BT を何も畳まない
    bool HasHashableState() const { return !instances_.empty(); }
    uint64_t StateHash() const;
    int InstanceCount() const { return static_cast<int>(instances_.size()); }

    // エンティティキー昇順
    const std::vector<BtInstance>& Instances() const { return instances_; }
    const BtInstance* FindInstance(EntityID entity) const;

private:
    // owner 1 体の同期と実行。表に残すなら true
    bool StepOwner(World& world, uint64_t tick, EntityID owner, BtInstance& inst);

    std::vector<BtInstance> instances_; // エンティティキー昇順
    std::vector<int32_t>* abortTrace_ = nullptr;
    std::set<uint64_t> warnedMissing_;  // 「木が見つからない」を警告済みの GUID (ログだけ。sim 状態ではない)
};

} // namespace mye
