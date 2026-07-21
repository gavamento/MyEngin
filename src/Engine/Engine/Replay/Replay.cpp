#include "Engine/Engine/Replay/Replay.h"

#include <filesystem>
#include <fstream>

#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {

void ReplayRecorder::Start(const std::wstring& path, uint64_t rngState, uint64_t rngInc,
                           uint32_t entityCount)
{
    path_ = path;
    header_ = {};
    header_.rngState = rngState;
    header_.rngInc = rngInc;
    header_.entityCount = entityCount;
    ticks_.clear();
    active_ = true;
    MYE_LOG_INFO("[replay] recording to %s", WideToUtf8(path).c_str());
}

void ReplayRecorder::RecordTick(const InputSnapshot& input, uint64_t worldHash)
{
    ticks_.push_back({ input, worldHash });
}

bool ReplayRecorder::Finish()
{
    if (!active_) {
        return false;
    }
    active_ = false;
    header_.tickCount = ticks_.size();

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path_).parent_path(), ec);
    std::ofstream f(std::filesystem::path(path_), std::ios::binary);
    if (!f) {
        MYE_LOG_ERROR("[replay] cannot write %s", WideToUtf8(path_).c_str());
        return false;
    }
    f.write(reinterpret_cast<const char*>(&header_), sizeof(header_));
    f.write(reinterpret_cast<const char*>(ticks_.data()),
            static_cast<std::streamsize>(ticks_.size() * sizeof(ReplayTick)));
    MYE_LOG_INFO("[replay] recorded %llu ticks -> %s",
                 static_cast<unsigned long long>(ticks_.size()), WideToUtf8(path_).c_str());
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
    if (!f || header_.magic != 0x5045524Du) {
        MYE_LOG_ERROR("[replay] bad file magic");
        return false;
    }
    if (header_.version != 3 || header_.inputSize != sizeof(InputSnapshot)) {
        MYE_LOG_ERROR("[replay] incompatible version/layout (v%u, input %u bytes)",
                      header_.version, header_.inputSize);
        return false;
    }
    ticks_.resize(header_.tickCount);
    f.read(reinterpret_cast<char*>(ticks_.data()),
           static_cast<std::streamsize>(ticks_.size() * sizeof(ReplayTick)));
    if (!f) {
        MYE_LOG_ERROR("[replay] truncated file");
        return false;
    }
    active_ = true;
    MYE_LOG_INFO("[replay] loaded %llu ticks from %s",
                 static_cast<unsigned long long>(ticks_.size()), WideToUtf8(path).c_str());
    return true;
}

} // namespace mye
