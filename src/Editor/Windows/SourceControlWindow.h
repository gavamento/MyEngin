#pragma once
#include <functional>
#include <string>
#include <vector>

#include "Editor/SourceControl/SourceControlState.h"

namespace mye {

// 窓が EditorApp に頼むこと / EditorApp が窓に渡すこと (M66c)。
//
// ★窓に EngineContext を渡さない。渡すと「ソース管理の窓がシーンを開き直す」
//   ような越境が書けてしまう。頼めるのは**保存 1 個**だけに絞る
struct SourceControlHost {
    // 今開いている文書 (シーン or ミニシーン編集中のアセット) に未保存の変更があるか
    bool sceneDirty = false;
    // 保存を実行し、保存した文書の**絶対パス**を返す。
    // 失敗 (まだ dirty) / 何もしなかったときは空文字列
    std::function<std::wstring()> saveDocument;
};

// Source Control 窓 (M66b = 読み取り、M66c = stage / unstage / commit / History / diff)。
//
// ★状態 (SourceControlSession) は EditorApp が持つ。窓が閉じていても status は
//   最新に保たれる (M66i の Content Browser バッジが窓の開閉に依存しないため)。
class SourceControlWindow {
public:
    // 既定は非表示。**プロジェクト起動のときだけ** EditorApp が true にする
    // (spec §4.3: 裸起動では Source control が成立しないので出さない)
    bool open = false;

    void OnImGui(SourceControlSession& scm, const SourceControlHost& host);

    // 「このパスを正にする」が押されたか (EditorApp がトーストを出すために消費する)。
    // ★窓がトーストを直接出さないのは、ToastCenter を EditorApp が持っているため
    //   (窓へ参照を配ると、全窓が通知を出せる = 誰が出したか追えなくなる)
    bool TakeAdoptCanonicalRoot();

private:
    void DrawHeader(SourceControlSession& scm);
    void DrawChanges(SourceControlSession& scm, const SourceControlHost& host);
    void DrawDiffPane(SourceControlSession& scm, float height);
    // compact = 窓が低いとき (ラベルを省いて入力欄を 2 行にする)
    void DrawCommitBox(SourceControlSession& scm, const SourceControlHost& host, bool compact);
    void DrawHistory(SourceControlSession& scm);
    // ツリーを 1 ノード分描く (再帰)
    void DrawNode(const SourceControlModel& model, int index);
    // 選択中のパスに対応する行を集める (消えた行は黙って落とす)
    std::vector<PairedEntry> SelectedRows(const SourceControlModel& model) const;
    // 選択が変わったら差分を取り直す
    void SyncDiffRequest(SourceControlSession& scm);

    std::vector<std::string> selected_; // 選択中の本体パス (path 昇順を保つ)
    // コミット本文。**固定バッファ**にしているのは、この版の ImGui に
    // std::string 版の InputText (misc/cpp/imgui_stdlib) を組み込んでいないため
    // (external/imgui/misc/cpp が無い)。1 KB は subject + 本文数行に十分
    char commitMessage_[1024] = {};
    std::string diffRequestedPath_;     // 最後に差分を頼んだパス (二重要求を避ける)
    std::string selectedCommit_;        // History で選択中の SHA
    bool diffStaged_ = false;           // 差分をステージ済み側で見るか
    bool diffRequestedStaged_ = false;
    bool historyRequested_ = false;     // History タブを一度でも開いたか
    bool adoptRequested_ = false;
};

} // namespace mye
