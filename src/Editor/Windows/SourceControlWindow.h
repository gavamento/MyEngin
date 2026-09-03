#pragma once
#include <functional>
#include <string>
#include <vector>

#include "Editor/SourceControl/GitTransaction.h"
#include "Editor/SourceControl/SourceControlState.h"

namespace mye {

// 窓が EditorApp に頼むこと / EditorApp が窓に渡すこと (M66c / M66d)。
//
// ★窓に EngineContext を渡さない。渡すと「ソース管理の窓がシーンを開き直す」
//   ような越境が書けてしまう。頼めるのは**保存**と**破棄の要求**だけに絞る
//   (実際の破棄は GitTransaction が確認モーダルとゲートを通してから実行する)
struct SourceControlHost {
    // 今開いている文書 (シーン or ミニシーン編集中のアセット) に未保存の変更があるか
    bool sceneDirty = false;
    // 保存を実行し、保存した文書の**絶対パス**を返す。
    // 失敗 (まだ dirty) / 何もしなかったときは空文字列
    std::function<std::wstring()> saveDocument;
    // 書き込み系 git 操作を今実行できない理由 (M66d)。空 = 実行してよい。
    // ★窓はこれを**判定に使うだけ**で自分では作らない。ゲートの表を窓側にも
    //   書くと、2 箇所の条件が食い違って「押せるのに何も起きない」が生まれる
    std::vector<GateBlocker> writeBlockers;
    // 破棄 (revert) を要求する。paths は toplevel 相対、untracked = 削除される件数
    std::function<void(std::vector<std::string>, int)> requestRevert;
    // ブランチ切替を要求する (M66e)。実際の切替は GitTransaction が
    // 「事前判定 → 確認 → checkout → 後処理」の順で行う
    std::function<void(std::string)> requestCheckout;
    // ---- M66f ----
    // pull を要求する。checkout と同じくトランザクション経由 (ゲート + 段階 A/B/C)
    std::function<void()> requestPull;
    // 背景 fetch の設定 (EditorSettings の現在値)。窓は**表示と編集だけ**で、
    // 保存も hello の再送も EditorApp 側が行う
    bool autoFetch = true;
    int fetchIntervalMin = 5;
    std::function<void(bool, int)> applyFetchSettings;
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
    // 差分は**別の dockable 窓** (M66e)。行を選ぶと開き、閉じられる。
    // ★Source Control 窓の中の帯にすると、既定のドック幅では一覧と差分と
    //   コミット欄が同居できない (M66c で実測)。読み取り専用なので切り離してよい
    bool diffOpen = false;

    void OnImGui(SourceControlSession& scm, const SourceControlHost& host);

    // 「このパスを正にする」が押されたか (EditorApp がトーストを出すために消費する)。
    // ★窓がトーストを直接出さないのは、ToastCenter を EditorApp が持っているため
    //   (窓へ参照を配ると、全窓が通知を出せる = 誰が出したか追えなくなる)
    bool TakeAdoptCanonicalRoot();
    // 作成に成功したブランチ名を 1 回だけ取り出す (同上)
    std::string TakeCreatedBranch();
    // push が成功した先 ("origin/main")。1 回だけ取り出す (同上、M66f)
    std::string TakePushed();

private:
    void DrawHeader(SourceControlSession& scm, const SourceControlHost& host);
    void DrawChanges(SourceControlSession& scm, const SourceControlHost& host);
    // 上流との関係の帯 + fetch / pull / push の 3 ボタン (M66f)
    void DrawRemoteBar(SourceControlSession& scm, const SourceControlHost& host);
    void DrawBranches(SourceControlSession& scm, const SourceControlHost& host);
    void DrawDiffWindow(SourceControlSession& scm);
    // compact = 窓が低いとき (ラベルを省いて入力欄を 2 行にする)
    void DrawCommitBox(SourceControlSession& scm, const SourceControlHost& host, bool compact);
    void DrawHistory(SourceControlSession& scm);
    // ツリーを 1 ノード分描く (再帰)
    void DrawNode(const SourceControlModel& model, int index);
    // ゲートが閉じている理由をツールチップに出す (直前の項目に対して)
    static void DrawBlockerTooltip(const std::vector<GateBlocker>& blockers);
    // 行の集合 -> revert に渡すパスと「削除される件数」
    static void CollectRevertTargets(const std::vector<PairedEntry>& rows,
                                     std::vector<std::string>& paths, int& untracked);
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
    // ---- M66e: Branches ----
    std::string selectedBranch_;   // 一覧で選択中の短縮名
    std::string createdBranch_;    // 作成に成功した名前 (EditorApp が取り出す)
    char newBranchName_[128] = {}; // 作成モーダルの入力欄 (InputText は固定バッファ)
    bool diffFocusRequest_ = false;  // 行を選んだ = 差分の窓を手前に出す (M66e)
    bool branchesRequested_ = false; // Branches タブを一度でも開いたか
    bool newBranchOpen_ = false;     // 作成モーダルを出しているか
    // ---- M66f ----
    bool remoteRequested_ = false;   // remote_state を一度でも聞いたか
    bool remoteExpanded_ = false;    // 帯を展開してコミット一覧を出しているか
    std::string pushedTo_;           // push に成功した先 (EditorApp が取り出す)
};

} // namespace mye
