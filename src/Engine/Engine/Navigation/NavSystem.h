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
class Pcg32;
struct NavMeshAgentComponent;
struct NavMeshObstacleComponent;
struct NavMeshModifierComponent;
struct NavMeshLinkComponent;
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
    uint8_t stuck = 0;             // 前進が止まっている (通知済み。前進が戻れば 0 へ)
    int32_t noProgressTicks = 0;   // 最良の残り距離を縮められていない tick 数
    float bestRemaining = -1.0f;   // 進捗の基準にしている残り距離。負 = 未設定
    float requestedDest[3] = {};
    // ---- Off-Mesh Link の渡り (dtCrowd は OFFMESH 状態で止めておき、補間は NavSystem が持つ) ----
    uint8_t linkPhase = 0;         // 0 = 渡っていない / 1 = 入口へ近づく / 2 = 渡る / 3 = Manual の完了待ち
    uint8_t linkMode = 0;          // navlinktraversal
    int32_t linkTick = 0;          // 現在のフェーズで経過した tick
    int32_t linkTicks = 0;         // 現在のフェーズの長さ (tick)。完了待ちでは 0
    float linkHeight = 0.0f;       // Jump の弧の高さ
    float linkSpeed = 0.0f;        // 渡る速さ (m/s)
    float linkFrom[3] = {};        // 近づく前の位置
    float linkStart[3] = {};       // 入口
    float linkEnd[3] = {};         // 出口
};

// NavMeshObstacle の形をワールドの NavObstacleSpec にする。m は行ベクトル規約の 4x4 ワールド行列 (行 3 が平行移動)。
// Box は y 回転だけなら回転箱、傾いていれば回転後の AABB、Cylinder は y 回転を無視した底面中心・半径 (max(|sx|, |sz|) 倍)・高さ (|sy| 倍)。
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

// エンティティキーから Link のキーを作る。キー順 == (index, generation) 順。Link だけの一覧なので Obstacle のキーと衝突しない
uint64_t NavLinkKey(EntityID entity);

// NavMeshLink をワールドの NavLinkSpec にする。m は行ベクトル規約の 4x4 ワールド行列。
// 吸着半径は width の半分に水平方向の拡大を掛けたもの。座標が非有限、入口と出口が同じ点なら false (その Link は無いものとして扱う)
bool NavMakeLinkSpec(const NavMeshLinkComponent& link, const float (&m)[4][4], EntityID entity, NavLinkSpec& out);

// Link の置き方の静的な検査 (ナビメッシュを読み込まなくても分かる分)。Inspector と SelfTest が使う
enum class NavLinkPlacement : uint8_t {
    Ok,
    NoSurface,   // 入口がどの Surface の範囲にも入らない (ベイクされない所)
    ExitTooFar,  // 出口が入口のタイルから 2 枚以上離れている (Detour は同じか隣のタイルまでしかつなげない)
};
NavLinkPlacement NavCheckLinkPlacement(World& world, const NavLinkSpec& link);

// World の有効な NavMeshLink を key 昇順に集める (out は上書き)
void NavCollectLinkSpecs(World& world, std::vector<NavLinkSpec>& out);

// all のうち、入口が Surface のベイク範囲 (ワールド AABB) に入るものだけを out に入れる (順序は保つ)
void NavFilterLinksToSurface(World& world, EntityID surface, const std::vector<NavLinkSpec>& all,
                             std::vector<NavLinkSpec>& out);

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
    int linkChanges = 0;           // 直近の同期で足した / 外した / 動かした Link の数
    int linkDisconnected = 0;      // 入口か出口が歩行面につながっていない Link の数 (タイルを作り直すたびに数え直す)
    int linkWarnings = 0;          // 「つながらない Link」の警告を出した回数 (Reset 以降)
};

// QueryRaycast の結果。hit = ナビメッシュの縁 (壁・歩けないエリア) で止まった。止まらなければ point = to
struct NavRaycastResult {
    bool hit = false;
    float point[3] = {};
    float normal[3] = {}; // hit のときの壁の法線 (水平)
    float distance = 0.0f;
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

    // 渡り開始のログを出すか。再シム (ロールバック・巻き戻しの再実行) では TickRunner が false にして重複を避ける
    void SetCrossingLogEnabled(bool enable) { logCrossings_ = enable; }

