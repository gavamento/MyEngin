//====================================================================================
//                          ScenePreviewFormat.h
//  MyEngine/ 秋田蓮音                                                      11/16/2026
//                                          Before / After の比較の画面に出す文面 (宣言)
//====================================================================================
#pragma once
#include <string>
#include <vector>

#include "Editor/ChangeTracking/ScenePreviewSession.h"

namespace mye {

// 比較の窓が出す内容。窓の描画と試験が同じ関数から読む (ImGui を回さずに、画面に出る文面を確かめる)
struct ScenePreviewScreen {
    std::string target;     // 対象のパスと Revision
    std::string beforeLine; // 「Before: ...」(読み込み中・エンティティ数・出せない理由)
    std::string afterLine;
    bool busy = false;      // どちらかを取得・読み込み中
    std::vector<std::string> notes; // アセットは今の内容、変わったアセット、C# を読まなかった数、未保存は比べない、など
};

// 取得の失敗の理由の文面
const char* PreviewFailureText(PreviewFailure failure);
// 側ごとの 1 行 (ラベルは含めない)
std::string PreviewSideText(const ScenePreviewSession::Side& side);

ScenePreviewScreen BuildScenePreviewScreen(const ScenePreviewSession& session);

} // namespace mye
