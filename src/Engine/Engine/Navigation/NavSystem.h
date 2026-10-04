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
    float requestedDest[3] = {};
};

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
};

// Surface の .mnav を読み込んで dtNavMesh を持ち、NavMeshAgent を dtCrowd で歩かせる。
// 状態は 2 種類ある:
//   - ナビメッシュ (store): アセットから作る導出値。Obstacle 等で変わった分は Nav 節に入る
//   - crowd と slots: sim 状態。SimSnapshot の Nav 節 (SaveSnapshot / ApplySnapshot) と
//     ワールドハッシュ (StateHash) に入る。Agent が 1 体も居なければハッシュには何も足さない
class NavSystem {
public:
    // tick ごとに呼ぶ (stepSim の中、物理より前)。Surface の構成 (エンティティ・navAsset) が
    // 変わっていたら読み直し、Agent を crowd と同期して dt だけ進め、CharacterController.moveInput を書く。
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
    void CollectAgents(World& world);
    void UpdateSurface(World& world, size_t surfaceIndex, float dt);

    std::vector<NavSurfaceRuntime> surfaces_;
    std::vector<Key> loadedKeys_;
    std::vector<Key> scanKeys_; // Update の作業用 (毎 tick の確保を避ける)
    std::vector<AgentRef> agents_;
    std::vector<std::vector<int>> wantedPerSurface_;
    NavSystemStats stats_;
};

} // namespace mye