    // 物理 (フェーズ 3.6) の後、Transform の前に呼ぶ。Link を渡っている Agent の位置と CC.velocity を上書きし、
    // 渡り終えたら dtCrowd の OFFMESH を解く。渡っている間 CC.moveInput は 0 (Update が書く)
    void PostPhysics(World& world, float dt);

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

    // ---- スクリプト API (ABI v24) のクエリ ----
    // 読み取り専用 (sim 状態を変えない)。対象は agentTypeId が合う最初の Surface (Agent の割り当てと同じ規則)。
    // 通れるエリアは areaMask と、その Surface の areaCosts (Agent と同じ filter)。
    // Surface が未読み込み (最初の tick の Update より前を含む) なら全部失敗を返す
    // 始点・終点を 1 m x 2 m の近傍でナビメッシュへ吸着して経路を引き、角を out へ書く (先頭は吸着した始点)。
    // 届かない目的地は届く限りの最寄りまで (outPartial = true)。戻り値は書いた角の数 (0 = 失敗)
    int QueryFindPath(World& world, int agentTypeId, const float* from, const float* to, uint32_t areaMask,
                      float* outCorners, int maxCorners, bool* outPartial) const;
    // pos の最寄りのナビメッシュ上の点 (extents は半径の箱)。見つからなければ false
    bool QuerySamplePosition(World& world, int agentTypeId, const float* pos, const float* extents, uint32_t areaMask,
                             float* outPoint) const;
    // ナビメッシュ上を from から to へ歩く線が壁で止まるか。from がナビメッシュに乗らなければ false (out は触らない)
    bool QueryRaycast(World& world, int agentTypeId, const float* from, const float* to, uint32_t areaMask,
                      NavRaycastResult& out) const;
    // center を中心とする半径 radius の円の中から一様に選んだ点のうち、ナビメッシュに乗るもの (最大 16 回試す)。
    // rng は呼び出し側の World の RNG。Surface なし / center の近傍にナビメッシュなし / radius が不正のときは rng を引かない。
    // center からつながっているかは見ない
    bool QueryRandomPoint(World& world, int agentTypeId, const float* center, float radius, uint32_t areaMask, Pcg32& rng,
                          float* outPoint) const;
    // Manual の Link で止まっている (入口へ近づく途中を含む) Agent に完了を通知する。該当しなければ false
    bool CompleteLink(World& world, EntityID agent) const;

    const std::vector<NavSurfaceRuntime>& Surfaces() const { return surfaces_; }
    const NavSystemStats& Stats() const { return stats_; }

private:
    // agentTypeId の Surface (最初の 1 つ) を返し、filter を areaMask と Surface の areaCosts で組む。Loaded でなければ null
    const NavSurfaceRuntime* ResolveQuerySurface(World& world, int agentTypeId, uint32_t areaMask, dtQueryFilter& filter) const;

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
    static void AppendLinkLines(const NavTileStore& store, std::vector<DebugLineCmd>& out);
    // 前進が止まった Agent の到着判定 (渋滞): 目的地の近く、または同じ目的地で到着済みの Agent に接している。
    // wanted は UpdateSurface のキー順の Agent 一覧。同じ tick に先に到着した Agent も後続の判定に使う
    bool IsJamArrival(const NavSurfaceRuntime& surface, const std::vector<int>& wanted, const NavMeshAgentComponent& agent,
                      int slotIndex, float remaining, float arriveDistance) const;
    void BeginLink(World& world, NavSurfaceRuntime& surface, int slotIndex, NavMeshAgentComponent& agent, float dt) const;
    static void FinishLink(NavSurfaceRuntime& surface, int slotIndex, const NavMeshAgentComponent& agent, const float* exitPos);
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
    std::vector<NavLinkSpec> wantedLinks_;             // SyncObstacles の作業用 (Link)
    std::vector<NavLinkSpec> wantedLinksHere_;         // ...そのうち 1 つの Surface の範囲に入口があるもの
    int linkDisconnected_ = 0;                         // 直近のログに出した「入口か出口がナビメッシュにつながらない Link」の数
    bool logCrossings_ = true;
    std::vector<uint32_t> filterMasks_;            // UpdateSurface の作業用: dtCrowd の filter 番号 -> areaMask (昇順・重複なし)
    bool filterOverflowWarned_ = false;
    int obstacleFailures_ = 0;                    // 直近のログに出した失敗数 (同じ警告を毎 tick 出さない)
    NavSystemStats stats_;
};

} // namespace mye
