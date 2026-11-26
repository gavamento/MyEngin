//====================================================================================
//                          GameLiftSettings.cpp
//  MyEngin/ 秋田蓮音                                                       11/27/2026
//                                          GameLift 窓の個人設定の検査と既定のコンピュート名の実装
//====================================================================================
#include "Editor/Tools/GameLift/GameLiftSettings.h"

#include "Editor/Tools/GameLift/AwsCliCommands.h"

namespace mye {
namespace {

constexpr char kFallbackComputeName[] = "mye-dev-pc";
constexpr size_t kMaxComputeNameLength = 64;

bool IsAsciiAlnum(wchar_t c)
{
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
}

} // namespace

void SanitizeGameLiftSettings(GameLiftSettings& settings)
{
    const GameLiftSettings defaults;
    if (!settings.region.empty() && !IsValidRegion(settings.region)) {
        settings.region.clear();
    }
    if (!IsValidLocationSuffix(settings.locationSuffix)) {
        settings.locationSuffix = defaults.locationSuffix;
    }
    if (!IsValidFleetName(settings.anywhereFleetName)) {
        settings.anywhereFleetName = defaults.anywhereFleetName;
    }
    if (!settings.computeName.empty() && !IsValidComputeName(settings.computeName)) {
        settings.computeName.clear();
    }
    if (!IsValidIpv4(settings.ipAddress)) {
        settings.ipAddress = defaults.ipAddress;
    }
    if (!IsValidPort(settings.port)) {
        settings.port = defaults.port;
    }
    if (!IsValidMaxPlayers(settings.maxPlayers)) {
        settings.maxPlayers = defaults.maxPlayers;
    }
    if (!settings.anywhereFleetId.empty() && !IsValidResourceId(settings.anywhereFleetId)) {
        settings.anywhereFleetId.clear();
    }
}

std::string DefaultComputeName(const std::wstring& computerName)
{
    std::string name;
    for (const wchar_t c : computerName) {
        if (name.size() >= kMaxComputeNameLength) {
            break;
        }
        const bool keep = IsAsciiAlnum(c) || c == L'-';
        if (name.empty() && !IsAsciiAlnum(c)) {
            continue; // 先頭は英数 (IsValidComputeName の条件)。'-' や記号は落とす
        }
        name.push_back(keep ? static_cast<char>(c) : '-');
    }
    return name.empty() ? std::string(kFallbackComputeName) : name;
}

} // namespace mye
