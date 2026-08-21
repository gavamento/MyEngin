#include "Engine/Engine/Replay/Replay.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace {
constexpr uint32_t kReplayMagic = 0x5045524Du; // 'MREP'
constexpr uint32_t kReplayVersion = kReplayFileVersion;
} // namespace

void ReplayRecorder::Start(const std::wstring& path, uint64_t rngState, uint64_t rngInc,
                           uint32_t entityCount, uint32_t playerCount, const std::byte* snapshot,
                           size_t snapshotSize)
{
    path_ = path;
    header_ = {};
    header_.rngState = rngState;
    header_.rngInc = rngInc;
    header_.entityCount = entityCount;
    header_.playerCount = (playerCount == 0) ? 1u : playerCount;
    snapshot_.clear();
    if (snapshot != nullptr && snapshotSize > 0) {
        snapshot_.assign(snapshot, snapshot + snapshotSize);
    }
    header_.snapshotSize = snapshot_.size();
    inputs_.clear();
    hashes_.clear();
    active_ = true;
    MYE_LOG_INFO("[replay] recording to %s (players %u, snapshot %zu bytes)",
                 WideToUtf8(path).c_str(), header_.playerCount, snapshot_.size());
}

void ReplayRecorder::RecordTick(const InputSnapshot* lanes, uint32_t playerCount,
                                uint64_t worldHash)
{
    // 宣言と実際が食い違ったら**宣言側に合わせて**書く (足りない分はゼロ値)。
    // ここで黙って可変長にすると、ファイルの tick レコード長が tick ごとに変わって
    // 再生側が一切読めなくなる
    for (uint32_t p = 0; p < header_.playerCount; ++p) {
        inputs_.push_back((lanes != nullptr && p < playerCount) ? lanes[p] : InputSnapshot{});
    }
    hashes_.push_back(worldHash);
}

bool ReplayRecorder::Finish()
{
    if (!active_) {
        return false;
    }
    active_ = false;
    header_.tickCount = hashes_.size();

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path_).parent_path(), ec);
    std::ofstream f(std::filesystem::path(path_), std::ios::binary);
    if (!f) {
        MYE_LOG_ERROR("[replay] cannot write %s", WideToUtf8(path_).c_str());
        return false;
    }
    f.write(reinterpret_cast<const char*>(&header_), sizeof(header_));
    if (!snapshot_.empty()) {
        f.write(reinterpret_cast<const char*>(snapshot_.data()),
                static_cast<std::streamsize>(snapshot_.size()));
    }
    // tick レコード = InputSnapshot × playerCount + uint64 hash。
    // 入力列とハッシュ列を別々に持っているので、書くときに tick 単位で綴じ直す
    const size_t perTick = header_.playerCount;
    for (size_t t = 0; t < hashes_.size(); ++t) {
        f.write(reinterpret_cast<const char*>(&inputs_[t * perTick]),
                static_cast<std::streamsize>(perTick * sizeof(InputSnapshot)));
        f.write(reinterpret_cast<const char*>(&hashes_[t]), sizeof(uint64_t));
    }
    MYE_LOG_INFO("[replay] recorded %llu ticks -> %s",
                 static_cast<unsigned long long>(hashes_.size()), WideToUtf8(path_).c_str());
    return true;
}

bool ReplayPlayer::Load(const std::wstring& path)
{
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) {
        MYE_LOG_ERROR("[replay] cannot open %s", WideToUtf8(path).c_str());
        return false;
    }
    f.read(reinterpret_cast<char*>(&header_), sizeof(header_));
    if (!f || header_.magic != kReplayMagic) {
        MYE_LOG_ERROR("[replay] bad file magic");
        return false;
    }
    if (header_.version != kReplayVersion || header_.inputSize != sizeof(InputSnapshot)) {
        MYE_LOG_ERROR("[replay] incompatible version/layout (v%u, input %u bytes)",
                      header_.version, header_.inputSize);
        return false;
    }
    if (header_.playerCount == 0) {
        MYE_LOG_ERROR("[replay] playerCount = 0");
        return false;
    }
    snapshot_.resize(static_cast<size_t>(header_.snapshotSize));
    if (!snapshot_.empty()) {
        f.read(reinterpret_cast<char*>(snapshot_.data()),
               static_cast<std::streamsize>(snapshot_.size()));
    }
    const size_t perTick = header_.playerCount;
    inputs_.resize(static_cast<size_t>(header_.tickCount) * perTick);
    hashes_.resize(static_cast<size_t>(header_.tickCount));
    for (size_t t = 0; t < hashes_.size(); ++t) {
        f.read(reinterpret_cast<char*>(&inputs_[t * perTick]),
               static_cast<std::streamsize>(perTick * sizeof(InputSnapshot)));
        f.read(reinterpret_cast<char*>(&hashes_[t]), sizeof(uint64_t));
    }
    if (!f) {
        MYE_LOG_ERROR("[replay] truncated file");
        return false;
    }
    active_ = true;
    MYE_LOG_INFO("[replay] loaded %llu ticks from %s (players %u, snapshot %zu bytes)",
                 static_cast<unsigned long long>(hashes_.size()), WideToUtf8(path).c_str(),
                 header_.playerCount, snapshot_.size());
    return true;
}

