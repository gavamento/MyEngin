//====================================================================================
//                          TrackProjectId.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          projectId と pipe 名の計算 (固定表と一致させる)
//====================================================================================
#include "Editor/ChangeTracking/TrackProjectId.h"

#include "Engine/Platform/PathUtil.h"
#include "Engine/Platform/Sha256.h"

namespace mye {

namespace {
constexpr size_t kProjectIdLength = 16;
}

std::string ProjectIdSource(const std::wstring& projectRoot)
{
    const std::wstring key = NormalizePathKey(projectRoot);
    const bool isDriveRoot = key.size() == 3 && key[1] == L':' && key[2] == L'\\';
    if (isDriveRoot) {
        return WideToUtf8(key);
    }
    const size_t end = key.find_last_not_of(L'\\');
    // 全部が区切り (空にしかならない入力) はそのまま使う
    return WideToUtf8(end == std::wstring::npos ? key : key.substr(0, end + 1));
}

std::string ProjectId(const std::wstring& projectRoot)
{
    const std::string hex = Sha256Hex(ProjectIdSource(projectRoot));
    return hex.size() >= kProjectIdLength ? hex.substr(0, kProjectIdLength) : std::string();
}

std::string TrackPipeName(const std::string& projectId)
{
    return projectId.empty() ? std::string() : "\\\\.\\pipe\\mye-track-" + projectId;
}

} // namespace mye
