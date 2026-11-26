//====================================================================================
//                          GameLiftWindow.h
//  MyEngin/ 秋田蓮音                                                       11/27/2026
//                                          GameLift 窓 (AWS GameLift を操作する ImGui 窓。状態確認・Anywhere の手順・確認ダイアログ)
//====================================================================================
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "Editor/Tools/GameLift/GameLiftFlow.h"
#include "Editor/Tools/GameLift/GameLiftHost.h"
#include "Editor/Tools/GameLift/GameLiftSettings.h"

namespace mye {

// 入力欄のバッファを設定の値へ揃える。違えば写して true (呼び出し側は編集中の ImGui の入力状態を捨てる。
// 捨てないと編集中の欄は外から書き換えたバッファを無視し、古い文字のまま設定を上書きする)
bool SyncTextBuffer(char* buffer, size_t size, const std::string& value);

// 窓が毎フレーム EditorApp から受け取る環境
struct GameLiftWindowEnv
{
    std::wstring projectRoot; // 空 = プロジェクト未オープン
    std::wstring exeDir;      // Editor.exe のフォルダ (Server.exe / Runtime.exe の場所)
};

// ドック可能な「GameLift」窓。手順の状態機械 (GameLiftFlow) と、aws CLI・Server.exe の実プロセスを所有する。
// 窓は描くだけで、AWS に触る判断 (有効・無効、確認、順序) は GameLiftFlow が持つ。毎フレーム
//   1. Pump    : 窓が閉じていても呼ぶ (CLI の完了・Server.exe のログと終了を見る。呼ばないとサーバの監視が止まる)
//   2. OnImGui : 開いているときだけ描く
// の順。窓を閉じても動いている Server.exe は残る。エディタ終了時は Shutdown で停止手順を踏む
class GameLiftWindow
{
public:
    bool open = false;

    // 自己テスト・スクショ検証用: aws CLI と Server.exe の実プロセスの代わりに差し替える。Bind より前に呼ぶ
    void UseBackends(IAwsCliRunner& runner, ILocalProcessHost& host);

    // 設定 (保存済みの値を持つ) と保存の手段を受け取り、手順を組む。起動時に 1 回
    void Bind(GameLiftSettings& settings, std::function<void()> saveSettings);

    void Pump(const GameLiftWindowEnv& env);
    void OnImGui(const GameLiftWindowEnv& env);
    void Shutdown();

    // 手順の状態機械 (Bind 前は null)。スクショ検証が Request と同じ経路で状態を作るのに使う
    GameLiftFlow* Flow() { return flow_.get(); }

private:
    void DrawStatus();
    void DrawAnywhereTab();
    void DrawStepRow(FlowAction action, const char* buttonLabel);
    void DrawServerBlock();
    void DrawBottom();
    void DrawConfirmPopup();
    void SyncBuffersFromSettings();

    ProcessAwsCliRunner ownedRunner_;
    LocalProcessHost ownedHost_;
    IAwsCliRunner* runner_ = &ownedRunner_;
    ILocalProcessHost* host_ = &ownedHost_;
    std::unique_ptr<GameLiftFlow> flow_;
    GameLiftSettings* settings_ = nullptr;
    std::function<void()> saveSettings_;

    char region_[64] = {};
    char suffix_[96] = {};
    char fleetName_[96] = {};
    char computeName_[96] = {};
    char ipAddress_[64] = {};
    std::wstring computerName_;
    bool wasOpen_ = false;
    bool confirmIsPrepare_ = true; // 直近に出した確認ダイアログの種類 (ポップアップ ID の選択用)
    std::string loginOpenError_;
};

} // namespace mye
