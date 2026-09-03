#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "Editor/SourceControl/CollabClient.h"

namespace mye {

// Source Control の表示モデルとセッション制御 (M66b)。
//
// ここには **ImGui も Windows.h も入れない**。status 応答 (JSON) からモデルを組む
// 部分は純関数 (BuildModel) にしてあり、SourceControlSelfTest が偽のトランスクリプトを
// 流し込んで検査する — 窓を開かないと確かめられない形にすると、対の束ねや
// フォルダ集約のような「間違えても画面上は自然に見える」ロジックが永久に未検査になる。

// 1 ファイルの変更状態。**値の大小がそのまま「重さ」**で、合成は max を取る
// (spec §4.1: 競合 > D > R > A > M > ?)。並び順を変えると合成の意味が変わる
enum class ChangeState : uint8_t {
    None = 0,
    Untracked, // ? (未追跡)
    Modified,  // M (内容変更) / T (型変更)
    Added,     // A (追加) / C (コピー)
    Renamed,   // R (リネーム)
    Deleted,   // D (削除)
    Conflict,  // u (未マージ)
};

// 重い方を返す
ChangeState CombineState(ChangeState a, ChangeState b);
// porcelain v2 の XY 1 文字 → 状態
ChangeState StateFromStatusChar(char c);
// 一覧に出すバッジ文字 ("M" "A" "D" "R" "?" "!" のいずれか)。None は空文字列
const char* ChangeStateBadge(ChangeState s);

// サイドカーのパスから**本体**のパスを求める。本体自身はそのまま返る。
//   "x.png.meta"          -> "x.png"
//   "x.terrain.edit"      -> "x.terrain.json"   ★.json を .edit に差し替えた形
//                                                (TerrainEdit.h の EditPathFor が正本。
//                                                 ".terrain.edit" を削るだけだと "x" になり
//                                                 本体と束ならない)
//   "x.terrain.edit.meta" -> "x.terrain.json"   (何段でも剥がす)
std::string PrimaryPathFor(const std::string& path);

// 一覧の 1 行。本体 + サイドカーを束ねたもの。
// ★束ねる理由: `.meta` は本体を保存すると必ず一緒に動く。別行で出すと変更件数が
//   常に倍に見え、「1 個だけ stage する」がほぼ不可能になる (M66c)
struct PairedEntry {
    std::string path;                  // 本体の toplevel 相対パス ('/' 区切り)
    std::string oldPath;               // R のときの旧パス (本体のもの)
    std::vector<std::string> sidecars; // 実在するサイドカーだけ (.meta / .terrain.edit)
    ChangeState state = ChangeState::None;      // 合成後 = 行のバッジ
    ChangeState indexState = ChangeState::None; // 本体の index 側 (M66c の stage 表示)
    ChangeState worktreeState = ChangeState::None;
    bool conflict = false;
    // false = 本体には status の行が無い (サイドカーだけが変わった)。
    // この行を stage するときは sidecars だけを渡す
    bool primaryListed = false;
};

// フォルダ折り畳み用のツリー。**平坦な配列 + 添字**で持つ
// (ポインタで持つと push_back の再確保で全部ぶら下がりになる)
struct ScmNode {
    std::string name; // 表示名 (パスの最終要素)。ルートは空
    std::string path; // toplevel 相対のフルパス
    bool folder = false;
    ChangeState state = ChangeState::None;
    int entry = -1; // folder=false のとき entries への添字
    std::vector<int> children;
};

// History タブの 1 行 (M66c)。`log` 応答の 1 コミット。
// ★整形はしない (短縮 SHA も日時の表示形も窓の仕事)。サービスは git が出した
//   ままの値を返し、C++ が表示用に削る — 逆にするとサービス側の都合で
//   「7 桁だと思っていたら 8 桁だった」のような表示専用のバグが増える
struct CommitInfo {
    std::string sha;     // 40 桁
    std::string author;
    std::string date;    // 厳密 ISO-8601 (%aI)
    std::string subject; // 本文の 1 行目だけ
};

// 差分の子窓が読む内容 (M66c)。シーン JSON の意味付けはしない (v1.5)
struct DiffView {
    std::string path;
    bool staged = false;
    std::string text;
    bool truncated = false; // サービス側で上限まで切られた
    bool loading = false;   // 応答待ち
    bool valid = false;     // 一度でも応答を受けたか
};

struct SourceControlModel {
    std::string branch;   // "main" / "(detached)"
    std::string upstream; // "origin/main"。空 = 追跡なし
    std::string head;     // HEAD の SHA。空 = 未出生ブランチ
    int ahead = 0;
    int behind = 0;
    std::vector<PairedEntry> entries; // path 昇順 (決定的)
    std::vector<ScmNode> nodes;       // nodes[0] = ルート
    bool valid = false;               // status の応答を 1 度でも受けたか

