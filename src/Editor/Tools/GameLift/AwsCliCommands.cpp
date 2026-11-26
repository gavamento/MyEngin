//====================================================================================
//                          AwsCliCommands.cpp
//  MyEngin/ 秋田蓮音                                                       11/27/2026
//                                          aws CLI 呼び出しの argv 組み立てと利用者入力の検査の実装
//====================================================================================
#include "Editor/Tools/GameLift/AwsCliCommands.h"

#include <algorithm>

#include <Windows.h>

namespace mye {
namespace {

constexpr size_t kMaxNameLength = 64;
constexpr size_t kMaxSuffixLength = 50; // "custom-" (7) を足しても API の上限 64 に収まる
constexpr size_t kMaxIdLength = 512;    // ゲームセッション ID は ARN で長い
constexpr int kMaxPort = 65535;
constexpr int kMaxPlayersLimit = 4;
constexpr int kMaxEventLimit = 100;
constexpr wchar_t kDefaultAwsInstallPath[] = L"C:\\Program Files\\Amazon\\AWSCLIV2\\aws.exe";
constexpr char kLocationPrefixNarrow[] = "custom-";

bool IsAsciiAlnum(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// 先頭が英数、残りが英数か allowed のいずれか、長さ 1〜maxLen
bool IsSimpleToken(const std::string& text, size_t maxLen, const char* allowed)
{
    if (text.empty() || text.size() > maxLen || !IsAsciiAlnum(text.front())) {
        return false;
    }
    for (const char c : text) {
        if (!IsAsciiAlnum(c) && std::string(allowed).find(c) == std::string::npos) {
            return false;
        }
    }
    return true;
}

// 検査済みの ASCII を UTF-16 へ (検査前の文字列を渡さない)
std::wstring Widen(const std::string& ascii)
{
    return std::wstring(ascii.begin(), ascii.end());
}

std::wstring IntToWide(int value)
{
    return std::to_wstring(value);
}

// gamelift <op> <opArgs...> --output json --no-cli-pager --region <region>
AwsCall MakeGameLiftCall(const wchar_t* op, std::vector<std::wstring> opArgs, const std::string& region,
                         bool secret = false)
{
    AwsCall call;
    call.args = { L"gamelift", op };
    call.args.insert(call.args.end(), std::make_move_iterator(opArgs.begin()), std::make_move_iterator(opArgs.end()));
    call.args.insert(call.args.end(), { L"--output", L"json", L"--no-cli-pager", L"--region", Widen(region) });
    call.label = std::wstring(L"gamelift ") + op;
    call.secret = secret;
    return call;
}

// fleet-id / compute-name の組を取る 3 操作の共通部
bool BuildComputeCall(const wchar_t* op, const std::string& region, const std::string& fleetId,
                      const std::string& computeName, bool secret, AwsCall& out)
{
    if (!IsValidRegion(region) || !IsValidResourceId(fleetId) || !IsValidComputeName(computeName)) {
        return false;
    }
    out = MakeGameLiftCall(op, { L"--fleet-id", Widen(fleetId), L"--compute-name", Widen(computeName) }, region, secret);
    return true;
}

} // namespace

bool IsValidLocationSuffix(const std::string& suffix)
{
    return IsSimpleToken(suffix, kMaxSuffixLength, "-_.") && suffix.rfind(kLocationPrefixNarrow, 0) != 0;
}

bool IsValidFleetName(const std::string& name)
{
    return IsSimpleToken(name, kMaxNameLength, "-_.");
}

bool IsValidComputeName(const std::string& name)
{
    return IsSimpleToken(name, kMaxNameLength, "-_.");
}

bool IsValidBuildName(const std::string& name)
{
    return IsSimpleToken(name, kMaxNameLength, "-_.");
}

bool IsValidBuildVersion(const std::string& version)
{
    return IsSimpleToken(version, kMaxNameLength, "-_.");
}

bool IsValidInstanceType(const std::string& type)
{
    return IsSimpleToken(type, 32, "-_.");
}

bool IsValidPlayerId(const std::string& id)
{
    return IsSimpleToken(id, kMaxNameLength, "-_.");
}

bool IsValidIpv4(const std::string& ip)
{
    int parts = 0;
    size_t pos = 0;
    while (pos <= ip.size()) {
        const size_t dot = ip.find('.', pos);
        const std::string part = ip.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
        if (part.empty() || part.size() > 3 || (part.size() > 1 && part.front() == '0')) {
            return false;
        }
        int value = 0;
        for (const char c : part) {
            if (c < '0' || c > '9') {
                return false;
            }
            value = value * 10 + (c - '0');
        }
        if (value > 255) {
            return false;
        }
        ++parts;
        if (dot == std::string::npos) {
            break;
        }
        pos = dot + 1;
    }
    return parts == 4;
}

bool IsValidPort(int port)
{
    return port >= 1 && port <= kMaxPort;
}

bool IsValidMaxPlayers(int count)
{
    return count >= 1 && count <= kMaxPlayersLimit;
}

bool IsValidRegion(const std::string& region)
{
    if (region.size() < 3 || region.size() > 30 || region.front() < 'a' || region.front() > 'z') {
        return false;
    }
    return std::all_of(region.begin(), region.end(),
                       [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

bool IsValidResourceId(const std::string& id)
{
    return IsSimpleToken(id, kMaxIdLength, "-_.:/");
}

std::string LocationName(const std::string& suffix)
{
    return std::string(kLocationPrefixNarrow) + suffix;
}

AwsCall AwsVersionCall()
{
    AwsCall call;
    call.args = { L"--version" };
    call.label = L"--version";
    return call;
}

AwsCall AwsConfigureGetRegionCall()
{
    AwsCall call;
    call.args = { L"configure", L"get", L"region" };
    call.label = L"configure get region";
    return call;
}

bool AwsStsGetCallerIdentityCall(const std::string& region, AwsCall& out)
{
    if (!region.empty() && !IsValidRegion(region)) {
        return false;
    }
    AwsCall call;
    call.args = { L"sts", L"get-caller-identity", L"--output", L"json", L"--no-cli-pager" };
    if (!region.empty()) {
        call.args.insert(call.args.end(), { L"--region", Widen(region) });
    }
    call.label = L"sts get-caller-identity";
    out = std::move(call);
    return true;
}

bool GameLiftListCustomLocationsCall(const std::string& region, AwsCall& out)
{
    if (!IsValidRegion(region)) {
        return false;
    }
    out = MakeGameLiftCall(L"list-locations", { L"--filters", L"CUSTOM" }, region);
    return true;
}

bool GameLiftCreateLocationCall(const std::string& region, const std::string& suffix, AwsCall& out)
{
    if (!IsValidRegion(region) || !IsValidLocationSuffix(suffix)) {
        return false;
    }
    out = MakeGameLiftCall(L"create-location", { L"--location-name", Widen(LocationName(suffix)) }, region);
    return true;
}

bool GameLiftDeleteLocationCall(const std::string& region, const std::string& suffix, AwsCall& out)
{
    if (!IsValidRegion(region) || !IsValidLocationSuffix(suffix)) {
        return false;
    }
    out = MakeGameLiftCall(L"delete-location", { L"--location-name", Widen(LocationName(suffix)) }, region);
    return true;
}

bool GameLiftCreateAnywhereFleetCall(const std::string& region, const std::string& fleetName,
                                     const std::string& locationSuffix, AwsCall& out)
{
    if (!IsValidRegion(region) || !IsValidFleetName(fleetName) || !IsValidLocationSuffix(locationSuffix)) {
        return false;
    }
    out = MakeGameLiftCall(L"create-fleet",
                           { L"--name", Widen(fleetName), L"--compute-type", L"ANYWHERE", L"--locations",
                             std::wstring(L"Location=") + Widen(LocationName(locationSuffix)) },
                           region);
    return true;
}

bool GameLiftCreateFleetFromJsonCall(const std::string& region, const std::wstring& jsonPath, AwsCall& out)
{
    if (!IsValidRegion(region) || jsonPath.empty()) {
        return false;
    }
    out = MakeGameLiftCall(L"create-fleet", { L"--cli-input-json", L"file://" + jsonPath }, region);
    return true;
}

bool GameLiftDeleteFleetCall(const std::string& region, const std::string& fleetId, AwsCall& out)
{
    if (!IsValidRegion(region) || !IsValidResourceId(fleetId)) {
        return false;
    }
    out = MakeGameLiftCall(L"delete-fleet", { L"--fleet-id", Widen(fleetId) }, region);
    return true;
}

bool GameLiftDescribeFleetAttributesCall(const std::string& region, const std::string& fleetId, AwsCall& out)
{
    if (!IsValidRegion(region) || !IsValidResourceId(fleetId)) {
        return false;
    }
    out = MakeGameLiftCall(L"describe-fleet-attributes", { L"--fleet-ids", Widen(fleetId) }, region);
    return true;
}

bool GameLiftDescribeFleetCapacityCall(const std::string& region, const std::string& fleetId, AwsCall& out)
{
    if (!IsValidRegion(region) || !IsValidResourceId(fleetId)) {
        return false;
    }
    out = MakeGameLiftCall(L"describe-fleet-capacity", { L"--fleet-ids", Widen(fleetId) }, region);
    return true;
}

bool GameLiftDescribeFleetEventsCall(const std::string& region, const std::string& fleetId, int limit, AwsCall& out)
{
    if (!IsValidRegion(region) || !IsValidResourceId(fleetId) || limit < 1 || limit > kMaxEventLimit) {
        return false;
    }
    out = MakeGameLiftCall(L"describe-fleet-events", { L"--fleet-id", Widen(fleetId), L"--limit", IntToWide(limit) },
                           region);
    return true;
}

bool GameLiftUpdateFleetCapacityCall(const std::string& region, const std::string& fleetId, int desiredInstances,
                                     AwsCall& out)
{
    if (!IsValidRegion(region) || !IsValidResourceId(fleetId) || desiredInstances < 0 || desiredInstances > 1) {
        return false;
    }
    out = MakeGameLiftCall(L"update-fleet-capacity",
                           { L"--fleet-id", Widen(fleetId), L"--desired-instances", IntToWide(desiredInstances),
                             L"--min-size", L"0", L"--max-size", L"1" },
                           region);
    return true;
}

bool GameLiftRegisterComputeCall(const std::string& region, const std::string& fleetId, const std::string& computeName,
                                 const std::string& ipAddress, const std::string& locationSuffix, AwsCall& out)
{
    if (!IsValidRegion(region) || !IsValidResourceId(fleetId) || !IsValidComputeName(computeName) ||
        !IsValidIpv4(ipAddress) || !IsValidLocationSuffix(locationSuffix)) {
        return false;
    }
    out = MakeGameLiftCall(L"register-compute",
                           { L"--fleet-id", Widen(fleetId), L"--compute-name", Widen(computeName), L"--ip-address",
                             Widen(ipAddress), L"--location", Widen(LocationName(locationSuffix)) },
                           region);
    return true;
}

bool GameLiftDeregisterComputeCall(const std::string& region, const std::string& fleetId,
                                   const std::string& computeName, AwsCall& out)
{
    return BuildComputeCall(L"deregister-compute", region, fleetId, computeName, false, out);
}

bool GameLiftDescribeComputeCall(const std::string& region, const std::string& fleetId, const std::string& computeName,
                                 AwsCall& out)
{
    return BuildComputeCall(L"describe-compute", region, fleetId, computeName, false, out);
}

bool GameLiftGetComputeAuthTokenCall(const std::string& region, const std::string& fleetId,
                                     const std::string& computeName, AwsCall& out)
{
    return BuildComputeCall(L"get-compute-auth-token", region, fleetId, computeName, true, out);
}

bool GameLiftCreateGameSessionCall(const std::string& region, const std::string& fleetId,
                                   const std::string& locationSuffix, int maxPlayers, AwsCall& out)
{
    if (!IsValidRegion(region) || !IsValidResourceId(fleetId) || !IsValidMaxPlayers(maxPlayers) ||
        (!locationSuffix.empty() && !IsValidLocationSuffix(locationSuffix))) {
        return false;
    }
    std::vector<std::wstring> opArgs = { L"--fleet-id", Widen(fleetId) };
    if (!locationSuffix.empty()) {
        opArgs.insert(opArgs.end(), { L"--location", Widen(LocationName(locationSuffix)) });
    }
    opArgs.insert(opArgs.end(), { L"--maximum-player-session-count", IntToWide(maxPlayers) });
    out = MakeGameLiftCall(L"create-game-session", std::move(opArgs), region);
    return true;
}

bool GameLiftDescribeGameSessionsCall(const std::string& region, const DescribeGameSessionsQuery& query, AwsCall& out)
{
    if (!IsValidRegion(region) || !IsValidResourceId(query.fleetId) ||
        (!query.locationSuffix.empty() && !IsValidLocationSuffix(query.locationSuffix)) ||
        (!query.gameSessionId.empty() && !IsValidResourceId(query.gameSessionId))) {
        return false;
    }
    std::vector<std::wstring> opArgs = { L"--fleet-id", Widen(query.fleetId) };
    if (!query.locationSuffix.empty()) {
        opArgs.insert(opArgs.end(), { L"--location", Widen(LocationName(query.locationSuffix)) });
    }
    if (!query.gameSessionId.empty()) {
        opArgs.insert(opArgs.end(), { L"--game-session-id", Widen(query.gameSessionId) });
    }
    if (query.activeOnly) {
        opArgs.insert(opArgs.end(), { L"--status-filter", L"ACTIVE" });
    }
    out = MakeGameLiftCall(L"describe-game-sessions", std::move(opArgs), region);
    return true;
}

bool GameLiftCreatePlayerSessionCall(const std::string& region, const std::string& gameSessionId,
                                     const std::string& playerId, AwsCall& out)
{
    if (!IsValidRegion(region) || !IsValidResourceId(gameSessionId) || !IsValidPlayerId(playerId)) {
        return false;
    }
    out = MakeGameLiftCall(L"create-player-session",
                           { L"--game-session-id", Widen(gameSessionId), L"--player-id", Widen(playerId) }, region);
    return true;
}

bool GameLiftUploadBuildCall(const std::string& region, const std::string& buildName, const std::string& buildVersion,
                             const std::wstring& buildRoot, AwsCall& out)
{
    if (!IsValidRegion(region) || !IsValidBuildName(buildName) || !IsValidBuildVersion(buildVersion) ||
        buildRoot.empty()) {
        return false;
    }
    out = MakeGameLiftCall(L"upload-build",
                           { L"--name", Widen(buildName), L"--build-version", Widen(buildVersion), L"--build-root",
                             buildRoot, L"--operating-system", L"WINDOWS_2022", L"--server-sdk-version", L"5.6.0" },
                           region);
    return true;
}

bool GameLiftDescribeBuildCall(const std::string& region, const std::string& buildId, AwsCall& out)
{
    if (!IsValidRegion(region) || !IsValidResourceId(buildId)) {
        return false;
    }
    out = MakeGameLiftCall(L"describe-build", { L"--build-id", Widen(buildId) }, region);
    return true;
}

bool GameLiftDeleteBuildCall(const std::string& region, const std::string& buildId, AwsCall& out)
{
    if (!IsValidRegion(region) || !IsValidResourceId(buildId)) {
        return false;
    }
    out = MakeGameLiftCall(L"delete-build", { L"--build-id", Widen(buildId) }, region);
    return true;
}

std::vector<std::wstring> AwsExecutableCandidates(const std::wstring& configuredPath, const std::wstring& searchPathHit)
{
    std::vector<std::wstring> candidates;
    for (const std::wstring& path : { configuredPath, searchPathHit, std::wstring(kDefaultAwsInstallPath) }) {
        if (!path.empty() && std::find(candidates.begin(), candidates.end(), path) == candidates.end()) {
            candidates.push_back(path);
        }
    }
    return candidates;
}

std::wstring FindAwsExecutable(const std::wstring& configuredPath)
{
    wchar_t found[MAX_PATH] = {};
    std::wstring searchHit;
    const DWORD length = SearchPathW(nullptr, L"aws.exe", nullptr, MAX_PATH, found, nullptr);
    if (length > 0 && length < MAX_PATH) {
        searchHit.assign(found, length);
    }
    for (const std::wstring& candidate : AwsExecutableCandidates(configuredPath, searchHit)) {
        const DWORD attributes = GetFileAttributesW(candidate.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return candidate;
        }
    }
    return std::wstring();
}

} // namespace mye
