//====================================================================================
//                          NavSystem.h
//  MyEngin/ 秋田蓮音                                                     10/03/2026
//                                          ナビメッシュの実行時の持ち主 (Surface ごとの読み込み・Agent の移動・スナップショット)
//====================================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "Engine/Core/Ecs/EntityID.h"
#include "Engine/Engine/Navigation/NavMeshAsset.h"
#include "Engine/Engine/Rendering/DebugDraw.h"

namespace mye {

class World;
struct NavMeshAgentComponent;
struct NavMeshObstacleComponent;
struct NavMeshModifierComponent;
struct CharacterControllerComponent;
struct LocalTransform;

enum class NavSurfaceState : uint8_t {
    NoAsset, // navAsset が未設定 (未ベイク)
    Loaded,
    Failed,  // 読み込み失敗。この Surface だけ無効で、他の Surface は動く
};

// dtCrowd の 1 スロットが誰のものか、と NavSystem が持つ Agent ごとの記録。
// dtCrowd 自身が持てない状態 (要求済みの目的地・到着済みか) をここに置く。スナップショット対象
struct NavAgentSlot {
    EntityID entity = kNullEntity; // kNullEntity = 空きスロット
    uint8_t requested = 0;         // requestedDest を crowd へ要求済み (到達不能として記録した場合も 1)
    uint8_t destInvalid = 0;       // 目的地の近くにナビメッシュが無かった
    uint8_t arrived = 0;           // 到着して crowd の目標を外した
    uint8_t stuck = 0;             // 詰まり検出で止めた (目的地が変わるまで)
    int32_t noProgressTicks = 0;   // 最良の残り距離を縮められていない tick 数
    float bestRemaining = -1.0f;   // 進捗の基準にしている残り距離。負 = 未設定
    float requestedDest[3] = {};
};

// NavMeshObstacle の形をワールドの NavObstacleSpec にする。m は行ベクトル規約の 4x4 ワールド行列 (行 3 が平行移動)。
// Box は回転後の AABB、Cylinder は y 回転を無視した底面中心・半径 (max(|sx|, |sz|) 倍)・高さ (|sy| 倍)。
// 寸法が非有限・0 以下なら false (その障害物は無いものとして扱う)
bool NavMakeObstacleSpec(const NavMeshObstacleComponent& obstacle, const float (&m)[4][4], uint64_t key,
                         NavObstacleSpec& out);

// エンティティキーから障害物のキーを作る。キー順 == (index, generation) 順
uint64_t NavObstacleKey(EntityID entity);

// NavMeshModifier の store 上のキー。Obstacle と同じ store の一覧に入るので、同じエンティティでも衝突しないよう
// 最上位ビットを立てる (index は 2^31 未満)。キー順 == (index, generation) 順は Modifier どうしの間で保たれる
uint64_t NavModifierKey(EntityID entity);

// NavMeshModifier の箱をワールドの NavObstacleSpec (area 付き) にする。箱の扱いは NavMakeObstacleSpec の Box と同じ。
// area は 0..15 に丸める。寸法が非有限・0 以下なら false
bool NavMakeModifierSpec(const NavMeshModifierComponent& modifier, const float (&m)[4][4], uint64_t key,
                         NavObstacleSpec& out);

// World の有効な NavMeshModifier を key 昇順に集める (out は上書き)。配置は NavSystem の Obstacle と同じ規則
void NavCollectModifierSpecs(World& world, std::vector<NavObstacleSpec>& out);

// all のうち、Surface のベイク範囲 (ワールド AABB) と重なるものだけを out に入れる (順序は保つ)
void NavFilterSpecsToSurface(World& world, EntityID surface, const std::vector<NavObstacleSpec>& all,
                             std::vector<NavObstacleSpec>& out);

// store の塗り替え (Modifier) を wanted に合わせ、変えたら Commit まで済ませる。差分が無ければ何もしない。
// 編集中の表示 (NavDebugView) と NavSystem が同じ関数で適用する。戻り値は変えた数 (失敗は failures に加算)
int NavApplyModifiers(NavTileStore& store, const std::vector<NavObstacleSpec>& wanted, int& failures);

struct NavQueryDeleter {
    void operator()(dtNavMeshQuery* query) const;
};
struct NavCrowdDeleter {
    void operator()(dtCrowd* crowd) const;
};

// Surface 1 つ分の実行時の状態。層 (TileCache の入力) と dtNavMesh は store が所有する
struct NavSurfaceRuntime {
    EntityID entity;
    uint64_t assetGuid = 0;
    NavSurfaceState state = NavSurfaceState::NoAsset;
    std::unique_ptr<NavTileStore> store;
    std::unique_ptr<dtNavMeshQuery, NavQueryDeleter> query; // Agent の置き場所・目的地の最近点用
    std::unique_ptr<dtCrowd, NavCrowdDeleter> crowd;
    std::vector<NavAgentSlot> slots; // crowd の容量と同じ長さ
    int layerCount = 0;
    int tileCount = 0;
    int polyCount = 0;
    int OccupiedSlots() const;
};

// 計測用 (sim 状態ではない。ハッシュにもスナップショットにも入らない)
struct NavSystemStats {
    int agents = 0;             // 直近の Update で crowd に載っていた Agent 数
    double updateUs = 0.0;      // 直近の Update 全体
    double crowdUpdateUs = 0.0; // 直近の dtCrowd::update の合計
    double maxCrowdUpdateUs = 0.0; // Reset 以降の最大
    double obstacleUs = 0.0;       // 直近の障害物の同期 (TileCache の更新 + Commit)。変更が無い tick は 0
    double maxObstacleUs = 0.0;    // Reset 以降の最大
    int obstacleChanges = 0;       // 直近の同期で足した / 外した障害物の数
};

// Surface の .mnav を読み込んで dtNavMesh を持ち、NavMeshAgent を dtCrowd で歩かせる。
// 状態は 2 種類ある:
//   - ナビメッシュ (store): アセットから作る導出値。Obstacle 等で変わった分は Nav 節に入る
//   - crowd と slots: sim 状態。SimSnapshot の Nav 節 (SaveSnapshot / ApplySnapshot) と
//     ワールドハッシュ (StateHash) に入る。Agent が 1 体も居なければハッシュには何も足さない
class NavSystem {
public:
    // tick ごとに呼ぶ (stepSim の中、物理より前)。Surface の構成 (エンティティ・navAsset) が
    // 変わっていたら読み直し、NavMeshObstacle の増減・移動を TileCache へ同期確定し (Commit)、
    // Agent を crowd と同期して dt だけ進め、CharacterController.moveInput を書く。
    // .mnav の読み込みはメインスレッド専用 (ファイル I/O + ログ)
    void Update(World& world, float dt);

