#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "Editor/SourceControl/CollabClient.h"
#include "Editor/SourceControl/StageClassifier.h" // StageChange (checkout の応答をそのまま段階分類へ)

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

// バッジ引きのキー: toplevel 相対 '/' 区切りを小文字化したもの (M66i)。
// ★ディスク側は `NormalizePathKey` (絶対化 + 小文字 + '\\') でキー化するので、
//   **同じ towlower を通す**。バイト単位の ASCII 小文字化にすると非 ASCII の
//   綴り違いでディスク側とだけキーがずれ、バッジが黙って消える。
//   Windows のパスは大小を区別しない = git が記録した綴りとディスクの綴りが
//   違うだけでバッジが出ない、という形の不具合を先に潰しておく
std::string ScmPathKey(const std::string& rel);

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

// Branches タブの 1 行 (M66e)。`branches` 応答の 1 件
struct BranchInfo {
    std::string name;     // 短縮名 ("main" / "origin/main")。checkout にそのまま渡す
    std::string oid;      // 先端のコミット (40 桁)
    std::string upstream; // 追跡先の短縮名。空 = 追跡なし
};

// `branches` 応答。**リモートは別の配列**にしておく (checkout の扱いが違う —
// リモート追跡だけの名前は `-t` で追跡ブランチを作ってから乗る)
struct BranchList {
    std::string current; // 今のブランチ。空 = detached HEAD
    std::vector<BranchInfo> locals;
    std::vector<BranchInfo> remotes;
    bool valid = false; // 一度でも応答を受けたか
};

// `branches` 応答の `result` → モデル。**純関数**
BranchList BuildBranchList(const nlohmann::json& branchesResult);

// upstream との関係 (M66f)。`remote_state` / `fetch` / `pull` / `push` の応答と
// `remote_changed` 通知が同じ形で運んでくる
struct RemoteState {
    std::string upstream;             // "origin/main"。空 = 追跡なし
    bool hasRemote = false;           // リモートが 1 つでも設定されているか
    int ahead = 0;                    // 手元にあって upstream に無い
    int behind = 0;                   // upstream にあって手元に無い
    std::vector<CommitInfo> commits;  // HEAD..@{u} の最大 20 件 (帯の展開に出す)
    bool valid = false;               // 一度でも応答/通知を受けたか
};

// `remote_state` 相当の JSON → モデル。**純関数**
RemoteState BuildRemoteState(const nlohmann::json& remoteResult);

// 競合した 1 ファイル (M66g)。`conflicts` 応答の 1 件。
// ★`kind` はサービスの綴りをそのまま持つ ("both_modified" / "deleted_by_them" …)。
//   C++ 側で列挙型に落とさないのは、git が組を増やしても**表示だけ**が
//   「不明」になって済むようにするため (分岐に使うのは ours / theirs の 2 つだけ)
struct ConflictFile {
    std::string path;
    std::string kind;
    bool ours = false;   // 自分側の版が index にある = ours を採れる
    bool theirs = false; // 相手側の版が index にある = theirs を採れる
};

// 競合一覧の 1 行。**対の規則で束ねたもの** (M66g)。
// ★本体が競合していなくてもサイドカー (`.meta`) だけが競合することがある。
//   その場合も行の見出しは本体で、resolve に渡すのは「実際に競合しているパスだけ」
struct ConflictRow {
    std::string path;               // 本体のパス (表示とキー)
    std::vector<std::string> paths; // resolve に渡す = 競合しているパスだけ (昇順)
    std::string kind;               // 代表の種別 (本体が競合していればそれ)
    bool ours = true;               // 束の全員が ours を採れるか
    bool theirs = true;             // 束の全員が theirs を採れるか
    bool primaryConflicted = false; // 本体そのものが競合しているか
};

// `conflicts` 応答。
struct ConflictList {
    std::vector<ConflictFile> files;
    // 競合せずにマージ済みのファイル (段階分類の入力にそのまま使う)
    std::vector<StageChange> merged;
    bool mergeInProgress = false;
    bool rebaseInProgress = false;
    bool valid = false; // 一度でも応答を受けたか
};

