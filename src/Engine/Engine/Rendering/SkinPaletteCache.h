//====================================================================================
//                          SkinPaletteCache.h
//  MyEngin/ 秋田蓮音                                                       10/09/2026
//                                          スキンのボーンパレットのビュー間キャッシュとアニメ間引き (URO) の判定
//====================================================================================
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/EntityID.h"
#include "Engine/Engine/Animation/SkinningSystem.h"

namespace mye {

// ---- URO (アニメの距離間引き。描画専用、sim のポーズ評価には触れない) ----
// 外接球が画面の高さに占める割合 (LodScreenSize) が小さいスキンほど、パレットを作り直す間隔 (tick) を延ばす。
// 閾値は 1080p の画面高さに対する画素数の目安で選んだ: 5% = 54px 以上は毎 tick、2% = 22px 以上は 30Hz、
// 0.8% = 9px 以上は 15Hz、それ未満は 7.5Hz。画素差を測って決めた値ではなく、目視で調整する前提の初期値
struct UroTier {
    float minScreenSize; // この値以上なら interval を使う
    uint32_t interval;   // 作り直す間隔 (tick)
};
inline constexpr UroTier kUroTiers[] = { { 0.05f, 1 }, { 0.02f, 2 }, { 0.008f, 4 }, { 0.0f, 8 } };

inline uint32_t UroUpdateInterval(float screenSize)
{
    for (const UroTier& tier : kUroTiers) {
        if (screenSize >= tier.minScreenSize) {
            return tier.interval;
        }
    }
    return kUroTiers[sizeof(kUroTiers) / sizeof(kUroTiers[0]) - 1].interval;
}

// 間引く対象か。ラグドール作動中 (剛体が置いた骨) と、クリップ・層を混ぜている最中は毎 tick 作り直す
inline bool UroEligible(const SkinnedMeshComponent& sm, bool ragdollActive)
{
    return !ragdollActive && !IsPoseBlending(sm);
}

// 更新 tick の位相。entity.index をそのまま使うと、1 体で複数のエンティティを作るローダ (20 刻み等) で
// 位相が揃い、更新が 1 tick に集中する。決定的な整数ミックス (murmur3 の fmix32) で散らす
inline uint32_t UroPhase(uint32_t entityIndex)
{
    uint32_t h = entityIndex;
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

// このエンティティが interval の間隔で作り直す tick か。tick と entityIndex だけで決まる (実時間・描画フレーム数に依存しない)
inline bool UroIsUpdateTick(uint64_t simTick, uint32_t entityIndex, uint32_t interval)
{
    return interval <= 1 || (simTick + UroPhase(entityIndex)) % interval == 0;
}

// simTick が属する「更新の窓」の先頭 tick (その窓で最後に作り直す tick だった tick)。負になりうる
inline int64_t UroWindowStart(uint64_t simTick, uint32_t entityIndex, uint32_t interval)
{
    if (interval <= 1) {
        return static_cast<int64_t>(simTick);
    }
    return static_cast<int64_t>(simTick) - static_cast<int64_t>((simTick + UroPhase(entityIndex)) % interval);
}

// ---- ビュー間のパレットキャッシュ ----
// 描画専用 (sim から見えない)。Scene View と Game View が同じキャラのパレットを 2 回作らない。
// 検索・確保は直列段だけで行い、palette への書き込みだけを並列段が行う (エントリのアドレスは安定)
class SkinPaletteCache {
public:
    struct Entry {
        EntityID entity;
        AssetID model;
        SkinnedMeshComponent pose; // パレットを作ったときのポーズ入力
        float alpha = 1.0f;        // 作ったときの補間 alpha (補間しないときは 1)
        int64_t window = 0;        // URO の窓の先頭 tick (間引かないときは作った tick)
        uint32_t interval = 1;     // 作ったときの URO 間隔
        uint64_t evalTick = 0;     // 作った tick
        bool cacheable = true;     // false = ECS のポーズ入力だけでは決まらない (ラグドール)。再利用しない
        bool valid = false;
        uint64_t lastUsedView = 0;
        std::vector<DirectX::XMFLOAT4X4> palette;
    };

    struct Request {
        EntityID entity;
        AssetID model;
        const SkinnedMeshComponent* sm = nullptr;
        float alpha = 1.0f;     // 補間しないときは 1
        uint64_t simTick = 0;
        uint32_t interval = 1;  // URO の間隔。1 = 毎 tick
        bool cacheable = true;
    };

    // 1 回の Render の先頭で呼ぶ (古いエントリの掃除に使う)
    void BeginFrame();
    // 再利用できるエントリ。無ければ nullptr
    Entry* Find(const Request& req);
    // 作り直すエントリを確保して鍵を書く (palette は呼び出し側が埋める)
    Entry* Claim(const Request& req);
    void Clear();

    size_t LiveCount() const { return liveCount_; }

private:
    static bool Reusable(const Entry& e, const Request& req);

    std::deque<Entry> entries_; // deque = push_back でアドレスが動かない
    std::vector<int32_t> slotByIndex_;
    std::vector<int32_t> freeSlots_;
    uint64_t viewCounter_ = 0;
    size_t liveCount_ = 0;
};

} // namespace mye
