//====================================================================================
//                          GameLiftHost.h
//  MyEngin/ 秋田蓮音                                                       11/27/2026
//                                          GameLift 窓の手順が外界 (aws CLI・Server.exe・クライアント) に触る口と、その実装
//====================================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Editor/Tools/GameLift/AwsCliCommands.h"
#include "Editor/Tools/GameLift/CliProcess.h"
#include "Editor/Tools/GameLift/ServerProcess.h"

namespace mye {

// aws CLI を 1 本ずつ走らせる口。手順 (GameLiftFlow) はこの型だけを知る。
// 実装は CliProcess を包んだ ProcessAwsCliRunner と、自己テストの偽 CLI (プロセス内の差し替え)
class IAwsCliRunner
{
public:
    virtual ~IAwsCliRunner() = default;

    // aws の場所を探す。見つからなければ false
    virtual bool Locate(std::wstring& awsPath) = 0;
    // call を走らせ始める (待たない)。実行中は false。失敗なら error に理由
    virtual bool Start(const AwsCall& call, uint32_t timeoutMs, std::string& error) = 0;
    virtual CliRunState Poll() = 0;
    virtual void Cancel() = 0;
    virtual uint32_t ExitCode() const = 0;
    virtual uint64_t ElapsedMs() const = 0;
    virtual std::string StdoutText() const = 0;
    virtual std::string StderrText() const = 0;
    // 出力を消す (秘密のメモリはゼロで埋めてから)
    virtual void ClearOutput() = 0;
};

// CliProcess + FindAwsExecutable。1 インスタンス同時 1 本
class ProcessAwsCliRunner : public IAwsCliRunner
{
public:
    bool Locate(std::wstring& awsPath) override;
    bool Start(const AwsCall& call, uint32_t timeoutMs, std::string& error) override;
    CliRunState Poll() override { return process_.Poll(); }
    void Cancel() override { process_.Cancel(); }
    uint32_t ExitCode() const override { return process_.ExitCode(); }
    uint64_t ElapsedMs() const override { return process_.ElapsedMs(); }
    std::string StdoutText() const override { return process_.StdoutText(); }
    std::string StderrText() const override { return process_.StderrText(); }
    void ClearOutput() override { process_.ClearOutput(); }

private:
    CliProcess process_;
    std::wstring awsPath_;
};

// 参加するクライアントの起動 (Runtime.exe)。独立したプロセスで、エディタが落ちても道連れにしない
struct ClientLaunchSpec
{
    std::wstring exePath;
    std::vector<std::wstring> args;
    std::wstring workDir;
};

// ローカルのプロセス (Server.exe とクライアント) に触る口。実装は LocalProcessHost と、自己テストの偽物
class ILocalProcessHost
{
public:
    virtual ~ILocalProcessHost() = default;

    virtual bool StartServer(const ServerLaunchSpec& spec, std::string& error) = 0;
    // 毎フレーム呼ぶ (ログを読み、終了と停止の猶予切れを見る)
    virtual void PollServer() = 0;
    virtual ServerView ServerStatus() const = 0;
    // イベントを立て、graceMs 内に終わらなければ強制終了する (待たない)
    virtual void RequestServerStop(uint32_t graceMs) = 0;
    // 終わるまで待つ停止 (エディタの終了用)
    virtual void StopServerBlocking(uint32_t graceMs) = 0;
    virtual bool LaunchClient(const ClientLaunchSpec& spec, std::string& error) = 0;
};

class LocalProcessHost : public ILocalProcessHost
{
public:
    bool StartServer(const ServerLaunchSpec& spec, std::string& error) override { return server_.Start(spec, error); }
    void PollServer() override { server_.Poll(); }
    ServerView ServerStatus() const override { return server_.View(); }
    void RequestServerStop(uint32_t graceMs) override { server_.RequestStop(graceMs); }
    void StopServerBlocking(uint32_t graceMs) override { server_.StopBlocking(graceMs); }
    bool LaunchClient(const ClientLaunchSpec& spec, std::string& error) override;

private:
    ServerProcess server_;
};

// 新しいコンソール窓で `aws login` を起動する (終わったら "pause" で結果を読ませる)。完了は待たない。
// 対話はユーザーがそちらで行い、終わったら窓の「再確認」を押す
bool OpenAwsLoginConsole(const std::wstring& awsPath, std::string& error);

} // namespace mye
