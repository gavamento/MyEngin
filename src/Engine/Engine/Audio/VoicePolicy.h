#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

// ボイスのスティール (奪い取り) 判定。**純関数のみ**を置くヘッダで、XAudio2 にも D3D にも
// 依存しないのでヘッドレス selftest から直接叩ける
// (Renderer/FrustumCull.h・Renderer/PostFxMath.h・Particles/ParticleCurves.h と同じ流儀)。

namespace mye {

// スティール判定に必要な情報だけを抜き出したボイスの要約
struct VoiceSlotInfo {
    bool active = false;    // 再生中か
    int32_t priority = 128; // **大きいほど重要** (Unity の 0=最重要とは逆。混同注意)
    uint64_t startSeq = 0;  // 再生開始の通し番号。小さいほど古い (同値は起こらない)
};

// 空きスロットが無いときに奪う相手を選ぶ。
//   候補   = active かつ priority <= newPriority (自分より重要な音は奪わない)
//   選び方 = 候補のうち最も priority が低いもの、同値なら最も古いもの (startSeq 最小)
//   戻り値 = 奪うスロット index。候補が無ければ -1 (= 新規再生をあきらめる)
//
// 比較キー (priority, startSeq) は全順序で startSeq が一意なので、**走査順に依存しない** =
// 決定論的 (spec 11.2 規則 7: 並べ替えは明示キーで行うこと)。
inline int PickVoiceToSteal(const VoiceSlotInfo* slots, size_t count, int32_t newPriority)
{
    int best = -1;
    int32_t bestPriority = 0;
    uint64_t bestSeq = 0;
    for (size_t i = 0; i < count; ++i) {
        const VoiceSlotInfo& s = slots[i];
        if (!s.active || s.priority > newPriority) {
            continue;
        }
        if (best < 0 || s.priority < bestPriority ||
            (s.priority == bestPriority && s.startSeq < bestSeq)) {
            best = static_cast<int>(i);
            bestPriority = s.priority;
            bestSeq = s.startSeq;
        }
    }
    return best;
}

// ミキサー UI が dB、XAudio2 が線形倍率なので相互変換を一箇所に置く。
// kMinDb 以下は完全な無音 (0.0) として扱う (フェーダを下げ切れるようにするため)。
inline constexpr float kMinDb = -80.0f;

inline float DbToLinear(float db)
{
    if (!(db > kMinDb)) { // NaN もここで無音に落ちる (手編集された .mixer.json 対策)
        return 0.0f;
    }
    return std::pow(10.0f, db * 0.05f); // 10^(dB/20)
}

inline float LinearToDb(float linear)
{
    if (linear <= 0.0f) {
        return kMinDb;
    }
    const float db = 20.0f * std::log10(linear);
    return db < kMinDb ? kMinDb : db;
}

} // namespace mye
