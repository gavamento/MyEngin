//====================================================================================
//                          OwnSaveRecord.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          自分の保存の直後に控えたファイルの印 (実装)
//====================================================================================
#include "Editor/App/OwnSaveRecord.h"

#include <filesystem>

#include "Engine/Platform/PathUtil.h"

namespace mye {

FileStamp StatFileStamp(const std::wstring& absPath)
{
    FileStamp stamp;
    std::error_code ec;
    const std::filesystem::path path(absPath);
    const uint64_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        return stamp;
    }
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return stamp;
    }
    stamp.valid = true;
    stamp.size = size;
    stamp.writeTime = static_cast<int64_t>(time.time_since_epoch().count());
    return stamp;
}

void OwnSaveRecord::Note(const std::wstring& absPath)
{
    Note(absPath, StatFileStamp(absPath));
}

void OwnSaveRecord::Note(const std::wstring& absPath, const FileStamp& stamp)
{
    const std::wstring key = NormalizePathKey(absPath);
    if (key.empty()) {
        return;
    }
    if (stamp.valid) {
        noted_[key] = stamp;
    } else {
        noted_.erase(key); // 読めなかった保存は、古い印を残さない (違うファイルの印で捨てないため)
    }
}

bool OwnSaveRecord::Matches(const FileStamp& noted, const FileStamp& current)
{
    return noted.valid && current.valid && noted == current;
}

bool OwnSaveRecord::MatchesNow(const std::wstring& pendingPath) const
{
    const auto it = noted_.find(NormalizePathKey(pendingPath));
    return it != noted_.end() && Matches(it->second, StatFileStamp(pendingPath));
}

} // namespace mye
