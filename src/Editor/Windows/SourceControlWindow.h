#pragma once
#include <string>
#include <vector>

#include "Editor/SourceControl/SourceControlState.h"

namespace mye {

// Source Control 窓 (M66b): 変更一覧を**読むだけ**。
//
// ★M66b の時点では stage も commit も無い。ここで意図的に「読み取り専用の窓」を
//   先に通しているのは、status の対の束ね・フォルダ集約・利用不可の理由表示という
//   「間違えても画面は自然に見える」層を、書き込みが入る前に固めたいため。
// ★状態 (SourceControlSession) は EditorApp が持つ。窓が閉じていても status は
//   最新に保たれる (M66i の Content Browser バッジが窓の開閉に依存しないため)。
class SourceControlWindow {
public:
    bool open = false; // 既定は非表示 (プロジェクトの大半は git 管理下にない) (プロジェクトの大半は git 管理下にない)

    void OnImGui(SourceControlSession& scm);

    // 「このパスを正にする」が押されたか (EditorApp がトーストを出すために消費する)。
    // ★窓がトーストを直接出さないのは、ToastCenter を EditorApp が持っているため
    //   (窓へ参照を配ると、全窓が通知を出せる = 誰が出したか追えなくなる)
    bool TakeAdoptCanonicalRoot();

private:
    void DrawHeader(SourceControlSession& scm);
    void DrawChanges(const SourceControlModel& model);
    // ツリーを 1 ノード分描く (再帰)
    void DrawNode(const SourceControlModel& model, int index);

    std::vector<std::string> selected_; // 選択中の本体パス (path 昇順を保つ)
    bool adoptRequested_ = false;
};

} // namespace mye