    int ChangedCount() const { return static_cast<int>(entries.size()); }
    bool HasConflict() const;
    // 本体パスで行を引く (窓の選択 -> 行。見つからなければ nullptr)
    const PairedEntry* FindEntry(const std::string& path) const;
};

// status 応答の `result` → モデル。**純関数** (副作用なし・入力だけで決まる)
SourceControlModel BuildModel(const nlohmann::json& statusResult);

// repo_check の `result` とプロジェクトルートから利用可否を決める。**純関数**。
//   isRepo=false                      -> NotRepo
//   toplevel != projectRoot (正規化後) -> ToplevelMismatch
//   それ以外                           -> None
// ★ここを純関数にしているのは、DLL 無しでもセルフテストから叩けるようにするため。
//   toplevel の一致判定を間違えると **別のディレクトリのファイルを stage する**
//   (M66c 以降) ので、画面を見て気付ける類の誤りではない
Unavailable EvaluateRepoCheck(const nlohmann::json& repoCheckResult,
                              const std::wstring& projectRoot);

// エディタ 1 プロセス分の Source Control セッション。
// 「窓」ではなく「状態」— 窓は閉じていても status は最新に保たれる
// (M66i の Content Browser バッジが窓の開閉に依存しないため)
class SourceControlSession {
public:
    SourceControlSession() = default;
    // ★コピーも move も禁止。CollabClient へ渡すイベントハンドラが this を捕まえており、
    //   複製すると古い this を指したままのラムダが残る (= 破棄後に呼ばれて落ちる)。
    //   CollabClient 自身も DLL ハンドルを持つので二重解放になる
    SourceControlSession(const SourceControlSession&) = delete;
    SourceControlSession& operator=(const SourceControlSession&) = delete;

    // 起動時に 1 回。projectRoot が空 (裸起動) なら NoProject のまま何もしない
    void Start(const std::wstring& exeDir, const std::wstring& projectRoot);
    // 毎フレーム 1 回 (OnImGui の先頭)。応答/通知の配布とタイムアウトの回収
    void Poll();
    void Shutdown();

    // 窓の「更新」ボタン / 起動直後。飛んでいる status があれば二重には出さない
    void RequestStatus();
    // 保存直後の即時取り直し (M66i)。paths は toplevel 相対
    void HintChanged(const std::vector<std::string>& paths);

    Unavailable State() const;
    bool Ready() const { return State() == Unavailable::None; }
    const SourceControlModel& Model() const { return model_; }
    // 直近の失敗 (error.code と detail)。空 = 失敗していない
    const std::string& ErrorCode() const { return errorCode_; }
    const std::string& ErrorDetail() const { return errorDetail_; }
    bool Busy() const { return client_.PendingCount() > 0; }
    bool MergeInProgress() const { return mergeInProgress_; }
    bool RebaseInProgress() const { return rebaseInProgress_; }
    const std::string& Toplevel() const { return toplevel_; }

    // ---- canonicalRoot (spec §4.2) ----
    // マニフェストに記録された「正のパス」と今開いているパスが食い違うか。
    // 食い違い自体は害ではない (クローン先が人によって違うのは普通) が、
    // **チームで共有する project.mye.json に個人のパスが載っている**ことは知らせる
    bool CanonicalRootMismatch() const { return canonicalMismatch_; }
    const std::string& CanonicalRoot() const { return canonicalRoot_; }
    bool AdoptCanonicalRoot(); // 今のパスをマニフェストへ書き戻す

    // 外部での HEAD 移動を 1 回だけ取り出す (EditorApp がトーストにする)
    bool TakeHeadMoved();

