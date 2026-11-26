//====================================================================================
//                          GameLiftFlow.h
//  MyEngin/ 秋田蓮音                                                       11/27/2026
//                                          GameLift 窓の手順の状態機械 (状態確認と Anywhere の準備・起動・参加・片付け)
//====================================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "Editor/Tools/GameLift/AwsCliParse.h"
#include "Editor/Tools/GameLift/GameLiftHost.h"
#include "Editor/Tools/GameLift/GameLiftSettings.h"

namespace mye {

// 窓のボタン 1 つ = 1 つの操作
enum class FlowAction
{
    Prepare,     // ロケーション・フリート・コンピュートを無ければ作る (確認あり)
    StartServer, // トークンを取って Server.exe を起動する
    Join,        // ゲームセッションと player session を作ってクライアントを起動する
    StopServer,  // stop イベント → 猶予 → 強制終了
    Cleanup,     // サーバ停止 → deregister → delete-fleet → 消えるまで → delete-location (確認あり)
};
constexpr int kFlowActionCount = 5;

// ボタンが無効な理由 (窓が Tr() で文にする)
enum class BlockReason
{
    None,
    Busy,
    StatusUnknown, // まだ「再確認」が終わっていない
    CliMissing,    // aws.exe が無い / 版が読めない
    CliTooOld,     // v1
    NotLoggedIn,   // 資格情報が無い・切れた
    NoRegion,
    InvalidRegion,
    InvalidSuffix,
    InvalidFleetName,
    InvalidComputeName,
    InvalidIp,
    InvalidPort,
    InvalidMaxPlayers,
    NoProject,
    NoGameLogic,   // <project>\cache\GameLogic.dll が無い
    NoServerExe,
    NoRuntimeExe,
    NotPrepared,   // 保存済みのフリート ID が無い
    ServerAlreadyRunning,
    ServerNotRunning,
    ServerNotReady, // 起動中で ProcessReady がまだ
    ClientLimit,    // 最大人数ぶん起動済み
};

struct ActionState
{
    bool enabled = false;
    BlockReason reason = BlockReason::None;
};

// 手順の行に出す状態 (未 / 実行中 / 済 / 失敗)
enum class StepState
{
    NotDone,
    Running,
    Done,
    Failed,
};

enum class LoginState
{
    Unknown,
    LoggedIn,
    NotLoggedIn, // 資格情報が無い・切れた (aws login を促す)
    Error,       // それ以外の理由で確認できなかった (detail)
};

// 上段の状態 (RefreshStatus の結果)
struct FlowStatus
{
    bool checked = false;  // 一度でも確認が終わったか
    bool checking = false; // 確認中
    bool cliFound = false;
    std::wstring awsPath;
    bool versionKnown = false;
    AwsCliVersion version;
    LoginState login = LoginState::Unknown;
    AwsCallerIdentity identity;
    std::string detail;    // 確認に失敗したときの stderr の先頭行
};

// 窓が毎フレーム渡す環境 (プロジェクトとエディタの場所)
struct FlowEnvironment
{
    std::wstring projectRoot; // 空 = プロジェクト未オープン
    std::wstring exeDir;      // Server.exe / Runtime.exe のあるフォルダ (Editor.exe と同じ)
    std::wstring computerName;
};

// 失敗の種類 (窓が Tr() で文にし、detail を続ける)
enum class FlowFailure
{
    None,
    Aws,                      // aws CLI が失敗を返した (kind / code / detail)
    CliStartFailed,           // aws.exe を起動できなかった
    CliTimedOut,
    InvalidInput,             // 引数を組めなかった (窓は事前に検査するので通常は起きない)
    BadResponse,              // 応答を読めなかった
    NoSdkEndpoint,            // コンピュートに SDK エンドポイントが無い
    ServerStartFailed,
    ClientStartFailed,
    CapacityRetriesExhausted, // create-game-session が容量不足のまま上限まで再試行した
    SessionNotActive,         // ゲームセッションが ACTIVE にならず終わった (TERMINATED など)
    SessionWaitTimedOut,
    FleetDeleteTimedOut,
};

struct FlowError
{
    FlowFailure failure = FlowFailure::None;
    FlowAction action = FlowAction::Prepare;
    AwsErrorKind awsKind = AwsErrorKind::None;
    std::string code;   // 型付きエラーの名前 ("NotFoundException" など)
    std::string detail; // stderr の先頭 3 行、または内部の理由
};

enum class FlowOp
{
    None,
    Check,
    Prepare,
    StartServer,
    Join,
    Cleanup,
};

// 待ちの種類 (進捗の表示用)
enum class WaitKind
{
    None,
    CapacityRetry, // create-game-session の容量不足の再試行
    SessionActive, // ゲームセッションが ACTIVE になるまで
    FleetGone,     // フリートが消えるまで
    ServerStop,    // Server.exe が止まるまで (片付けの先頭)
};

struct FlowProgress
{
    FlowOp op = FlowOp::None;
    bool cliRunning = false;
    std::wstring cliLabel;        // 実行中の CLI の名前 (値と秘密を含まない)
    uint64_t cliElapsedMs = 0;
    WaitKind wait = WaitKind::None;
    int attempt = 0;
    int maxAttempts = 0;
};

// 確認ダイアログに出す内容
struct ConfirmInfo
{
    FlowAction action = FlowAction::Prepare;
    std::string region;
    std::string locationName;
    std::string fleetName;
    std::string computeName;
    std::string ipAddress;
    std::string fleetId; // 片付けで消す保存済みのフリート (空 = 無い)
};

// 待ちの間隔と上限 (既定は spec 4.1.5 の表。自己テストは短くする)
struct FlowTimings
{
    uint32_t cliTimeoutMs = 60000;
    uint32_t capacityRetryIntervalMs = 3000;
    int capacityRetryMax = 20;
    uint32_t sessionPollIntervalMs = 2000;
    uint32_t sessionWaitMaxMs = 60000;
    uint32_t fleetGonePollIntervalMs = 15000;
    uint32_t fleetGoneWaitMaxMs = 15u * 60u * 1000u;
    uint32_t serverStopGraceMs = 10000;
};

// Server.exe の環境変数名 (認証トークン。引数には出さない)
inline constexpr wchar_t kServerTokenEnvName[] = L"MYE_GAMELIFT_AUTH_TOKEN";

// --- 純関数 (自己テストで検査する) ---

struct AnywhereServerLaunchInput
{
    std::wstring exeDir;
    std::wstring projectRoot;
    std::string wsUrl;
    std::string fleetId;
    std::string computeName;
    int port = 0;
    int maxPlayers = 0;
    std::wstring stopEventName;
    std::wstring logPath;
    std::string authToken; // 子の環境だけへ入る
};
// Server.exe --project <proj> --hosting gamelift --gamelift-ws-url ... --stop-event <名前> + 環境にトークン
ServerLaunchSpec BuildAnywhereServerLaunch(const AnywhereServerLaunchInput& input);

// Runtime.exe --project <proj> --net-connect <host>:<port> --player-session-id <id> (作業ディレクトリ = プロジェクト)
ClientLaunchSpec BuildAnywhereClientLaunch(const std::wstring& exeDir, const std::wstring& projectRoot,
                                           const std::string& host, int port, const std::string& playerSessionId);

// プロジェクト起動の GameLogic.dll の場所 (BuildSettings / EngineLoop と同じ: <project>\cache\GameLogic.dll)
std::wstring ProjectGameLogicPath(const std::wstring& projectRoot);

// <project>\cache\gamelift\server-<stamp>.log
std::wstring ServerLogPathFor(const std::wstring& projectRoot, const std::wstring& stamp);

// GameLift 窓の手順の状態機械。UI から切り離してあり、自己テストは偽 CLI (IAwsCliRunner) と偽ホスト
// (ILocalProcessHost) を差して回す。窓が毎フレーム SetEnvironment → Tick → Query / 表示 の順に呼ぶ。
//
// ★1 窓で同時に走る CLI は 1 本。操作 (FlowOp) は 1 つずつで、前の CLI の結果を見て次を決める。
// ★作成・削除 (Prepare / Cleanup) は Request → Confirm の 2 段でしか始まらない (Decline なら CLI を 1 本も出さない)。
// ★認証トークンは get-compute-auth-token の応答から Server.exe の環境へ渡すだけ。ファイル・設定・ログに書かない
class GameLiftFlow
{
public:
    GameLiftFlow(IAwsCliRunner& runner, ILocalProcessHost& host, GameLiftSettings& settings,
                 FlowTimings timings = FlowTimings());