// `conflicts` 応答の `result` → モデル。**純関数**
ConflictList BuildConflictList(const nlohmann::json& conflictsResult);
// 競合ファイル → 対で束ねた一覧の行。**純関数** (path 昇順)
std::vector<ConflictRow> BuildConflictRows(const std::vector<ConflictFile>& files);
// toplevel 相対パスが競合一覧に含まれるか (**サイドカーだけの競合も本体に効く**)。
// **純関数** — 保存を止めるかの判定なので、窓を開かずに検査できる形にしておく
bool ConflictMatchesPath(const std::vector<ConflictFile>& files, const std::string& rel);

// `checkout` / `diff_names` の `names` 配列 → 段階分類の入力。**純関数**。
// ★git の name-status 1 文字と BatchChange::Kind の対応をここ 1 箇所に閉じる。
//   2 箇所に書くと「D を Modified と読んで消えたファイルを読み直そうとする」形で
//   静かに壊れる (ReloadHub のリトライ列に永久に残る)
std::vector<StageChange> ChangesFromNames(const nlohmann::json& names);

struct SourceControlModel {
    std::string branch;   // "main" / "(detached)"
    std::string upstream; // "origin/main"。空 = 追跡なし
    std::string head;     // HEAD の SHA。空 = 未出生ブランチ
    int ahead = 0;
    int behind = 0;
    std::vector<PairedEntry> entries; // path 昇順 (決定的)
    std::vector<ScmNode> nodes;       // nodes[0] = ルート
    bool valid = false;               // status の応答を 1 度でも受けたか

    // ---- M66i: Content Browser のバッジ引き ----
    // ★これが「集約のキャッシュ」そのもの。BuildModel が entries / nodes と同時に
    //   組むので、**status を受け直した瞬間に必ず作り直される** = 無効化を別に
    //   書く必要がない (無効化を手で書くと、必ずどこかの経路で忘れて古い
    //   バッジが残る)。キーは ScmPathKey を通したもの
    std::map<std::string, ChangeState> fileBadges;   // 対で束ねた本体 1 行につき 1 件
    std::map<std::string, ChangeState> folderBadges; // フォルダ (PropagateFolderState の結果)

    int ChangedCount() const { return static_cast<int>(entries.size()); }
    bool HasConflict() const;
    // 本体パスで行を引く (窓の選択 -> 行。見つからなければ nullptr)
    const PairedEntry* FindEntry(const std::string& path) const;

    // ---- M66i: バッジ (relKey は ScmPathKey を通したキー) ----
    // ★**サイドカーは本体へ寄せて引く** (PrimaryPathFor)。`x.png.meta` だけが
    //   変わっていても `x.png` のタイルに出る = 一覧 (対で束ねた行) と同じ見え方。
    //   None = 変更なし (「引けなかった」と区別しない — 変更が無いことと
    //   一覧に居ないことは同じ意味なので、optional にしても呼び手が困るだけ)
    ChangeState StateFor(const std::string& relKey) const;
    ChangeState FolderStateFor(const std::string& relDirKey) const;
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

    // 起動時に 1 回。projectRoot が空 (裸起動) なら NoProject のまま何もしない。
    // autoFetch / fetchIntervalMin は EditorSettings 由来で、そのまま hello に載る
    void Start(const std::wstring& exeDir, const std::wstring& projectRoot, bool autoFetch,
               int fetchIntervalMin);
    // 毎フレーム 1 回 (OnImGui の先頭)。応答/通知の配布とタイムアウトの回収
    void Poll();
    void Shutdown();

    // 窓の「更新」ボタン / 起動直後。飛んでいる status があれば二重には出さない
    void RequestStatus();
    // 保存直後の即時取り直し (M66i)。paths は toplevel 相対。**その場で 1 往復投げる**
    void HintChanged(const std::vector<std::string>& paths);