    // ---- M66c: stage / unstage / commit / log / diff / identity ----
    // 選択された行を対の規則で束ねてから stage する。
    // `.meta` が欠けている資産はここで EnsureMeta してから git へ渡す
    void StageRows(const std::vector<PairedEntry>& rows);
    // index を戻すだけ。**ファイルは 1 個も作らない**
    void UnstageRows(const std::vector<PairedEntry>& rows);
    // 「保存してコミット」用: 今保存した文書 (絶対パス) を対の規則で stage する。
    // リポジトリの外なら何もしない
    void StageSavedPath(const std::wstring& absPath);
    // 本文は空でないこと (空はサービスが bad_request で弾くが、窓側でも止める)
    void Commit(const std::string& message);
    void RequestLog(int n);
    void RequestDiff(const std::string& path, bool staged);
    void RequestIdentity();

    const std::vector<CommitInfo>& History() const { return history_; }
    bool HistoryValid() const { return historyValid_; }
    const DiffView& Diff() const { return diff_; }
    // identity_check の結果。未応答なら Checked() が false (案内は出さない —
    // 「まだ聞いていない」と「未設定」を混ぜると起動直後に必ず警告が出る)
    bool IdentityChecked() const { return identityChecked_; }
    bool IdentityOk() const { return identityOk_; }
    // 直近に成功した commit の SHA を 1 回だけ取り出す (EditorApp がトーストにする)
    std::string TakeLastCommit();

    // ---- M66d: revert ----
    // paths を「最後に stage / commit した状態」へ戻す (未追跡は削除)。
    // ★**GitTransaction 経由でのみ呼ぶこと**。ゲート (未保存 / 再生中 / ビルド中) と
    //   ReloadHub の一括適用が掛かっていない状態で working tree を書き換えると、
    //   エディタが掴んだままのファイルが消える / 中間状態でホットリロードが走る。
    //   done(ok, code, detail) は応答が届いたときに 1 回だけ呼ばれる
    using WriteDoneFn =
        std::function<void(bool ok, const std::string& code, const std::string& detail)>;
    void Revert(const std::vector<std::string>& paths, WriteDoneFn done);

    // 書き込み系 (stage / unstage / commit) が飛んでいる間は true。
    // ★読み取り系 (status の自動更新) では立たない — 立てると監視が動くたびに
    //   ボタンが押せなくなる
    bool WriteInFlight() const { return client_.OpInFlight(); }

    CollabClient& Client() { return client_; }

private:
    void SendHello();
    void SendRepoCheck();
    void ApplyStatusResult(const nlohmann::json& result);
    void ApplyError(const nlohmann::json& msg);
    // 書き込み系の応答 (`{"status": {...}}`) を status 経路へ流す共通処理。
    // 成功なら true
    bool ApplyWriteResult(const nlohmann::json& msg);
    // toplevel 相対 -> 絶対パス (EnsureMeta / 実在判定に使う)
    std::wstring AbsolutePathOf(const std::string& rel) const;
    // error.code → Unavailable。分類できないものは None (= 窓にエラー文だけ出す)
    static Unavailable UnavailableFromCode(const std::string& code);

    CollabClient client_;
    SourceControlModel model_;
    std::wstring projectRoot_;
    std::string toplevel_;
    std::string canonicalRoot_;
    std::string errorCode_;
    std::string errorDetail_;
    std::string gitVersion_;
    Unavailable repoState_ = Unavailable::NoProject;
    bool started_ = false;
    bool statusInFlight_ = false;
    bool mergeInProgress_ = false;
    bool rebaseInProgress_ = false;
    bool canonicalMismatch_ = false;
    bool headMoved_ = false;

    // ---- M66c ----
    std::vector<CommitInfo> history_;
    DiffView diff_;
    std::string lastCommit_;   // TakeLastCommit で 1 回だけ取り出す
    std::string identityName_;
    std::string identityEmail_;
    bool historyValid_ = false;
    bool logInFlight_ = false;
    bool identityChecked_ = false;
    bool identityOk_ = false;
    int historyCount_ = 0; // 最後に要求した件数 (commit 後の取り直しで使う)
};

} // namespace mye
