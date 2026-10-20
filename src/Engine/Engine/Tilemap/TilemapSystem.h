//====================================================================================
//                          TilemapSystem.h
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          タイルマップの実効セル・衝突・書き換え (sim 状態) と座標変換
//====================================================================================
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/EntityID.h"
#include "Engine/Engine/Physics/Rigid/StaticShapeSource.h"
#include "Engine/Engine/Tilemap/TilemapAssets.h"

namespace mye {

class ByteReader;
class ByteWriter;
class World;

// セル 1 個の書き換え。アセットの初期値と違うセルだけがここに載る (差分が最小に保たれる)
struct TileOverride {
    uint32_t entityIndex = 0;
    uint32_t entityGeneration = 0;
    uint32_t layer = 0;   // 0..kMaxTilemapLayers - 1
    uint32_t cellKey = 0; // TilemapSystem::MakeCellKey。(cy, cx) 昇順と同じ並び
    uint32_t value = 0;   // tilecell:: のビット
};

// SimSnapshot の TM2D 節の読み値。World を差し替えた後に ApplySnapshot が当てる
struct TilemapSnapshot {
    std::vector<TileOverride> overrides;
};

// tick 末に生成するエフェクト (破壊されたタイルの destroyEffect)
struct TileEffectRequest {
    uint64_t prefabHash = 0; // PrefabLibrary のキー
    DirectX::XMFLOAT3 pos = { 0.0f, 0.0f, 0.0f };
};

enum class TileDamageResult {
    kNone,      // 何も起きなかった (空 / 壊れないタイル / amount <= 0 / 対象外)
    kDamaged,   // damage が増えた
    kDestroyed, // hp に届いて destroyedTile に置き換わった
};

// TilemapComponent を持つエンティティのタイルを扱う。
//   実効セル = アセットの初期値に override 表 (sim 状態) を重ねた値
//   衝突     = collision レイヤーのセルを行方向に結合した箱を、PhysicsSystem の静的収集へ供給する (StaticShapeSource)
// 持ち主は EngineLoop / HeadlessSim で、SimRefs / TickServices / SimSources から同じ実体を指すこと (3 点セット契約)。
// 原点はエンティティのワールド位置で、回転とスケールは無視する
class TilemapSystem final : public StaticShapeSource {
public:
    TilemapSystem() = default;
    TilemapSystem(const TilemapSystem&) = delete;
    TilemapSystem& operator=(const TilemapSystem&) = delete;

    // セル座標の範囲 (cellKey の 16 bit に収める)。範囲外への書き込みは拒否する
    static constexpr int32_t kCellCoordMin = -32768;
    static constexpr int32_t kCellCoordMax = 32767;
    // FillRect が 1 回で書ける最大のセル数
    static constexpr int64_t kMaxFillCells = 1 << 18;

    // ---- アセットと座標変換 (状態を持たない) ----

    // エンティティの TilemapComponent が指すアセット。コンポーネント無し / 未登録は nullptr
    static const TilemapAsset* AssetOf(World& world, EntityID e);

    // タイルマップの原点 (セル (0, 0) の左下角) のワールド座標
    static DirectX::XMFLOAT3 OriginOf(World& world, EntityID e);

    // アセットだけから引いた 1 チャンク分のセル (override を重ねない)。描画側が System() を持たないときの代わり
    static bool AssetChunkCells(World& world, EntityID e, int32_t layer, ChunkKey key,
                                std::array<uint32_t, kTilemapChunkCells>& out);
    static void AssetEnumerateChunks(World& world, EntityID e, int32_t layer, std::vector<ChunkKey>& out);

    // ワールド座標 → セル座標 (床除算。セルの外周は下側 / 左側に属する)
    static void WorldToCell(World& world, EntityID e, const DirectX::XMFLOAT3& worldPos, int32_t& cellX,
                            int32_t& cellY);
    // セル座標 → セル中心のワールド座標 (z は原点の z)
    static DirectX::XMFLOAT3 CellToWorld(World& world, EntityID e, int32_t cellX, int32_t cellY);

