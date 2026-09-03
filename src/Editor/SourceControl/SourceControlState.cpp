#include "Editor/SourceControl/SourceControlState.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <utility>

#include "Editor/SourceControl/PairRule.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/AssetDatabase.h"
#include "Engine/Engine/Project.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {

namespace {

bool EndsWith(const std::string& s, const char* suffix)
{
    const size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// nlohmann の value() は型が違うと例外を投げるので、char は文字列から取り出す
char FirstChar(const nlohmann::json& j, const char* key, char fallback)
{
    if (!j.contains(key) || !j[key].is_string()) {
        return fallback;
    }
    const std::string s = j[key].get<std::string>();
    return s.empty() ? fallback : s[0];
}

// 子の状態から親フォルダの状態を作る。
// ★spec §4.1 は「子に M があれば M / ? だけなら ? / 混在は M」と書いているが、
//   実装は **CombineState (= 最も重いもの)** に一般化してある。3 つの例は
//   どれも満たしつつ、「削除された子を含むフォルダが M に見える」= 削除が
//   フォルダの折り畳みに隠れる、という致命的な誤表示を防ぐため
void PropagateFolderState(std::vector<ScmNode>& nodes, int index)
{
    ScmNode& node = nodes[static_cast<size_t>(index)];
    if (!node.folder) {
        return;
    }
    ChangeState combined = ChangeState::None;
    for (const int child : node.children) {
        PropagateFolderState(nodes, child);
        combined = CombineState(combined, nodes[static_cast<size_t>(child)].state);
    }
    nodes[static_cast<size_t>(index)].state = combined;
}

// 子の並び: フォルダが先、同種は path 昇順。**明示的な決定論キー**で並べる
// (unordered な構築順がそのまま画面に出ると、同じリポジトリを 2 台で開いたときに
//  一覧の順が違って見える)
void SortChildren(std::vector<ScmNode>& nodes, int index)
{
    std::vector<int>& children = nodes[static_cast<size_t>(index)].children;
    std::sort(children.begin(), children.end(), [&nodes](int a, int b) {
        const ScmNode& na = nodes[static_cast<size_t>(a)];
        const ScmNode& nb = nodes[static_cast<size_t>(b)];
        if (na.folder != nb.folder) {
            return na.folder;
        }
        return na.path < nb.path;
    });
    for (const int child : children) {
        SortChildren(nodes, child);
    }
}

} // namespace

ChangeState CombineState(ChangeState a, ChangeState b)
{
    return static_cast<uint8_t>(a) >= static_cast<uint8_t>(b) ? a : b;
}

ChangeState StateFromStatusChar(char c)
{
    switch (c) {
    case 'M':
    case 'T': // 型変更 (ファイル ⇄ シンボリックリンク)。UI では M と同じ扱いでよい
        return ChangeState::Modified;
    case 'A':
    case 'C': // コピー。git は追加と同じ意味で出す
        return ChangeState::Added;
    case 'R':
        return ChangeState::Renamed;
    case 'D':
        return ChangeState::Deleted;
    case '?':
        return ChangeState::Untracked;
    default: // '.' (変更なし) と未知の文字
        return ChangeState::None;
    }
}

const char* ChangeStateBadge(ChangeState s)
{
    switch (s) {
    case ChangeState::Untracked:
        return "?";
    case ChangeState::Modified:
        return "M";
    case ChangeState::Added:
        return "A";
    case ChangeState::Renamed:
        return "R";
    case ChangeState::Deleted:
        return "D";
    case ChangeState::Conflict:
        return "!";
    case ChangeState::None:
    default:
        return "";
    }
}

std::string PrimaryPathFor(const std::string& path)
{
    std::string p = path;
    // 何段でも剥がす (".terrain.edit.meta" のような二重サイドカーに備える)。
    // 上限を付けるのは、将来サフィックスを足したときに無限ループにしないため
    for (int guard = 0; guard < 4; ++guard) {
        if (EndsWith(p, ".meta")) {
            p.resize(p.size() - 5);
            continue;
        }
        if (EndsWith(p, ".terrain.edit")) {
            // ".edit" (5 文字) を ".json" に差し替える = "x.terrain.json"
            p.resize(p.size() - 5);
            p += ".json";
            continue;
        }
        break;
    }
    return p;
}

const PairedEntry* SourceControlModel::FindEntry(const std::string& path) const
{
    for (const PairedEntry& e : entries) {
        if (e.path == path) {
            return &e;
        }
    }
    return nullptr;
}

bool SourceControlModel::HasConflict() const
{
    for (const PairedEntry& e : entries) {
        if (e.conflict) {
            return true;
        }
    }
    return false;
}

SourceControlModel BuildModel(const nlohmann::json& statusResult)
{
    SourceControlModel m;
    if (!statusResult.is_object()) {
        return m;
    }
    m.valid = true;
    m.branch = statusResult.value("branch", std::string());
    m.upstream = statusResult.value("upstream", std::string());
    m.head = statusResult.value("head", std::string());
    m.ahead = statusResult.value("ahead", 0);
    m.behind = statusResult.value("behind", 0);

    // ---- 対の束ね ----
    // 本体パスをキーに 1 行へまとめる。std::map = キー昇順で走査できるので
    // ソートを別途やらずに決定的な並びが出る
    std::map<std::string, PairedEntry> rows;
    if (statusResult.contains("entries") && statusResult["entries"].is_array()) {
        for (const nlohmann::json& e : statusResult["entries"]) {
            if (!e.is_object()) {
                continue;
            }
            const std::string path = e.value("path", std::string());
            if (path.empty()) {
                continue;
            }
            const char idx = FirstChar(e, "index", '.');
            const char wt = FirstChar(e, "worktree", '.');
            // '!' = --ignored を付けたときだけ来る無視ファイル。「変更」ではないので
            // 一覧に出さない (status は --ignored を付けていないので普段は来ない)
            if (idx == '!' && wt == '!') {
                continue;
            }
            const bool conflict = e.value("conflict", false);
            const std::string primary = PrimaryPathFor(path);
            PairedEntry& row = rows[primary];
            if (row.path.empty()) {
                row.path = primary;
            }
            const ChangeState idxState = StateFromStatusChar(idx);
            const ChangeState wtState = StateFromStatusChar(wt);
            const ChangeState own =
                conflict ? ChangeState::Conflict : CombineState(idxState, wtState);
            row.state = CombineState(row.state, own);
            row.conflict = row.conflict || conflict;
            if (path == primary) {
                row.primaryListed = true;
                row.indexState = idxState;
                row.worktreeState = wtState;
                row.oldPath = e.value("oldPath", std::string());
            } else {
                row.sidecars.push_back(path);
            }
        }
    }
    m.entries.reserve(rows.size());
    for (auto& [key, row] : rows) {
        // サイドカーの並びも決定的に (map のキー順に足しているとは限らない)
        std::sort(row.sidecars.begin(), row.sidecars.end());
        m.entries.push_back(std::move(row));
    }

    // ---- フォルダツリー ----
    ScmNode root;
    // ★root.folder = true が要る。PropagateFolderState は folder でないノードで
    //   即 return するので、ここを既定の false のままにすると**集約が 1 段も走らない**
    //   (子は正しく並ぶのでツリーは自然に見え、バッジだけが全部消える)
    root.folder = true;
    m.nodes.push_back(std::move(root)); // [0] = ルート
    std::map<std::string, int> folderIndex;
    folderIndex[""] = 0;
    for (size_t i = 0; i < m.entries.size(); ++i) {
        const std::string& full = m.entries[i].path;
        int parent = 0;
        std::string accum;
        size_t start = 0;
        while (true) {
            const size_t slash = full.find('/', start);
            if (slash == std::string::npos) {
                break;
            }
            const std::string seg = full.substr(start, slash - start);
            accum = accum.empty() ? seg : accum + "/" + seg;
            auto it = folderIndex.find(accum);
            if (it == folderIndex.end()) {
                ScmNode folder;
                folder.name = seg;
                folder.path = accum;
                folder.folder = true;
                m.nodes.push_back(std::move(folder));
                const int idx = static_cast<int>(m.nodes.size()) - 1;
                m.nodes[static_cast<size_t>(parent)].children.push_back(idx);
                folderIndex[accum] = idx;
                parent = idx;
            } else {
                parent = it->second;
            }
            start = slash + 1;
        }
        ScmNode leaf;
        leaf.name = full.substr(start);
        leaf.path = full;
        leaf.folder = false;
        leaf.state = m.entries[i].state;
        leaf.entry = static_cast<int>(i);
        m.nodes.push_back(std::move(leaf));
        m.nodes[static_cast<size_t>(parent)].children.push_back(
            static_cast<int>(m.nodes.size()) - 1);
    }
    SortChildren(m.nodes, 0);
    PropagateFolderState(m.nodes, 0);
    return m;
}

Unavailable EvaluateRepoCheck(const nlohmann::json& repoCheckResult,
                              const std::wstring& projectRoot)
{
    if (!repoCheckResult.is_object() || !repoCheckResult.value("isRepo", false)) {
        // v1 は `git init` を提供しない (spec §3)。案内だけ出す
        return Unavailable::NotRepo;
    }
    const std::string toplevel = repoCheckResult.value("toplevel", std::string());
    if (toplevel.empty()) {
        return Unavailable::NotRepo;
    }
    // ★git は '/' 区切り・元の大小で返す。NormalizePathKey (絶対化 + 小文字 + '\\')
    //   を両側に通してから比べる
    if (NormalizePathKey(Utf8ToWide(toplevel)) != NormalizePathKey(projectRoot)) {
        return Unavailable::ToplevelMismatch;
    }
    return Unavailable::None;
}

// ---- SourceControlSession ----

void SourceControlSession::Start(const std::wstring& exeDir, const std::wstring& projectRoot)
{
    if (started_) {
        return;
    }
    started_ = true;
    projectRoot_ = projectRoot;
    if (projectRoot_.empty()) {
        // 裸起動。**DLL のロードすらしない** — プロジェクトが無ければ
        // 「どのリポジトリか」が決まらないので、機能自体が成立しない
        repoState_ = Unavailable::NoProject;
        return;
    }
    repoState_ = Unavailable::None;

    // canonicalRoot の照合 (spec §4.2)。DLL の有無とは無関係なのでここで済ませる
    ProjectManifest manifest;
    if (LoadProjectManifest(projectRoot_, manifest)) {
        canonicalRoot_ = manifest.canonicalRoot;
        if (!canonicalRoot_.empty()) {
            canonicalMismatch_ = NormalizePathKey(Utf8ToWide(canonicalRoot_))
                != NormalizePathKey(projectRoot_);
        }
    }

    if (!client_.Load(exeDir)) {
        return; // 理由は client_.State() (NoService / ProtoMismatch)
    }
    if (!client_.Create(WideToUtf8(projectRoot_))) {
        return;
    }
    client_.SetEventHandler([this](const nlohmann::json& msg) {
        const std::string ev = msg.value("event", std::string());
        if (ev == "status_changed" && msg.contains("status")) {
            ApplyStatusResult(msg["status"]);
        } else if (ev == "repo_changed") {
            // v1 は「トーストで知らせる」だけ (spec §3「後回し」)。状態そのものは
            // 同時に来る status_changed が持ってくる
            headMoved_ = true;
        }
    });
    SendHello();
}

void SourceControlSession::SendHello()
{
    client_.Request(collabop::kHello, { { "fetchIntervalMin", 5 }, { "autoFetch", false } },
                    [this](const nlohmann::json& msg) {
                        if (!msg.value("ok", false)) {
                            ApplyError(msg);
                            return;
                        }
                        gitVersion_ = msg["result"].value("gitVersion", std::string());
                        MYE_LOG_INFO("[collab] git %s", gitVersion_.c_str());
                        SendRepoCheck();
                    });
}

void SourceControlSession::SendRepoCheck()
{
    client_.Request(collabop::kRepoCheck, nlohmann::json::object(), [this](const nlohmann::json& msg) {
        if (!msg.value("ok", false)) {
            ApplyError(msg);
            return;
        }
        const nlohmann::json& r = msg["result"];
        toplevel_ = r.value("toplevel", std::string());
        mergeInProgress_ = r.value("mergeInProgress", false);
        rebaseInProgress_ = r.value("rebaseInProgress", false);
        // ★プロジェクトルートがリポジトリのトップでないと、パスの相対化 (toplevel 相対)
        //   とアセットの相対化 (projectRoot 相対) がずれる。ずれたまま stage / revert を
        //   通すと**別のファイルを触る**ので、ここで機能ごと止める
        repoState_ = EvaluateRepoCheck(r, projectRoot_);
        if (repoState_ != Unavailable::None) {
            return;
        }
        RequestStatus();
        // ★起動時に 1 回聞いておく (M66c)。コミットしようとした瞬間に初めて
        //   「名前が設定されていません」と言われるより、欄の下に最初から
        //   案内が出ている方がずっと親切
        RequestIdentity();
    });
}

void SourceControlSession::RequestStatus()
{
    if (!client_.Ready() || repoState_ != Unavailable::None || statusInFlight_) {
        return;
    }
    statusInFlight_ = true;
    client_.Request(collabop::kStatus, nlohmann::json::object(), [this](const nlohmann::json& msg) {
        statusInFlight_ = false;
        if (!msg.value("ok", false)) {
            ApplyError(msg);
            return;
        }
        ApplyStatusResult(msg["result"]);
    });
}

void SourceControlSession::HintChanged(const std::vector<std::string>& paths)
{
    if (!client_.Ready() || repoState_ != Unavailable::None) {
        return;
    }
    client_.Request(collabop::kHintChanged, { { "paths", paths } },
                    [this](const nlohmann::json& msg) {
                        if (!msg.value("ok", false)) {
                            ApplyError(msg);
                            return;
                        }
                        // 監視経路 (status_changed 通知) と**同じ 1 本**へ流す
                        ApplyStatusResult(msg["result"]);
                    });
}

void SourceControlSession::ApplyStatusResult(const nlohmann::json& result)
{
    model_ = BuildModel(result);
    errorCode_.clear();
    errorDetail_.clear();
}

void SourceControlSession::ApplyError(const nlohmann::json& msg)
{
    if (!msg.contains("error") || !msg["error"].is_object()) {
        return;
    }
    errorCode_ = msg["error"].value("code", std::string());
    errorDetail_ = msg["error"].value("detail", std::string());
    MYE_LOG_WARN("[collab] %s: %s", errorCode_.c_str(), errorDetail_.c_str());
    const Unavailable u = UnavailableFromCode(errorCode_);
    if (u != Unavailable::None) {
        // git が無い / 古い / リポジトリでない は「窓が使えない理由」。
        // それ以外 (locked_index など) は一時的な失敗なのでエラー文だけ出す
        if (u == Unavailable::NoGit || u == Unavailable::GitTooOld) {
            client_.SetUnavailable(u);
        } else {
            repoState_ = u;
        }
    }
}

Unavailable SourceControlSession::UnavailableFromCode(const std::string& code)
{
    if (code == collaberr::kGitMissing) {
        return Unavailable::NoGit;
    }
    if (code == collaberr::kGitTooOld) {
        return Unavailable::GitTooOld;
    }
    if (code == collaberr::kNotRepo) {
        return Unavailable::NotRepo;
    }
    if (code == collaberr::kServiceDead || code == collaberr::kInternalPanic) {
        return Unavailable::ServiceDied;
    }
    return Unavailable::None;
}

Unavailable SourceControlSession::State() const
{
    if (projectRoot_.empty()) {
        return Unavailable::NoProject;
    }
    // DLL 側の理由 (NoService / ProtoMismatch / ServiceDied) が先。
    // リポジトリの理由 (NotRepo / ToplevelMismatch) はサービスが動いて初めて分かる
    if (client_.State() != Unavailable::None) {
        return client_.State();
    }
    return repoState_;
}

void SourceControlSession::Poll()
{
    client_.Poll();
}

void SourceControlSession::Shutdown()
{
    client_.Shutdown();
    started_ = false;
}

bool SourceControlSession::TakeHeadMoved()
{
    const bool moved = headMoved_;
    headMoved_ = false;
    return moved;
}

bool SourceControlSession::AdoptCanonicalRoot()
{
    if (projectRoot_.empty()) {
        return false;
    }
    ProjectManifest manifest;
    // ★Load → 1 フィールドだけ差し替え → Save。SaveProjectManifest は構造体から
    //   全書き出しするので、読まずに書くと name / bootScene を消す
    if (!LoadProjectManifest(projectRoot_, manifest)) {
        return false;
    }
    manifest.canonicalRoot = WideToUtf8(projectRoot_);
    if (!SaveProjectManifest(projectRoot_, manifest)) {
        return false;
    }
    canonicalRoot_ = manifest.canonicalRoot;
    canonicalMismatch_ = false;
    return true;
}


// ---- M66c: stage / unstage / commit / log / diff / identity ----

std::wstring SourceControlSession::AbsolutePathOf(const std::string& rel) const
{
    // toplevel == projectRoot は EvaluateRepoCheck が保証している (違えば
    // ToplevelMismatch で機能ごと止まる) ので、素直に連結してよい
    std::filesystem::path p(projectRoot_);
    p /= std::filesystem::path(Utf8ToWide(rel));
    return p.lexically_normal().wstring();
}

bool SourceControlSession::ApplyWriteResult(const nlohmann::json& msg)
{
    if (!msg.value("ok", false)) {
        ApplyError(msg);
        return false;
    }
    // 書き込み系 op は「実行後の status」を応答に載せて返す。
    // ★取り込まずに監視 (300 ms デバウンス) 任せにすると、押した直後の一覧が
    //   古いまま = 二度押しを誘発する
    if (msg.contains("result") && msg["result"].contains("status")) {
        ApplyStatusResult(msg["result"]["status"]);
    }
    return true;
}

void SourceControlSession::StageRows(const std::vector<PairedEntry>& rows)
{
    if (!client_.Ready() || repoState_ != Unavailable::None || rows.empty()) {
        return;
    }
    const pairrule::PairPlan plan = pairrule::Collect(rows, [this](const std::string& rel) {
        std::error_code ec;
        return std::filesystem::exists(AbsolutePathOf(rel), ec);
    });
    // ★EnsureMeta を **git を呼ぶ前に** 済ませる。plan.toStage には生成後の
    //   `<path>.meta` が既に入っているので、順番を逆にすると pathspec エラーで
    //   選択ごと失敗する
    for (const std::string& asset : plan.toEnsureMeta) {
        const uint64_t guid = AssetDatabase::EnsureMeta(AbsolutePathOf(asset));
        MYE_LOG_INFO("[collab] created %s.meta (guid %llu) before staging", asset.c_str(),
                     static_cast<unsigned long long>(guid));
    }
    if (plan.toStage.empty()) {
        return;
    }
    client_.Request(collabop::kStage, { { "paths", plan.toStage } },
                    [this](const nlohmann::json& msg) { ApplyWriteResult(msg); });
}

void SourceControlSession::UnstageRows(const std::vector<PairedEntry>& rows)
{
    if (!client_.Ready() || repoState_ != Unavailable::None || rows.empty()) {
        return;
    }
    const std::vector<std::string> paths = pairrule::ListedPaths(rows);
    if (paths.empty()) {
        return;
    }
    client_.Request(collabop::kUnstage, { { "paths", paths } },
                    [this](const nlohmann::json& msg) { ApplyWriteResult(msg); });
}

void SourceControlSession::StageSavedPath(const std::wstring& absPath)
{
    if (!client_.Ready() || repoState_ != Unavailable::None || absPath.empty()) {
        return;
    }
    std::error_code ec;
    // ★lexically_relative ではなく std::filesystem::relative を使う理由:
    //   projectRoot_ は起動引数由来で相対のことがあり、絶対化していないと
    //   ".." だらけの結果になる
    const std::filesystem::path rel =
        std::filesystem::relative(std::filesystem::path(absPath), std::filesystem::path(projectRoot_), ec);
    if (ec || rel.empty()) {
        return;
    }
    std::string relUtf8 = WideToUtf8(rel.wstring());
    for (char& c : relUtf8) {
        if (c == '\\') {
            c = '/'; // git は toplevel 相対の '/' 区切りしか受けない
        }
    }
    // リポジトリの外 (別ドライブや親ディレクトリ) は黙って諦める。
    // ★ここを通すと **プロジェクト外のファイルを stage する**
    if (relUtf8.empty() || relUtf8.rfind("..", 0) == 0) {
        MYE_LOG_WARN("[collab] saved document is outside the repository: %s", relUtf8.c_str());
        return;
    }
    // status に出ていない (= まだ保存直後で status が古い) こともあるので、
    // primaryListed は false のまま。Collect が「実在するなら渡す」で拾う
    PairedEntry row;
    row.path = relUtf8;
    StageRows({ row });
}

void SourceControlSession::Commit(const std::string& message)
{
    if (!client_.Ready() || repoState_ != Unavailable::None || message.empty()) {
        return;
    }
    client_.Request(collabop::kCommit, { { "message", message } }, [this](const nlohmann::json& msg) {
        if (!ApplyWriteResult(msg)) {
            return;
        }
        lastCommit_ = msg["result"].value("head", std::string());
        MYE_LOG_INFO("[collab] commit %s", lastCommit_.c_str());
        // 履歴を一度でも見ていたら取り直す (History タブを開いたまま commit した
        // ときに、自分のコミットが出てこないのは明らかに壊れて見える)
        if (historyValid_ && historyCount_ > 0) {
            RequestLog(historyCount_);
        }
    });
}

void SourceControlSession::RequestLog(int n)
{
    if (!client_.Ready() || repoState_ != Unavailable::None || logInFlight_) {
        return;
    }
    historyCount_ = n;
    logInFlight_ = true;
    client_.Request(collabop::kLog, { { "n", n } }, [this](const nlohmann::json& msg) {
        logInFlight_ = false;
        if (!msg.value("ok", false)) {
            ApplyError(msg);
            return;
        }
        history_.clear();
        const nlohmann::json& result = msg["result"];
        if (result.contains("commits") && result["commits"].is_array()) {
            for (const nlohmann::json& c : result["commits"]) {
                CommitInfo info;
                info.sha = c.value("sha", std::string());
                info.author = c.value("author", std::string());
                info.date = c.value("date", std::string());
                info.subject = c.value("subject", std::string());
                history_.push_back(std::move(info));
            }
        }
        // ★commit が 0 件でも valid にする。未出生ブランチ (clone 直後 / git init 直後)
        //   で「読み込み中...」のまま止まって見えるのを防ぐ
        historyValid_ = true;
    });
}

void SourceControlSession::RequestDiff(const std::string& path, bool staged)
{
    if (!client_.Ready() || repoState_ != Unavailable::None || path.empty()) {
        return;
    }
    // 応答が返る前に別の行を選んでも、最後に投げた要求の結果だけが残るように
    // 「今どのパスを見ているか」を先に確定させる
    diff_.path = path;
    diff_.staged = staged;
    diff_.loading = true;
    client_.Request(collabop::kDiff, { { "path", path }, { "staged", staged } },
                    [this, path, staged](const nlohmann::json& msg) {
                        // 先行して投げた別パスの応答が後から来ることがある。
                        // 今見ているものと違えば捨てる (捨てないと一覧の選択と
                        // 差分の中身が食い違ったまま残る)
                        if (diff_.path != path || diff_.staged != staged) {
                            return;
                        }
                        diff_.loading = false;
                        if (!msg.value("ok", false)) {
                            ApplyError(msg);
                            diff_.text.clear();
                            diff_.valid = true;
                            return;
                        }
                        diff_.text = msg["result"].value("text", std::string());
                        diff_.truncated = msg["result"].value("truncated", false);
                        diff_.valid = true;
                    });
}

void SourceControlSession::RequestIdentity()
{
    if (!client_.Ready() || repoState_ != Unavailable::None) {
        return;
    }
    client_.Request(collabop::kIdentityCheck, nlohmann::json::object(),
                    [this](const nlohmann::json& msg) {
                        if (!msg.value("ok", false)) {
                            ApplyError(msg);
                            return;
                        }
                        const nlohmann::json& r = msg["result"];
                        identityOk_ = r.value("ok", false);
                        identityName_ = r.value("name", std::string());
                        identityEmail_ = r.value("email", std::string());
                        identityChecked_ = true;
                    });
}

std::string SourceControlSession::TakeLastCommit()
{
    std::string sha;
    sha.swap(lastCommit_);
    return sha;
}

// ---- M66d: revert ----

void SourceControlSession::Revert(const std::vector<std::string>& paths, WriteDoneFn done)
{
    if (paths.empty()) {
        return;
    }
    if (!client_.Ready() || repoState_ != Unavailable::None) {
        // ★呼び手 (GitTransaction) は ReloadHub::BeginBatch を済ませた後なので、
        //   ここで黙って return すると一括モードのまま帰ってこない。
        //   必ずコールバックを呼んで「失敗した」と伝える
        if (done) {
            done(false, collaberr::kServiceDead, "source control is not available");
        }
        return;
    }
    client_.Request(collabop::kRevert, { { "paths", paths } },
                    [this, done = std::move(done)](const nlohmann::json& msg) {
                        const bool ok = ApplyWriteResult(msg);
                        if (done) {
                            done(ok, errorCode_, errorDetail_);
                        }
                    });
}

} // namespace mye
