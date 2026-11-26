//====================================================================================
//                          AwsCliParse.h
//  MyEngin/ 秋田蓮音                                                       11/27/2026
//                                          aws CLI の応答 (JSON / テキスト) の解析とエラー分類 (純関数)
//====================================================================================
#pragma once

#include <string>
#include <vector>

namespace mye {

// --- エラー分類 ---

enum class AwsErrorKind
{
    None,             // 成功
    Credentials,      // 資格情報が無い・切れた (aws login を促す)
    NotFound,         // NotFoundException
    CapacityExceeded, // FleetCapacityExceededException
    Conflict,         // ConflictException 等 (既にある)
    InvalidRequest,   // InvalidRequestException
    NoRegion,         // リージョン未設定
    Other,            // 上記以外 (message に stderr の先頭行)
};

struct AwsError
{
    AwsErrorKind kind = AwsErrorKind::None;
    std::string code;      // "NotFoundException" など。型付きエラーでなければ空
    std::string operation; // "DescribeFleetAttributes" など。無ければ空
    std::string message;   // 人へ見せる 1 行 (Other では stderr の先頭の非空行)
};

// 終了コードと stderr から分類する。exitCode == 0 なら None
AwsError ClassifyAwsFailure(int exitCode, const std::string& stderrText);

// stderr の先頭から最大 maxLines 行 (空行を除く) を改行つなぎで返す (窓の「直近のエラー」用)
std::string FirstErrorLines(const std::string& stderrText, int maxLines);

// --- テキスト応答 ---

// `aws --version` ("aws-cli/2.15.30 Python/3.11.8 ..."。v1 は stderr に出すので呼び出し側が結合して渡す)
struct AwsCliVersion
{
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::string text; // 先頭の 1 行
};
bool ParseAwsCliVersion(const std::string& text, AwsCliVersion& out);

// `aws configure get region` などの 1 行テキスト。前後の空白を除いた先頭行 (空なら false)
bool ParseSingleLineText(const std::string& text, std::string& out);

// `upload-build` の stdout (進捗は '\r' 区切りでも来る)
struct AwsUploadProgress
{
    double percent = -1.0; // 最後に読めた "(21.45%)" の値。まだ無ければ -1
    std::string buildId;   // "Build ID: build-..." の値。まだ無ければ空
};
AwsUploadProgress ParseUploadBuildOutput(const std::string& text);

// --- JSON 応答。失敗 (JSON でない・必須キーが無い) は false と error ---

struct AwsCallerIdentity
{
    std::string account;
    std::string arn;
    std::string userId;
};
bool ParseCallerIdentity(const std::string& json, AwsCallerIdentity& out, std::string& error);

struct AwsLocation
{
    std::string name;
    std::string arn;
};
bool ParseListLocations(const std::string& json, std::vector<AwsLocation>& out, std::string& error);
bool ParseCreateLocation(const std::string& json, AwsLocation& out, std::string& error);

struct AwsFleetAttributes
{
    std::string fleetId;
    std::string fleetArn;
    std::string name;
    std::string status; // NEW / DOWNLOADING / VALIDATING / BUILDING / ACTIVATING / ACTIVE / DELETING / ERROR ...
    std::string computeType;
    std::string instanceType;
    std::string buildId;
};
// create-fleet は FleetAttributes がオブジェクト
bool ParseCreateFleet(const std::string& json, AwsFleetAttributes& out, std::string& error);
// describe-fleet-attributes は配列 (消えたフリートは空配列 = 成功)
bool ParseDescribeFleetAttributes(const std::string& json, std::vector<AwsFleetAttributes>& out, std::string& error);

struct AwsFleetCapacity
{
    std::string fleetId;
    std::string instanceType;
    std::string location;
    int desired = 0;
    int minimum = 0;
    int maximum = 0;
    int pending = 0;
    int active = 0;
    int idle = 0;
    int terminating = 0;

    // 課金が続いている台数 (ACTIVE + PENDING)
    int Billable() const { return active + pending; }
};
bool ParseDescribeFleetCapacity(const std::string& json, std::vector<AwsFleetCapacity>& out, std::string& error);

struct AwsFleetEvent
{
    std::string eventCode;
    std::string message;
    std::string eventTime;
};
bool ParseDescribeFleetEvents(const std::string& json, std::vector<AwsFleetEvent>& out, std::string& error);

struct AwsCompute
{
    std::string fleetId;
    std::string computeName;
    std::string computeArn;
    std::string status;
    std::string location;
    std::string ipAddress;
    std::string dnsName;
    std::string sdkEndpoint; // GameLiftServiceSdkEndpoint (Server.exe の --gamelift-ws-url)
};
// register-compute と describe-compute は同じ形 ({"Compute": {...}})
bool ParseCompute(const std::string& json, AwsCompute& out, std::string& error);

// 認証トークンは秘密。使ったら Wipe する
struct AwsComputeAuthToken
{
    std::string computeName;
    std::string authToken;
    std::string expiration;
    void Wipe();
};
bool ParseComputeAuthToken(const std::string& json, AwsComputeAuthToken& out, std::string& error);

struct AwsGameSession
{
    std::string gameSessionId;
    std::string fleetId;
    std::string status; // ACTIVATING / ACTIVE / TERMINATED ...
    std::string statusReason;
    std::string location;
    std::string ipAddress;
    std::string dnsName;
    int port = 0;
    int currentPlayers = 0;
    int maxPlayers = 0;
};
bool ParseCreateGameSession(const std::string& json, AwsGameSession& out, std::string& error);
bool ParseDescribeGameSessions(const std::string& json, std::vector<AwsGameSession>& out, std::string& error);

struct AwsPlayerSession
{
    std::string playerSessionId;
    std::string playerId;
    std::string gameSessionId;
    std::string status;
    std::string ipAddress;
    std::string dnsName;
    int port = 0;
};
bool ParseCreatePlayerSession(const std::string& json, AwsPlayerSession& out, std::string& error);

struct AwsBuild
{
    std::string buildId;
    std::string name;
    std::string version;
    std::string status; // INITIALIZED / READY / FAILED
    std::string operatingSystem;
};
bool ParseDescribeBuild(const std::string& json, AwsBuild& out, std::string& error);

} // namespace mye
