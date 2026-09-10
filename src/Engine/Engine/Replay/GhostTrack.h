#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/EntityID.h"

namespace mye {

class World;

// 分岐のゴースト (M72d): 「捨てなかった未来」の見た目を、分岐した瞬間に 1 回だけ再シムして
// 採取した **tick ごとのワールド行列** の列。描画 (SceneView、M72e) はこれを引くだけで、
// 第 2 のワールドを同時に回さない (TickServices の sim 側シングルトンは複製できない)。
//
// ★sim 状態ではない。World を読むだけで書かず、RNG も引かない = ハッシュに影響しない。
//   採取は EngineLoop の再シムループが RunOneTick の**後**に呼ぶ (状態は tick 末ハッシュと同一)。
// ★疎: 前の key と行列が 1 バイトも変わらないエンティティには key を積まない。
//   静的な物は key 1 個で終わる (描画側は keys.size() > 1 を「動いた物」として扱える)。
// ★消えたエンティティは alive=0 の墓標 key を 1 個積む (最後の位置に居座らせない)。

struct GhostKey {
    uint32_t tick = 0;  // この行列が「tick が走る前」の状態
    uint8_t alive = 1;  // 0 = この tick からエンティティは存在しない
    uint8_t pad[3] = {};
    float m[12] = {};   // ワールド行列の 3x4: [0..8] = 各軸 (行優先)、[9..11] = 平行移動
};

struct GhostEntityTrack {
    EntityID id;
    AssetID mesh;           // MeshRenderer.mesh (AABB の出所。無ければ単位箱)
    char name[32] = {};     // 表示用 (World::GetName の写し。ライブ側で消えていても描ける)
    uint32_t lastSeenTick = 0;
    std::vector<GhostKey> keys; // tick 昇順
};

struct GhostTrack {
    uint64_t firstTick = 0;
    uint64_t lastTick = 0;   // 最後に採取した tick (= この tick が走る前の状態まで持つ)
    size_t bytes = 0;        // key + track の概算
    size_t keyCount = 0;
    bool truncated = false;  // 予算 (bytes / ticks) で打ち切った
    bool verified = false;   // 焼き終わりのハッシュが記録と一致した
    uint64_t endHash = 0;
    std::vector<GhostEntityTrack> entities;

    void Reset();
    void Begin(uint64_t tick);
    // WorldMatrix + MeshRenderer を持つ全エンティティを見て、変化した物だけ key を積む。
    // 予算を超えたら truncated を立てて false
    bool Sample(World& world, uint64_t tick, size_t maxBytes);
    // entities[ent] の tick 時点の key (tick 以下で最後のもの)。無ければ nullptr
    const GhostKey* KeyAt(size_t ent, uint64_t tick) const;
    size_t MovingCount() const; // key が 2 個以上ある = 動いた (または消えた) エンティティ
    static void ToMatrix(const GhostKey& k, DirectX::XMFLOAT4X4& out);
    static void FromMatrix(const DirectX::XMFLOAT4X4& in, GhostKey& out);

private:
    size_t SlotFor(EntityID id, AssetID mesh, const char* name);
    std::vector<uint32_t> slotByIndex_; // entity.index → entities の添字 + 1 (0 = 未登録)
};

} // namespace mye