    // 旧シーンの NavMesh を捨てる (シーン遷移)。次の Update が読み直す
    void Reset();

    // drawAgentPaths の立った Surface の Agent の経路線を out へ足す。描画レーン (sim 状態に触れない)。
    // ナビメッシュ自体の輪郭・塗りは NavDebugView (NavDebugDraw.h) が持つ
    void AppendDebugLines(World& world, std::vector<DebugLineCmd>& out) const;

    // ---- SimSnapshot の Nav 節 ----
    // Loaded な Surface ごとに store の状態・crowd・slots を書く。経路要求が途中の crowd があると false
    bool SaveSnapshot(NavByteWriter& writer) const;
    // 書式の検査だけ (何も書き換えない)。RestoreSimSnapshot が World を差し替える前に呼ぶ
    static bool ValidateSnapshot(const uint8_t* data, size_t size);
    // World を差し替えた後に呼ぶ。World の Surface を読み込み (.mnav の読み込みを Nav 節を当てる前に済ませる =
    // 空の NavSystem へ復元した直後の Update が読み直して復元状態を捨てない)、状態を当てる。
    // 同じ構成で既に読み込んである Surface はそのまま使う
    bool ApplySnapshot(World& world, const uint8_t* data, size_t size);

    // ---- ワールドハッシュ ----
    // 載っている Agent が 1 体でもあるとき true。false なら WorldHasher は Nav を何も畳まない
    bool HasHashableState() const;
    uint64_t StateHash() const;
    int HashedAgentCount() const;

    const std::vector<NavSurfaceRuntime>& Surfaces() const { return surfaces_; }
    const NavSystemStats& Stats() const { return stats_; }

private:
    struct Key {
        EntityID entity;
        uint64_t assetGuid = 0;
    };
    // 1 tick の間だけ有効な Agent の作業記録 (コンポーネントへのポインタは構造変更が無い Update の間だけ有効)
    struct AgentRef {
        EntityID entity;
        NavMeshAgentComponent* agent = nullptr;
        CharacterControllerComponent* cc = nullptr;
        LocalTransform* transform = nullptr;
        bool rooted = false;      // 親が無い (回転を書ける)
        int surface = -1;         // surfaces_ の添字。乗れる Surface が無ければ -1
        int slot = -1;            // crowd のスロット。載っていなければ -1
        int inactiveReason = 0;   // 0 = 動かせる。それ以外は NavSystem.cpp の InactiveReason
        float feet[3] = {};       // CC の足元 (ワールド)
        float synced[3] = {};     // crowd へ書き戻した後の位置
    };

    static void Load(NavSurfaceRuntime& surface, const char* name);
    void ScanSurfaceKeys(World& world);
    bool SurfaceKeysChanged() const;
    void SyncSurfaces(World& world);
    void SyncObstacles(World& world);
    static void AppendObstacleLines(const NavTileStore& store, std::vector<DebugLineCmd>& out);
    void CollectAgents(World& world);
    void UpdateSurface(World& world, size_t surfaceIndex, float dt);

    std::vector<NavSurfaceRuntime> surfaces_;
    std::vector<Key> loadedKeys_;
    std::vector<Key> scanKeys_; // Update の作業用 (毎 tick の確保を避ける)
    std::vector<AgentRef> agents_;
    std::vector<std::vector<int>> wantedPerSurface_;
    std::vector<NavObstacleSpec> wantedObstacles_; // SyncObstacles の作業用
    std::vector<NavObstacleSpec> wantedHere_;      // ...そのうち 1 つの Surface の範囲と重なるもの
    std::vector<NavObstacleSpec> wantedModifiers_;     // SyncObstacles の作業用 (Modifier)
    std::vector<NavObstacleSpec> wantedModifiersHere_; // ...そのうち 1 つの Surface の範囲と重なるもの
    std::vector<uint32_t> filterMasks_;            // UpdateSurface の作業用: dtCrowd の filter 番号 -> areaMask (昇順・重複なし)
    bool filterOverflowWarned_ = false;
    int obstacleFailures_ = 0;                    // 直近のログに出した失敗数 (同じ警告を毎 tick 出さない)
    NavSystemStats stats_;
};

} // namespace mye
