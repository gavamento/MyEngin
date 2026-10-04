//====================================================================================
//                          NavDebugDraw.h
//  MyEngin/ 秋田蓮音                                                     10/04/2026
//                                          ナビメッシュの表示用ジオメトリ (半透明の塗り・輪郭線・タイル境界)
//====================================================================================
#pragma once

#include <cstdint>
#include <vector>

#include "Engine/Core/Ecs/EntityID.h"
#include "Engine/Engine/Rendering/DebugDraw.h"

namespace mye {

class World;

// .mnav から表示用の三角形と線を作って持つ。NavSystem (sim) とは独立で、tick を回さない編集中の
// SceneView でも使える。World と .mnav を読むだけで sim 状態・ハッシュには触れない。
// 描画フレームごとに Refresh を呼ぶ。Surface の構成 (エンティティ・navAsset・表示フラグ) が
// 前回と同じなら何も作り直さない。作り直すのは構成が変わったフレームだけ
class NavDebugView {
public:
    struct Stats {
        uint32_t refreshCount = 0;  // Refresh を呼んだ回数
        uint32_t rebuildCount = 0;  // 三角形・線を作り直した回数
        uint32_t loadCount = 0;     // .mnav を読んで表示用に組んだ回数 (Surface 1 つにつき 1)
        double lastRebuildUs = 0.0; // 直近の作り直しの所要時間 (.mnav の読み込みを含む)
        int lastTriangles = 0;
        int lastLines = 0;
    };

    // メインスレッド専用 (.mnav の読み込みとログ)
    void Refresh(World& world);

    // 旧シーンの分を捨てる
    void Reset();

    // 表示フラグの立った全 Surface をつないだ三角形リスト (3 頂点 1 枚、ワールド座標)
    const std::vector<DebugFillVertex>& FillVertices() const { return fill_; }
    // 塗りが変わるたびに増える。GPU の頂点バッファを作り直す印 (0 = まだ何も出ていない)
    uint64_t FillSerial() const { return fillSerial_; }
    // 輪郭線とタイル境界 (ワールド座標)
    const std::vector<DebugLineCmd>& Lines() const { return lines_; }

    const Stats& GetStats() const { return stats_; }

private:
    // 表示フラグ
    enum : uint8_t { kOutline = 1, kFill = 2, kTileBounds = 4 };

    struct Key {
        EntityID entity;
        uint64_t assetGuid = 0;
        uint8_t flags = 0;
        bool operator==(const Key& o) const
        {
            return entity == o.entity && assetGuid == o.assetGuid && flags == o.flags;
        }
    };

    // Surface 1 つ分の表示用ジオメトリ。(entity, assetGuid) が同じ間は使い回す
    struct Geometry {
        EntityID entity;
        uint64_t assetGuid = 0;
        bool loaded = false;
        std::vector<DebugFillVertex> fill;
        std::vector<DebugLineCmd> outline;
        std::vector<DebugLineCmd> tileBounds;
    };

    void ScanKeys(World& world);
    void Rebuild(World& world);
    Geometry& GeometryFor(World& world, const Key& key);

    std::vector<Key> keys_;     // 前回の構成
    std::vector<Key> scanKeys_; // Refresh の作業用 (毎フレームの確保を避ける)
    std::vector<Geometry> geometries_;
    std::vector<DebugFillVertex> fill_;
    std::vector<DebugLineCmd> lines_;
    uint64_t fillSerial_ = 0;
    Stats stats_;
};

} // namespace mye