    // ---- M66i: 保存ヒント (絶対パス) と Content Browser のバッジ ----
    // 「今このファイルを保存した / 作った / 消した」を伝える。監視 (notify + 300 ms
    // デバウンス) でも同じ status に行き着くので、これは**速く反映するためだけ**の口。
    // リポジトリの外・利用不可なら黙って捨てる (呼び出し側に分岐を書かせない)。
    // ★送信は Poll() まで遅延し、フレーム内の連打を 1 往復にまとめる。飛んでいる
    //   ヒントがある間は貯めておいて相乗りさせる — 一括インポート (数百ファイル) が
    //   そのまま数百回の git status になると、その間エディタが固まる
    void HintSaved(const std::wstring& absPath);
    // 絶対パス → バッジ。無変更 / リポジトリ外 / 利用不可はすべて None。
    // **毎フレーム・タイル 1 枚ごとに呼ばれる**ので、実ファイルには触らない
    ChangeState BadgeForFile(const std::wstring& absPath) const;
    ChangeState BadgeForFolder(const std::wstring& absDir) const;

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

    // 書き込み系 op の応答 1 件。done(ok, code, detail) は応答が届いたときに 1 回だけ
    // 呼ばれる。**送れなかったとき (サービス停止など) も必ず 1 回呼ぶ** — 呼ばないと
    // 呼び手 (GitTransaction は一括モード、窓は入力欄) が待ったまま帰ってこない
    using WriteDoneFn =
        std::function<void(bool ok, const std::string& code, const std::string& detail)>;

    // ---- M66c: stage / unstage / commit / log / diff / identity ----
    // 選択された行を対の規則で束ねてから stage する。
    // `.meta` が欠けている資産はここで EnsureMeta してから git へ渡す
    void StageRows(const std::vector<PairedEntry>& rows);
    // index を戻すだけ。**ファイルは 1 個も作らない**
    void UnstageRows(const std::vector<PairedEntry>& rows);
    // 「保存してコミット」用: 今保存した文書 (絶対パス) を対の規則で stage する。
    // リポジトリの外なら何もしない
    void StageSavedPath(const std::wstring& absPath);
    // 本文は空でないこと (空はサービスが bad_request で弾くが、窓側でも止める)。
    // ★done は省略可。**成功したときだけ入力欄を空にする**ために窓が渡す —
    //   commit は nothing_to_commit / identity_missing / hook 失敗で普通に失敗するので、
    //   投げた直後に消すとユーザーが書いた本文がどこにも残らない (review-1 #4)
    void Commit(const std::string& message, WriteDoneFn done = {});
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
    //   done(ok, code, detail) は応答が届いたときに 1 回だけ呼ばれる (上の WriteDoneFn)
    void Revert(const std::vector<std::string>& paths, WriteDoneFn done);

    // ---- M66e: branches / branch_create / checkout / diff_names ----
    void RequestBranches();
    const BranchList& Branches() const { return branches_; }
    // ブランチ作成 (working tree は動かない = トランザクション不要)。
    // from が空なら現在の HEAD から
    void CreateBranch(const std::string& name, const std::string& from, WriteDoneFn done);

    // 段階の**事前**判定に使う。from..to で working tree に降ってくるものを聞く
    using DiffNamesDoneFn = std::function<void(bool ok, const std::vector<StageChange>& changes,
                                               const std::string& code, const std::string& detail)>;
    void RequestDiffNames(const std::string& from, const std::string& to, DiffNamesDoneFn done);