    // (cy, cx) 昇順と一致するセルの鍵 (負の座標を含む)。範囲は kCellCoordMin..kCellCoordMax
    static uint32_t MakeCellKey(int32_t cellX, int32_t cellY);
    static void SplitCellKey(uint32_t key, int32_t& cellX, int32_t& cellY);

    // 描画側のチャンクキャッシュを全部無効にする。復元 (snapshot / ReloadHub) とセル書き換えが呼ぶ。
    // 値は増えるだけで、描画側が焼いたときの値と比べる
    static void MarkAllDirty();
    static uint64_t DirtyEpoch();

    // ---- 実効セル (override を重ねた値) ----

    // 実効セルの値 (tilecell:: のビット)。レイヤー範囲外 / アセット無しは 0
    uint32_t GetCell(World& world, EntityID e, int32_t layer, int32_t x, int32_t y) const;

    // 1 チャンク分の実効セルを out へ (行優先、添字 = ly * 16 + lx)。チャンクが空でも 0 埋めして true
    // (override で埋まるチャンクがアセットに無いことがあるため)。アセット / レイヤーが無ければ false
    bool GetChunkCells(World& world, EntityID e, int32_t layer, ChunkKey key,
                       std::array<uint32_t, kTilemapChunkCells>& out) const;

    // アセットまたは override に載るチャンクを (cy, cx) 昇順で列挙する。アセット / レイヤーが無ければ空
    void EnumerateChunks(World& world, EntityID e, int32_t layer, std::vector<ChunkKey>& out) const;

    // ---- 書き換え (sim 状態。スクリプトからは tick の中で呼ぶ) ----

    // 1 セルを書く。value がアセットの初期値と同じなら override を消す。
    // アセット無し / レイヤー範囲外 / 座標範囲外は何もせず false
    bool SetCell(World& world, EntityID e, int32_t layer, int32_t x, int32_t y, uint32_t value);

    // 矩形 (両端を含む。x0 > x1 でも可) を同じ値で埋める。対象が kMaxFillCells を超える / SetCell と同じ理由で false
    bool FillRect(World& world, EntityID e, int32_t layer, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                  uint32_t value);

    // セルに amount ぶんの damage を足す。palette の hp が 0 のタイルは壊れない。
    // hp に届いたら destroyedTile へ置き換え (damage 0)、destroyEffect があれば tick 末のエフェクトに積む
    TileDamageResult DamageCell(World& world, EntityID e, int32_t layer, int32_t x, int32_t y, int32_t amount);

    // 積まれたエフェクトを積んだ順に out の末尾へ移して空にする。tick 末のエフェクト生成の前に呼ぶ
    void DrainEffects(std::vector<TileEffectRequest>& out);

    // 持ち主が死んだ / Tilemap を失った項を捨てる (tick 末の構造変更の後)
    void Sweep(World& world);

    // 旧シーンの状態を捨てる (シーン遷移)
    void Reset();

    // ---- ワールドハッシュと SimSnapshot の TM2D 節 ----

    // 表に 1 件でもあれば true。false なら WorldHasher は Tilemap を何も畳まない
    bool HasLiveState() const { return !overrides_.empty(); }
    const std::vector<TileOverride>& Overrides() const { return overrides_; }

    void SaveSnapshot(ByteWriter& w) const;
    // 検証つきで読む。壊れた blob (件数・範囲・並びの不正) は false で out は不定
    static bool ReadSnapshot(ByteReader& r, TilemapSnapshot& out);
    // World を差し替えた後に呼ぶ。復元後は衝突キャッシュと描画のチャンクキャッシュを全部作り直させる
    void ApplySnapshot(World& world, TilemapSnapshot&& snapshot);

    // ---- 衝突 (StaticShapeSource) ----

    // tick ごとに PhysicsSystem が呼ぶ。同じ持ち主の箱は並べて返す。順序は入力だけで決まる
    // (持ち主 index 昇順 → レイヤー → チャンク (cy, cx) → 通常の箱 → 一方通行の箱)
    void Collect(World& world, std::vector<StaticShape>& out) override;
    // 動かす物体の範囲 (regions) に届くチャンクの箱だけ返す。箱が数百になる広い面で、広域探索と並べ替えの量を減らす。
    // 範囲外のチャンクは接触を作らないので、結果は絞らない Collect と同じ (範囲の余裕は PhysicsSystem が持つ)
    void CollectWithin(World& world, MotionRegions& regions, std::vector<StaticShape>& out) override;