namespace {

// InputSnapshot のどのフィールドが最初に食い違ったかを名前で返す (空 = 一致)。
// ★HashEntity と同じく**構造体まるごと**を見る — 明示パディング (pad / pad2) まで
//   比較対象に入れているのは、そこが .rep のバイト列に載る以上「一致していない .rep」は
//   本当に一致していないから (M48i の String64 終端以降と同じ理屈)
std::string FirstDifferentInputField(const InputSnapshot& a, const InputSnapshot& b)
{
    for (int i = 0; i < 32; ++i) {
        if (a.keys[i] != b.keys[i]) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "keys[%d]", i);
            return std::string(buf);
        }
    }
    if (a.mouseX != b.mouseX) return "mouseX";
    if (a.mouseY != b.mouseY) return "mouseY";
    if (a.wheelDelta != b.wheelDelta) return "wheelDelta";
    if (a.mouseButtons != b.mouseButtons) return "mouseButtons";
    if (std::memcmp(a.pad, b.pad, sizeof(a.pad)) != 0) return "pad";
    if (a.padButtons != b.padButtons) return "padButtons";
    if (a.padLeftTrigger != b.padLeftTrigger) return "padLeftTrigger";
    if (a.padRightTrigger != b.padRightTrigger) return "padRightTrigger";
    if (a.padLX != b.padLX) return "padLX";
    if (a.padLY != b.padLY) return "padLY";
    if (a.padRX != b.padRX) return "padRX";
    if (a.padRY != b.padRY) return "padRY";
    if (a.padConnected != b.padConnected) return "padConnected";
    if (std::memcmp(a.pad2, b.pad2, sizeof(a.pad2)) != 0) return "pad2";
    return std::string();
}

} // namespace

ReplayDiffResult DiffReplayFiles(const std::wstring& a, const std::wstring& b)
{
    ReplayDiffResult r;
    ReplayPlayer pa;
    ReplayPlayer pb;
    if (!pa.Load(a) || !pb.Load(b)) {
        r.summary = "one of the .rep files could not be loaded";
        return r;
    }
    const MyeReplayHeader& ha = pa.Header();
    const MyeReplayHeader& hb = pb.Header();
    char buf[256];
    // ヘッダ = 「同じ世界から同じ条件で録り始めたか」。ここが割れているなら
    // tick 列を比べても意味が無い (別のシーン同士を比べているだけ)
    struct HeaderField {
        const char* name;
        uint64_t a;
        uint64_t b;
    };
    const HeaderField fields[] = {
        { "version", ha.version, hb.version },
        { "playerCount", ha.playerCount, hb.playerCount },
        { "rngState", ha.rngState, hb.rngState },
        { "rngInc", ha.rngInc, hb.rngInc },
        { "entityCount", ha.entityCount, hb.entityCount },
        { "snapshotSize", ha.snapshotSize, hb.snapshotSize },
        { "tickCount", ha.tickCount, hb.tickCount },
    };
    for (const HeaderField& f : fields) {
        if (f.a != f.b) {
            std::snprintf(buf, sizeof(buf), "header.%s differs: %llu vs %llu", f.name,
                          static_cast<unsigned long long>(f.a),
                          static_cast<unsigned long long>(f.b));
            r.summary = buf;
            return r;
        }
    }
    if (pa.Snapshot() != pb.Snapshot()) {
        r.summary = "the embedded start snapshots differ";
        return r;
    }
    const uint64_t ticks = pa.TickCount();
    for (uint64_t t = 0; t < ticks; ++t) {
        for (uint32_t p = 0; p < ha.playerCount; ++p) {
            const std::string field =
                FirstDifferentInputField(pa.InputForTick(t, p), pb.InputForTick(t, p));
            if (!field.empty()) {
                std::snprintf(buf, sizeof(buf),
                              "tick %llu: input lane %u differs at %s (the two runs did NOT "
                              "consume the same input)",
                              static_cast<unsigned long long>(t), p, field.c_str());
                r.firstDiffTick = t;
                r.summary = buf;
                return r;
            }
        }
        if (pa.ExpectedHash(t) != pb.ExpectedHash(t)) {
            std::snprintf(buf, sizeof(buf),
                          "tick %llu: world hash differs (%016llx vs %016llx) - same input, "
                          "different simulation",
                          static_cast<unsigned long long>(t),
                          static_cast<unsigned long long>(pa.ExpectedHash(t)),
                          static_cast<unsigned long long>(pb.ExpectedHash(t)));
            r.firstDiffTick = t;
            r.summary = buf;
            return r;
        }
    }
    std::snprintf(buf, sizeof(buf), "identical: %llu ticks x %u lanes",
                  static_cast<unsigned long long>(ticks), ha.playerCount);
    r.same = true;
    r.summary = buf;
    return r;
}

} // namespace mye
