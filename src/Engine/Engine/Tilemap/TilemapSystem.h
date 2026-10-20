//====================================================================================
//                          TilemapSystem.h
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          タイルマップの実効セルと座標変換 (アセット + エンティティ)
//====================================================================================
#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Ecs/EntityID.h"
#include "Engine/Engine/Tilemap/TilemapAssets.h"

namespace mye {

class World;

// TilemapComponent を持つエンティティから、アセットのセルと座標変換を引く。
// 「実効セル」= アセットの初期値に sim 側の書き換え (override 表、sub-11) を重ねた値。今は無条件にアセットの値。
// 原点はエンティティのワールド位置 (WorldMatrix の平行移動) で、回転とスケールは無視する。
// 状態を持たない静的関数だけ (override 表を持つようになったら実体に変える)
class TilemapSystem {
public:
    // エンティティの TilemapComponent が指すアセット。コンポーネント無し / 未登録は nullptr
    static const TilemapAsset* AssetOf(World& world, EntityID e);

    // タイルマップの原点 (セル (0, 0) の左下角) のワールド座標
    static DirectX::XMFLOAT3 OriginOf(World& world, EntityID e);

    // 実効セルの値 (tilecell:: のビット)。レイヤー範囲外 / アセット無しは 0
    static uint32_t GetCell(World& world, EntityID e, int32_t layer, int32_t x, int32_t y);

    // 1 チャンク分の実効セルを out へ (行優先、添字 = ly * 16 + lx)。チャンクが空でも 0 埋めして true
    // (override で埋まるチャンクがアセットに無いことがあるため)。アセット / レイヤーが無ければ false
    static bool GetChunkCells(World& world, EntityID e, int32_t layer, ChunkKey key,
                              std::array<uint32_t, kTilemapChunkCells>& out);

    // 非空のチャンクを (cy, cx) 昇順で列挙する。アセット / レイヤーが無ければ空
    static void EnumerateChunks(World& world, EntityID e, int32_t layer, std::vector<ChunkKey>& out);

    // ワールド座標 → セル座標 (床除算。セルの外周は下側 / 左側に属する)
    static void WorldToCell(World& world, EntityID e, const DirectX::XMFLOAT3& worldPos, int32_t& cellX,
                            int32_t& cellY);
    // セル座標 → セル中心のワールド座標 (z は原点の z)
    static DirectX::XMFLOAT3 CellToWorld(World& world, EntityID e, int32_t cellX, int32_t cellY);

    // 描画側のチャンクキャッシュを全部無効にする。復元 (snapshot / ReloadHub) とセル書き換え (sub-11) が呼ぶ。
    // 値は増えるだけで、描画側が焼いたときの値と比べる
    static void MarkAllDirty();
    static uint64_t DirtyEpoch();
};

} // namespace mye
