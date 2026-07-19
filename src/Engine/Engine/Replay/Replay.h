#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Engine/Platform/Input.h"

namespace mye {

// リプレイファイル (.rep) — engine_spec.md 11.3。
// 形式 (リトルエンディアン、バイナリ):
//   MyeReplayHeader
//   tick 毎: InputSnapshot (48B) + uint64 worldHash
// InputSnapshot / WorldHasher のレイアウトが変わったら version を上げること
struct MyeReplayHeader {
    uint32_t magic = 0x5045524Du; // 'MREP'
    uint32_t version = 1;
    float fixedDt = 1.0f / 60.0f;
    uint32_t inputSize = sizeof(InputSnapshot);
    uint64_t tickCount = 0;   // 終了時に確定
    uint64_t rngState = 0;    // 記録開始時のワールド RNG (再生時に復元)
    uint64_t rngInc = 0;
    uint32_t entityCount = 0; // 記録開始時 (サニティチェック)
    uint32_t pad = 0;
};

struct ReplayTick {
    InputSnapshot input;
    uint64_t worldHash;
};

// 記録: tick 毎の入力 + ワールドハッシュを蓄積し、Stop でファイルへ書き出す
class ReplayRecorder {
public:
    void Start(const std::wstring& path, uint64_t rngState, uint64_t rngInc, uint32_t entityCount);
    void RecordTick(const InputSnapshot& input, uint64_t worldHash);
    bool Finish(); // ファイル書き出し
    bool IsActive() const { return active_; }
    uint64_t TickCount() const { return ticks_.size(); }

private:
    std::wstring path_;
    MyeReplayHeader header_;
    std::vector<ReplayTick> ticks_;
    bool active_ = false;
};

// 再生 + 検証: 記録済み入力でフェーズ 1 を置換し、tick 毎のハッシュを照合する
class ReplayPlayer {
public:
    bool Load(const std::wstring& path);
    bool IsActive() const { return active_; }
    uint64_t TickCount() const { return ticks_.size(); }
    uint64_t RngState() const { return header_.rngState; }
    uint64_t RngInc() const { return header_.rngInc; }

    const InputSnapshot& InputForTick(uint64_t tick) const { return ticks_[tick].input; }
    uint64_t ExpectedHash(uint64_t tick) const { return ticks_[tick].worldHash; }
    bool HasTick(uint64_t tick) const { return tick < ticks_.size(); }

    // 照合結果
    uint64_t verifiedTicks = 0;
    bool failed = false;
    uint64_t firstMismatchTick = 0;

private:
    MyeReplayHeader header_;
    std::vector<ReplayTick> ticks_;
    bool active_ = false;
};

} // namespace mye