    // 直近の Collect / CollectWithin が返した箱の数 (計測用)
    size_t LastCollectBoxCount() const { return lastBoxCount_; }

    // 持ち主 1 体ぶんの全箱 (絞り込まない) を out の末尾へ足す。Scene View の showCollision 用 (Collect と同じキャッシュを使う)
    void AppendCollisionShapes(World& world, EntityID e, std::vector<StaticShape>& out);

private:
    // 持ち主 + 世代。世代が違えば別のエンティティ
    struct EntityKey {
        uint32_t index = 0;
        uint32_t generation = 0;
        auto operator<=>(const EntityKey&) const = default;
    };
    // キャッシュの 1 チャンク。箱は持ち主のローカル (原点基準) で、z は中心 0
    struct LocalBox {
        float cx = 0.0f;
        float cy = 0.0f;
        float hx = 0.0f;
        float hy = 0.0f;
    };
    struct ChunkBoxes {
        std::vector<LocalBox> solid;
        std::vector<LocalBox> oneWay;
    };
    struct ChunkCacheKey {
        int32_t layer = 0;
        ChunkKey chunk;
        auto operator<=>(const ChunkCacheKey&) const = default;
    };
    // 持ち主ごとの衝突キャッシュ。ColliderComponent は PhysicsSystem が tick の間指すので、map の要素として動かさない
    struct EntityCollision {
        uint64_t assetId = 0;
        ColliderComponent solid;
        ColliderComponent oneWay;
        std::map<ChunkCacheKey, ChunkBoxes> chunks;
    };

    // override 表の添字 (entity + layer + cell の鍵に一致する先頭)
    size_t LowerBound(EntityKey owner, uint32_t layer, uint32_t cellKey) const;
    // 値を書く (アセット値と同じなら項を消す)。衝突キャッシュは捨てるが描画の dirty は立てない
    void WriteCell(EntityID e, const TilemapLayer& layer, int32_t layerIndex, int32_t x, int32_t y, uint32_t value);
    void InvalidateChunk(EntityKey owner, int32_t layerIndex, ChunkKey chunk);
    // 眠っている剛体のうち、セル矩形 (両端を含む) の近くにあるものを起こす。
    // 眠りは「覚醒中の剛体に触られたら起きる」だけなので、床のタイルを消しても自力では起きず宙に浮いてしまう
    void WakeBodiesNear(World& world, EntityID e, int32_t x0, int32_t y0, int32_t x1, int32_t y1) const;
    void BuildChunkBoxes(World& world, EntityID e, const TilemapAsset& map, const TilePalette& palette,
                         int32_t layerIndex, ChunkKey chunk, ChunkBoxes& out) const;
    // Collect / CollectWithin の本体。regions が null なら絞らない
    void CollectImpl(World& world, MotionRegions* regions, std::vector<StaticShape>& out);
    void CollectEntity(World& world, EntityID e, const TilemapComponent& tm, MotionRegions* regions,
                       std::vector<StaticShape>& out);
    // ライブラリの更新 (登録のやり直し) を見て、古い箱のキャッシュを捨てる
    void RefreshCacheEpochs();

    std::vector<TileOverride> overrides_; // (entity, 世代, レイヤー, cellKey) 昇順。sim 状態
    std::vector<TileEffectRequest> pendingEffects_; // tick 内だけ。sim 状態ではない (tick 末に空になる)

    // ---- 以降は導出値 (復元・ReloadHub で作り直せる) ----
    std::map<EntityKey, EntityCollision> collision_;
    uint64_t cachedMapEpoch_ = 0;
    uint64_t cachedPaletteEpoch_ = 0;
    size_t lastBoxCount_ = 0;
};

namespace tilemap {
// 描画側が実効セルを引くための注入口。sim の持ち主 (EngineLoop / HeadlessSim) が Install し、終了時に外す。
// 未接続の間は描画側がアセットの値だけで描く
void InstallSystem(TilemapSystem* system);
TilemapSystem* System();
} // namespace tilemap

} // namespace mye
