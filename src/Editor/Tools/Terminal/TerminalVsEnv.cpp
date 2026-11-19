//====================================================================================
//                          TerminalVsEnv.cpp
//  MyEngine/ 秋田蓮音                                                      11/19/2026
//                                          VS 開発者環境の取得 (set 出力の解析・差分・非同期の読み込み) とエンジン環境変数の組み立て
//====================================================================================
#include "Editor/Tools/Terminal/TerminalVsEnv.h"

#include <algorithm>
#include <map>
#include <utility>

namespace mye {

namespace {

// 名前を大文字小文字無視で並べる (Windows の環境変数名の比較)。検索専用で、出力の順序には使わない
struct EnvNameLess
{
    bool operator()(const std::string& a, const std::string& b) const { return CompareTerminalEnvName(a, b) < 0; }
};

bool IsNameOf(const std::string& name, std::string_view expected)
{
    return CompareTerminalEnvName(name, expected) == 0;
}

// 行末の空白・タブ・CR を落とす (目印の行の比較用。値の行には使わない: 値の末尾の空白は値の一部)
std::string_view TrimTrailingBlanks(std::string_view text)
{
    while (!text.empty() && (text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

// 次の 1 行を取り出して text を進める。LF 区切りで、行末の CR を 1 つ落とす
std::string_view TakeLine(std::string_view& text)
{
    const size_t lf = text.find('\n');
    std::string_view line = text.substr(0, lf);
    text = lf == std::string_view::npos ? std::string_view() : text.substr(lf + 1);
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    return line;
}

} // namespace

bool ParseTerminalSetOutput(std::string_view output, std::string_view marker, std::vector<TerminalEnvVar>& out)
{
    out.clear();
    std::string_view rest = output;
    if (!marker.empty()) {
        bool found = false;
        while (!rest.empty()) {
            if (TrimTrailingBlanks(TakeLine(rest)) == marker) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }

    std::map<std::string, bool, EnvNameLess> seen;
    while (!rest.empty()) {
        const std::string_view line = TakeLine(rest);
        // "=C:=C:\dir" のような隠し変数は名前が '=' で始まる。名前の区切りは 2 文字目以降の最初の '='
        const size_t pos = line.find('=', line.empty() || line[0] != '=' ? 0 : 1);
        if (pos == std::string_view::npos || (line[0] == '=' && pos == 1)) {
            continue;   // '=' が無い行 (改行を含む値の続きなど) と、名前が空の行
        }
        TerminalEnvVar var;
        var.name.assign(line.substr(0, pos));
        if (!seen.emplace(var.name, true).second) {
            continue;   // 同名は先のものを残す
        }
        var.value.assign(line.substr(pos + 1));
        out.push_back(std::move(var));
    }
    return true;
}

std::vector<TerminalEnvVar> ComputeTerminalEnvironmentDiff(const std::vector<TerminalEnvVar>& parent, const std::vector<TerminalEnvVar>& vsEnv)
{
    std::map<std::string, const std::string*, EnvNameLess> parentValues;
    for (const TerminalEnvVar& var : parent) {
        parentValues.emplace(var.name, &var.value);   // 同名は先のものを残す (emplace は既存を上書きしない)
    }
    std::vector<TerminalEnvVar> diff;
    std::map<std::string, bool, EnvNameLess> emitted;
    for (const TerminalEnvVar& var : vsEnv) {
        if (var.name.empty() || var.name[0] == '=') {
            continue;
        }
        const auto it = parentValues.find(var.name);
        if (it != parentValues.end() && *it->second == var.value) {
            continue;
        }
        if (emitted.emplace(var.name, true).second) {
            diff.push_back(var);
        }
    }
    return diff;
}

std::vector<TerminalEnvVar> ApplyTerminalEnvironmentDiff(const std::vector<TerminalEnvVar>& parent, const std::vector<TerminalEnvVar>& diff)
{
    std::vector<TerminalEnvVar> result = parent;
    std::map<std::string, size_t, EnvNameLess> index;
    for (size_t i = 0; i < result.size(); ++i) {
        index.emplace(result[i].name, i);
    }
    for (const TerminalEnvVar& var : diff) {
        const auto it = index.find(var.name);
        if (it != index.end()) {
            result[it->second].value = var.value;   // 親の綴りの名前のまま値だけ差し替える
        } else {
            index.emplace(var.name, result.size());
            result.push_back(var);
        }
    }
    return result;
}

std::string FormatTerminalId(uint32_t editorPid, uint64_t editorStartUnixSeconds, int serial)
{
    return std::to_string(editorPid) + "-" + std::to_string(editorStartUnixSeconds) + "-" + std::to_string(serial);
}

bool TerminalIdBelongsToEditor(std::string_view id, uint32_t editorPid, uint64_t editorStartUnixSeconds)
{
    const std::string prefix = std::to_string(editorPid) + "-" + std::to_string(editorStartUnixSeconds) + "-";
    if (id.size() <= prefix.size() || id.substr(0, prefix.size()) != prefix) {
        return false;
    }
    return std::all_of(id.begin() + prefix.size(), id.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::vector<TerminalEnvVar> BuildTerminalLaunchEnvironment(const std::vector<TerminalEnvVar>& parent, const std::vector<TerminalEnvVar>& vsDiff,
                                                           std::string_view projectRoot, std::string_view terminalId)
{
    std::vector<TerminalEnvVar> base = ApplyTerminalEnvironmentDiff(parent, vsDiff);
    // エンジンが決める 3 つは、親から継いだ値を捨てる (BuildTerminalEnvironment は同名の親の値を残すため、先に外す)
    std::erase_if(base, [](const TerminalEnvVar& var) {
        return IsNameOf(var.name, kTerminalEnvProjectRoot) || IsNameOf(var.name, kTerminalEnvTerminalId) ||
               IsNameOf(var.name, kTerminalEnvTermProgram);
    });

    // 末尾の区切りは落とす。ただし "C:\" は "C:" (ドライブの作業ディレクトリ) になってしまうので残す
    while (projectRoot.size() > 1 && (projectRoot.back() == '\\' || projectRoot.back() == '/') && projectRoot[projectRoot.size() - 2] != ':') {
        projectRoot.remove_suffix(1);
    }

    std::vector<TerminalEnvVar> additions;
    additions.push_back({ std::string(kTerminalEnvTerminalId), std::string(terminalId) });
    additions.push_back({ std::string(kTerminalEnvTermProgram), std::string(kTerminalTermProgramValue) });
    additions.push_back({ std::string(kTerminalEnvColorTerm), std::string(kTerminalColorTermValue) });
    std::string pathPrefix;
    if (!projectRoot.empty()) {
        additions.push_back({ std::string(kTerminalEnvProjectRoot), std::string(projectRoot) });
        // PATH は ';' 区切りで、要素の中の ';' は書き表せない。そのときは前置しない
        if (projectRoot.find(';') == std::string_view::npos) {
            pathPrefix = std::string(projectRoot);
            if (pathPrefix.back() != '\\' && pathPrefix.back() != '/') {
                pathPrefix += '\\';   // "C:\" のようにドライブ直下のときは、区切りが既にある
            }
            pathPrefix += kTerminalToolsDirName;
        }
    }
    return BuildTerminalEnvironment(base, additions, pathPrefix);
}

TerminalVsEnvLoader::TerminalVsEnvLoader(TerminalVsEnvFetch fetch, std::chrono::milliseconds timeout, std::chrono::milliseconds shutdownWait)
    : fetch_(std::move(fetch))
    , timeout_(timeout)
    , shutdownWait_(shutdownWait)
{
}

TerminalVsEnvLoader::~TerminalVsEnvLoader()
{
    if (!worker_.joinable()) {
        return;
    }
    shared_->cancel = true;
    bool finished = false;
    {
        std::unique_lock<std::mutex> lock(shared_->mutex);
        shared_->done.wait_for(lock, shutdownWait_, [this] { return shared_->finished; });
        finished = shared_->finished;
    }
    if (finished) {
        worker_.join();
    } else {
        worker_.detach();   // 戻ってこない fetch (vswhere のハングなど) でエディタの終了を止めない
    }
}

void TerminalVsEnvLoader::Start(Clock::time_point now)
{
    if (state_ != TerminalVsEnvState::Idle) {
        return;
    }
    shared_ = std::make_shared<Shared>();
    deadline_ = now + timeout_;
    state_ = TerminalVsEnvState::Loading;
    worker_ = std::thread([shared = shared_, fetch = fetch_]() {
        TerminalVsEnvFetchResult result;
        try {
            result = fetch(shared->cancel);
        } catch (...) {
            result = TerminalVsEnvFetchResult{};
            result.detail = "the loader threw an exception";
        }
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->result = std::move(result);
            shared->finished = true;
        }
        shared->done.notify_all();
    });
}

TerminalVsEnvState TerminalVsEnvLoader::Poll(Clock::time_point now)
{
    if (state_ != TerminalVsEnvState::Loading) {
        return state_;
    }
    bool finished = false;
    TerminalVsEnvFetchResult result;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        finished = shared_->finished;
        if (finished) {
            result = std::move(shared_->result);
        }
    }
    if (finished) {
        worker_.join();   // finished を立てた直後に抜ける。待つのは一瞬
        detail_ = std::move(result.detail);
        switch (result.status) {
        case TerminalVsEnvStatus::Ok:
            diff_ = std::move(result.diff);
            state_ = TerminalVsEnvState::Ready;
            break;
        case TerminalVsEnvStatus::NotInstalled:
            state_ = TerminalVsEnvState::NotInstalled;
            break;
        case TerminalVsEnvStatus::TimedOut:
            state_ = TerminalVsEnvState::TimedOut;
            break;
        case TerminalVsEnvStatus::Failed:
            state_ = TerminalVsEnvState::Failed;
            break;
        }
        return state_;
    }
    if (now >= deadline_) {
        shared_->cancel = true;   // スレッドはここでは待たない。破棄時に shutdownWait だけ待つ
        detail_ = "timed out";
        state_ = TerminalVsEnvState::TimedOut;
    }
    return state_;
}

} // namespace mye