    // working tree を入れ替える op (checkout / pull) の応答。
    // **changes が段階分類の唯一の入力**。
    // ★checkout と pull で 1 つの型を共有しているのは、`GitTransaction::ApplyResult`
    //   以降 (段階分類 → 登録 → EndBatch → 段階 B/C) が op に依存しないため。
    //   型を分けると「同じ後処理を 2 通り書く」ことになり、片方だけ直す事故が必ず出る
    struct TreeOpResult {
        bool ok = false;
        std::string branch;               // 切り替わった先 (checkout の成功時)
        std::vector<StageChange> changes; // working tree で入れ替わったもの
        std::string errorCode;
        std::string errorDetail;
        // local_changes_overwritten のとき「何を破棄すれば進めるか」(spec S7)
        std::vector<std::string> errorPaths;
    };
    using CheckoutDoneFn = std::function<void(const TreeOpResult&)>;
    // ★**GitTransaction 経由でのみ呼ぶこと** (Revert と同じ理由 — ゲートと
    //   ReloadHub の一括適用が掛かっていない状態で working tree を入れ替えると、
    //   エディタが掴んだままのファイルが下から差し替わる)
    void Checkout(const std::string& name, CheckoutDoneFn done);

    // ---- M66f: fetch / pull / push / remote_state ----
    // fetch は working tree を触らないのでゲートを通さない (塞ぐのは WriteInFlight だけ)。
    // 応答の remote / status はそのままモデルへ入る
    void RequestFetch();
    // 帯 (「upstream に N 件」) の取り直し。fetch はしない = ネットワークに出ない
    void RequestRemoteState();
    const RemoteState& Remote() const { return remote_; }
    // ★**GitTransaction 経由でのみ呼ぶこと** (Checkout と同じ理由)。
    //   allowMerge=false は `--ff-only`、true は `--no-rebase`
    void Pull(bool allowMerge, CheckoutDoneFn done);
    // push は working tree を触らないのでゲート不要。upstream が無ければ
    // サービス側が `-u origin <branch>` を張る
    void Push(bool setUpstream, WriteDoneFn done);
    // 背景 fetch が「何か来ている」と言ってきたら 1 回だけ true
    // (EditorApp がトーストにする。窓は Remote() を毎フレーム読む)
    bool TakeRemoteChanged();
    // 背景 fetch の失敗を 1 回だけ取り出す。戻り値 = 取り出したか
    bool TakeFetchError(std::string& code, std::string& detail);
    // 歯車で設定を変えたとき。**hello を再送**してタイマーを組み直す (spec の M66f)
    void ApplyFetchSettings(bool autoFetch, int fetchIntervalMin);

    // 起動時の残骸検査 (決定 13)。マージ / リベースの途中で開いたことを 1 回だけ知らせる
    bool TakeMergeWarning();

    // ---- M66g: 競合 (conflicts / resolve / merge_abort / continue) ----
    // 競合一覧の取り直し。**status がマージ中を告げるたびに自動で 1 回**投げるので、
    // 窓から明示的に呼ぶ必要は無い (done は GitTransaction が競合直後に使う)
    using ConflictsDoneFn = std::function<void(bool ok, const ConflictList& list)>;
    void RequestConflicts(ConflictsDoneFn done);
    const ConflictList& Conflicts() const { return conflicts_; }
    // 競合の解決 (`ours` / `theirs`)。paths は対で渡す — 本体だけ解決して `.meta` を
    // 競合のまま残すと、次の continue が「まだ未解決です」で止まる
    void Resolve(const std::vector<std::string>& paths, const char* side, WriteDoneFn done);
    // ★**GitTransaction 経由でのみ呼ぶこと** (Pull / Checkout と同じ理由 —
    //   working tree が丸ごと入れ替わる)。応答は同じ TreeOpResult
    void MergeAbort(CheckoutDoneFn done);
    void MergeContinue(CheckoutDoneFn done);
    // 絶対パスが競合一覧にあるか (保存を止める判定。spec §7「競合中のアクティブシーン」)
    bool IsConflictedPath(const std::wstring& absPath) const;
    // 競合の解決で入れ替わりうるファイルの集合 = マージ済み + 競合中。
    // abort / continue の**段階の事前予測**に使う (実行後は応答の names で分類し直す)
    std::vector<StageChange> ConflictChangeSet() const;