    void SetEnvironment(const FlowEnvironment& environment) { environment_ = environment; }
    // 設定 (保存済みの ID など) を書き換えたときに呼ぶ (窓が editor_settings.json へ保存する)
    void SetSettingsChangedHandler(std::function<void()> handler) { onSettingsChanged_ = std::move(handler); }

    // 毎フレーム。CLI の完了・待ちの満了・Server.exe の状態を進める (待たない)
    void Tick(uint64_t nowMs);

    // 上段の状態を確かめ直す (aws --version → リージョン → sts get-caller-identity)。実行中は何もしない
    void RefreshStatus();

    ActionState Query(FlowAction action) const;
    StepState Step(FlowAction action) const;

    // ボタンが押された。作成・削除は確認待ちにするだけ (Confirm で始まる)。それ以外は直ちに始める。無効なら false
    bool Request(FlowAction action);
    std::optional<ConfirmInfo> PendingConfirm() const;
    bool Confirm();
    void Decline();

    // 実行中の CLI を止めて操作を終える (AWS 側で受け付け済みの操作は取り消されない)
    void Cancel();
    // エディタの終了: CLI を止め、Server.exe を停止手順で止める (最大 serverStopGraceMs 待つ)
    void Shutdown();

    bool Busy() const { return op_ != FlowOp::None; }
    FlowOp CurrentOp() const { return op_; }
    const FlowStatus& Status() const { return status_; }
    const ServerView& Server() const { return server_; }
    const std::optional<FlowError>& LastError() const { return lastError_; }
    // 直近の操作が「取消」で終わった (AWS 側で受け付け済みの操作は取り消されない旨を窓が出す)
    bool WasCancelled() const { return cancelledNotice_; }
    FlowProgress Progress() const;
    int ClientsLaunched() const { return clientsLaunched_; }
    std::string EffectiveComputeName() const;

private:
    enum class Stage
    {
        Idle,
        CheckVersion,
        CheckRegion,
        CheckIdentity,
        PrepListLocations,
        PrepCreateLocation,
        PrepDescribeFleet,
        PrepCreateFleet,
        PrepDescribeCompute,
        PrepRegisterCompute,
        StartDescribeCompute,
        StartGetToken,
        JoinListActive,
        JoinCreateSession,
        JoinCreateRetryWait,
        JoinDescribeSession,
        JoinSessionPollWait,
        JoinCreatePlayer,
        CleanStopServer,
        CleanDeregister,
        CleanDeleteFleet,
        CleanDescribeFleet,
        CleanFleetPollWait,
        CleanDeleteLocation,
    };

