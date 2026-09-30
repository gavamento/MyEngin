#pragma once
#include <string>

namespace mye {

// プロジェクトマネージャ (Hub) の結果 (M26b)
enum class ProjectManagerAction {
    Exit,        // ウィンドウを閉じた (何も開かない)
    OpenProject, // projectRoot を開く (呼び出し側が再起動する)
};

struct ProjectManagerOutcome {
    ProjectManagerAction action = ProjectManagerAction::Exit;
    std::wstring projectRoot;
};

// Godot 方式のプロジェクト選択画面。EngineLoop を使わず
// Win32Window + GraphicsDevice + SwapChain + ImGuiRenderer だけの独立ミニループで回す
// (エンジン本体の初期化は assets ルート確定後に再起動したプロセスで行うため)。
//   testFrames > 0 : N フレーム後に自動終了 (CI 用)
//   shotPath 非空  : 終了直前のフレームを PNG 保存 (CI 用)
ProjectManagerOutcome RunProjectManager(int testFrames = 0, const std::wstring& shotPath = {});

// 自 exe を --project <dir> 付きで再起動する (ShellExecuteW)。true = 起動成功
bool RelaunchSelfWithProject(const std::wstring& projectRoot);

} // namespace mye
