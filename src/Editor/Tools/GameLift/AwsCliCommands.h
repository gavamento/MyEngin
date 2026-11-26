//====================================================================================
//                          AwsCliCommands.h
//  MyEngin/ 秋田蓮音                                                       11/27/2026
//                                          aws CLI 呼び出しの argv 組み立てと利用者入力の検査 (純関数)
//====================================================================================
#pragma once

#include <string>
#include <vector>

namespace mye {

// aws CLI を 1 回呼ぶための argv (exe 自身は含まない)。label は UI に出してよい名前 (値や秘密を含まない)
struct AwsCall
{
    std::vector<std::wstring> args;
    std::wstring label;      // 例: "gamelift describe-compute"
    bool secret = false;     // 出力にトークンを含む (get-compute-auth-token)
};

// --- 利用者が入れる値の検査 (ASCII の英数と '-' '_' '.'、先頭は英数。'-' 始まりを許すと CLI のオプションに化ける) ---

// ロケーションの接尾辞 (表示と API 名は "custom-" + 接尾辞)。"custom-" 始まりは二重になるので拒否
bool IsValidLocationSuffix(const std::string& suffix);
bool IsValidFleetName(const std::string& name);
bool IsValidComputeName(const std::string& name);
bool IsValidBuildName(const std::string& name);
bool IsValidBuildVersion(const std::string& version);
bool IsValidInstanceType(const std::string& type);
bool IsValidPlayerId(const std::string& id);
// ドット区切りの 4 数 (各 0〜255)
bool IsValidIpv4(const std::string& ip);
bool IsValidPort(int port);       // 1〜65535
bool IsValidMaxPlayers(int count); // 1〜4
// リージョン名 (小文字・数字・'-')
bool IsValidRegion(const std::string& region);
// AWS が返した ID / ARN (フリート ID・ビルド ID・ゲームセッション ID など)。':' と '/' を許す
bool IsValidResourceId(const std::string& id);

// 利用者入力の接尾辞 → API 上のロケーション名
std::string LocationName(const std::string& suffix);

// --- argv の組み立て。入力が不正なら false (out は変えない)。region は gamelift 系で必須、sts では空なら省く ---

AwsCall AwsVersionCall();
AwsCall AwsConfigureGetRegionCall();
bool AwsStsGetCallerIdentityCall(const std::string& region, AwsCall& out);

bool GameLiftListCustomLocationsCall(const std::string& region, AwsCall& out);
bool GameLiftCreateLocationCall(const std::string& region, const std::string& suffix, AwsCall& out);
bool GameLiftDeleteLocationCall(const std::string& region, const std::string& suffix, AwsCall& out);

bool GameLiftCreateAnywhereFleetCall(const std::string& region, const std::string& fleetName,
                                     const std::string& locationSuffix, AwsCall& out);
// EC2 フリート。入力は JSON ファイル (sub-05 が書く)
bool GameLiftCreateFleetFromJsonCall(const std::string& region, const std::wstring& jsonPath, AwsCall& out);
bool GameLiftDeleteFleetCall(const std::string& region, const std::string& fleetId, AwsCall& out);
bool GameLiftDescribeFleetAttributesCall(const std::string& region, const std::string& fleetId, AwsCall& out);
bool GameLiftDescribeFleetCapacityCall(const std::string& region, const std::string& fleetId, AwsCall& out);
bool GameLiftDescribeFleetEventsCall(const std::string& region, const std::string& fleetId, int limit, AwsCall& out);
bool GameLiftUpdateFleetCapacityCall(const std::string& region, const std::string& fleetId, int desiredInstances,
                                     AwsCall& out);

bool GameLiftRegisterComputeCall(const std::string& region, const std::string& fleetId, const std::string& computeName,
                                 const std::string& ipAddress, const std::string& locationSuffix, AwsCall& out);
bool GameLiftDeregisterComputeCall(const std::string& region, const std::string& fleetId,
                                   const std::string& computeName, AwsCall& out);
bool GameLiftDescribeComputeCall(const std::string& region, const std::string& fleetId, const std::string& computeName,
                                 AwsCall& out);
bool GameLiftGetComputeAuthTokenCall(const std::string& region, const std::string& fleetId,
                                     const std::string& computeName, AwsCall& out);

// locationSuffix が空なら --location を付けない (EC2 フリート)
bool GameLiftCreateGameSessionCall(const std::string& region, const std::string& fleetId,
                                   const std::string& locationSuffix, int maxPlayers, AwsCall& out);
struct DescribeGameSessionsQuery
{
    std::string fleetId;
    std::string locationSuffix; // 空 = 指定しない
    std::string gameSessionId;  // 空 = 指定しない
    bool activeOnly = false;    // --status-filter ACTIVE
};
bool GameLiftDescribeGameSessionsCall(const std::string& region, const DescribeGameSessionsQuery& query, AwsCall& out);
bool GameLiftCreatePlayerSessionCall(const std::string& region, const std::string& gameSessionId,
                                     const std::string& playerId, AwsCall& out);

// build-root は書き出したサーバ版のフォルダ。OS は WINDOWS_2022、SDK は 5.6.0 固定 (spec 4.1.6)
bool GameLiftUploadBuildCall(const std::string& region, const std::string& buildName, const std::string& buildVersion,
                             const std::wstring& buildRoot, AwsCall& out);
bool GameLiftDescribeBuildCall(const std::string& region, const std::string& buildId, AwsCall& out);
bool GameLiftDeleteBuildCall(const std::string& region, const std::string& buildId, AwsCall& out);

// --- aws.exe の探索 ---

// 探索順の候補 (存在確認はしない): 設定の明示パス → searchPathHit (SearchPathW の結果。無ければ空) → 既定のインストール先
std::vector<std::wstring> AwsExecutableCandidates(const std::wstring& configuredPath, const std::wstring& searchPathHit);
// 実環境で探す。見つからなければ空
std::wstring FindAwsExecutable(const std::wstring& configuredPath);

} // namespace mye