    // 操作の結末 (手順の行の状態と「取消」の表示に使う)
    enum class Outcome
    {
        None,
        Succeeded,
        Failed,
        Cancelled,
    };

    // CLI 1 本の結果
    struct CallResult
    {
        uint32_t exitCode = 0;
        AwsError aws;
        std::string out;
        std::string err;
    };

    bool Begin(FlowAction action);
    void FinishOp(Outcome outcome);
    void FailWith(FlowFailure failure, const std::string& detail, const CallResult* result = nullptr);

    // 引数を組めなかった (入力が不正) ときは nullopt を渡す。実行中の CLI があれば Stage を進める
    bool Issue(const std::optional<AwsCall>& call, Stage next);
    void WaitFor(uint32_t ms, WaitKind kind, Stage next);
    void OnCallFinished(CliRunState state);
    void OnTimer();
    void UpdateServerView();
    void ChangedSettings();

    void HandleCheck(const CallResult& r);
    void HandlePrepare(const CallResult& r);
    void HandleStartServer(const CallResult& r);
    void HandleJoin(const CallResult& r);
    void HandleCleanup(const CallResult& r);

    void PrepareNextFleet();
    void PrepareNextCompute();
    void LaunchServerWithToken(const CallResult& r);
    void IssueCreateGameSession();
    void IssueDescribeSession();
    void IssueCreatePlayerSession();
    void LaunchClientFor(const AwsPlayerSession& session);
    void CleanupAfterServer();
    void CleanupDeleteLocation();
    void IssueDescribeFleetGone();

    BlockReason ValidateInputs(FlowAction action) const;

    IAwsCliRunner& runner_;
    ILocalProcessHost& host_;
    GameLiftSettings& settings_;
    FlowTimings timings_;
    FlowEnvironment environment_;
    std::function<void()> onSettingsChanged_;

    FlowStatus status_;
    ServerView server_;
    std::optional<FlowError> lastError_;
    std::optional<FlowAction> pending_;
    Outcome outcome_[kFlowActionCount] = {};
    bool cancelledNotice_ = false;

    FlowOp op_ = FlowOp::None;
    Stage stage_ = Stage::Idle;
    uint64_t now_ = 0;
    bool calling_ = false;
    std::wstring currentLabel_;
    bool currentSecret_ = false;

    WaitKind waitKind_ = WaitKind::None;
    uint64_t waitUntil_ = 0;
    int attempt_ = 0;
    int maxAttempts_ = 0;

    std::string sdkEndpoint_;
    std::string gameSessionId_;
    int clientsLaunched_ = 0;
    int playerSeq_ = 0;
    int stopEventSeq_ = 0;
    std::string playerIdPrefix_;
};

} // namespace mye
