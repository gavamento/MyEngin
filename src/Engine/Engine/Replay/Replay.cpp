#include "Engine/Engine/Replay/Replay.h"

#include <filesystem>
#include <fstream>

#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace {
constexpr uint32_t kReplayMagic = 0x5045524Du; // 'MREP'
constexpr uint32_t kReplayVersion = 4;
} // namespace

void ReplayRecorder::Start(const std::wstring& path, uint64_t rngState, uint64_t rngInc,
                           uint32_t entityCount, const std::byte* snapshot, size_t snapshotSize)
{
    path_ = path;
    header_ = {};
    header_.rngState = rngState;
    header_.rngInc = rngInc;
    header_.entityCount = entityCount;
    snapshot_.clear();
    if (snapshot != nullptr && snapshotSize > 0) {
        snapshot_.assign(snapshot, snapshot + snapshotSize);
    }
    header_.snapshotSize = snapshot_.size();
    inputs_.clear();
    hashes_.clear();
    active_ = true;
    MYE_LOG_INFO("[replay] recording to %s (snapshot %zu bytes)", WideToUtf8(path).c_str(),
                 snapshot_.size());
}

void ReplayRecorder::RecordTick(const InputSnapshot& input, uint64_t worldHash)
{
    inputs_.push_back(input); // playerCount == 1 (マルチ入力は M52g)
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

} // namespace mye
