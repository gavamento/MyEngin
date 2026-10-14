//====================================================================================
//                          OnlineLobbyWindow.h
//  MyEngin/ 秋田蓮音                                                     10/14/2026
//                                          Runtime のオンラインのロビー窓 (ImGui の小窓)
//====================================================================================
#pragma once

namespace mye {

class OnlineSession;

// 窓だけが持つ入力途中の状態 (参加コードの入力欄)
struct OnlineLobbyWindowState {
    char joinCode[16] = {};
};

// ★最初の形として ImGui で出す (ゲーム用の見た目は uGUI の InputField (M75h) の後で置き換える)。
// ★ここは表示と操作の要求だけ。退出は OnlineSession::RequestLeave で EngineLoop へ頼む
//   (自分のクライアントに Bye を送らせてから中継を畳む順序を EngineLoop が守る)
void DrawOnlineLobbyWindow(OnlineSession& session, OnlineLobbyWindowState& state);

} // namespace mye
