#pragma once
#include <cstdint>
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

    CollabClient& Client() { return client_; }

private:
    void SendHello();
    void SendRepoCheck();
    void ApplyStatusResult(const nlohmann::json& result);
    void ApplyError(const nlohmann::json& msg);
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
};

} // namespace mye