    // 書き込み系 (stage / unstage / commit) が飛んでいる間は true。
    // ★読み取り系 (status の自動更新) では立たない — 立てると監視が動くたびに
    //   ボタンが押せなくなる
    bool WriteInFlight() const { return client_.OpInFlight(); }

    CollabClient& Client() { return client_; }

private:
    void SendHello();
    void SendRepoCheck();
    // 通知 (status_changed / remote_changed / repo_changed) の受け口。
    // ★Start が DLL のロードより先に CollabClient へ登録する = DLL が無くても
    //   配線は生きている (セルフテストが偽の通知行を流して検査できる)
    void ApplyEvent(const nlohmann::json& msg);
    void ApplyStatusResult(const nlohmann::json& result);
    void ApplyError(const nlohmann::json& msg);
    // 書き込み系の応答 (`{"status": {...}}`) を status 経路へ流す共通処理。
    // 成功なら true
    bool ApplyWriteResult(const nlohmann::json& msg);
    // working tree を入れ替える op (checkout / pull / merge_abort / continue) の共通経路。
    // ★4 本とも応答が同じ型 (`{head, names, status, remote}`) なので、受け取り方も
    //   1 本に寄せる — 分けて書くと「片方だけ names を読み落とす」形で静かに壊れる
    using TreeOpExtraFn = std::function<void(const nlohmann::json& result, TreeOpResult& r)>;
    void SendTreeOp(const char* op, const nlohmann::json& args, CheckoutDoneFn done,
                    TreeOpExtraFn extra);
    // toplevel 相対 -> 絶対パス (EnsureMeta / 実在判定に使う)
    std::wstring AbsolutePathOf(const std::string& rel) const;
    // 絶対パス -> toplevel 相対 '/' 区切り。リポジトリの外なら空
    std::string RelativePathOf(const std::wstring& absPath) const;
    // 絶対パス -> バッジ引きのキー (小文字 '/' 区切り)。リポジトリの外なら空。
    // ★RelativePathOf (std::filesystem::relative) は weakly_canonical 経由で
    //   **実ファイルを開く** ので、毎フレーム何十回も呼べない。バッジ用の
    //   こちらは NormalizePathKey の文字列比較だけで済ませる
    std::string RelativeKeyOf(const std::wstring& absPath) const;
    // 貯めた保存ヒントを 1 往復にまとめて送る (Poll から)
    void FlushHints();
    // error.code → Unavailable。分類できないものは None (= 窓にエラー文だけ出す)
    static Unavailable UnavailableFromCode(const std::string& code);

    CollabClient client_;
    SourceControlModel model_;
    std::wstring projectRoot_;
    // NormalizePathKey(projectRoot_) を末尾の区切りを落として控えたもの (M66i)。
    // バッジ引きが毎フレーム作り直さずに済むよう Start で 1 回だけ作る
    std::wstring rootKey_;
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
    bool mergeWarned_ = false; // 起動時の残骸トーストを 1 回だけ出すための取り出し口 (M66e)

    // ---- M66i ----
    // 次の Poll でまとめて送る保存ヒント。**set** なのは、同じパスを何度保存しても
    // 1 件に畳み、かつ送る順を決定的にするため
    std::set<std::string> hintPending_;
    bool hintInFlight_ = false;

    // ---- M66e ----
    BranchList branches_;
    bool branchesInFlight_ = false;

    // ---- M66g ----
    ConflictList conflicts_;
    // 飛んでいる conflicts に相乗りした呼び手。応答が返ったら全員に配る
    std::vector<ConflictsDoneFn> conflictsWaiters_;
    bool conflictsInFlight_ = false;

    // ---- M66f ----
    RemoteState remote_;
    std::string fetchErrorCode_;   // 背景 fetch の失敗 (TakeFetchError で 1 回だけ)
    std::string fetchErrorDetail_;
    bool autoFetch_ = true;        // hello に載せる値 (EditorSettings の写し)
    int fetchIntervalMin_ = 5;
    bool remoteChanged_ = false;   // TakeRemoteChanged で 1 回だけ
    bool remoteStateInFlight_ = false;

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
