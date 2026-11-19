//====================================================================================
//                          TerminalShellResolve.cpp
//  MyEngine/ 秋田蓮音                                                      11/19/2026
//                                          ターミナルで起動するシェルの解決 (設定値と存在確認から実行パスと引数を決める)
//====================================================================================
#include "Editor/Tools/Terminal/TerminalShellResolve.h"

namespace mye {

namespace {

bool IsSpace(char c)
{
    return c == ' ' || c == '\t';
}

std::string_view Trim(std::string_view text)
{
    while (!text.empty() && IsSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && IsSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

// 空白区切り。"..." の中の空白は区切らない (引用符自体は落とす)。閉じない引用符なら false
bool SplitArgs(std::string_view text, std::vector<std::string>& out)
{
    std::string current;
    bool inQuote = false;
    bool hasToken = false;
    for (const char c : text) {
        if (c == '"') {
            inQuote = !inQuote;
            hasToken = true;
        } else if (IsSpace(c) && !inQuote) {
            if (hasToken) {
                out.push_back(current);
                current.clear();
                hasToken = false;
            }
        } else {
            current += c;
            hasToken = true;
        }
    }
    if (inQuote) {
        return false;
    }
    if (hasToken) {
        out.push_back(current);
    }
    return true;
}

TerminalShellCommand Found(std::string path, std::vector<std::string> args)
{
    TerminalShellCommand command;
    command.path = std::move(path);
    command.args = std::move(args);
    return command;
}

TerminalShellCommand Failed(TerminalShellError error)
{
    TerminalShellCommand command;
    command.error = error;
    return command;
}

} // namespace

TerminalShellCommand ResolveTerminalShell(std::string_view setting, const TerminalExecutableFinder& finder)
{
    const std::string_view trimmed = Trim(setting);
    if (trimmed.empty()) {
        for (const char* candidate : kTerminalAutoShells) {
            std::string path = finder(candidate);
            if (!path.empty()) {
                return Found(std::move(path), {});
            }
        }
        return Failed(TerminalShellError::NoAutoShell);
    }

    std::string_view exePart;
    std::string_view argPart;
    if (trimmed.front() == '"') {
        const size_t close = trimmed.find('"', 1);
        if (close == std::string_view::npos) {
            return Failed(TerminalShellError::ConfiguredMalformed);
        }
        exePart = trimmed.substr(1, close - 1);
        argPart = trimmed.substr(close + 1);
    } else {
        // "C:\Program Files\x\sh.exe" を引用せずに書いた場合を救うため、全体を先に 1 つのパスとして試す
        std::string whole = finder(std::string(trimmed));
        if (!whole.empty()) {
            return Found(std::move(whole), {});
        }
        size_t split = 0;
        while (split < trimmed.size() && !IsSpace(trimmed[split])) {
            ++split;
        }
        exePart = trimmed.substr(0, split);
        argPart = trimmed.substr(split);
    }

    std::vector<std::string> args;
    if (!SplitArgs(argPart, args)) {
        return Failed(TerminalShellError::ConfiguredMalformed);
    }
    if (exePart.empty()) {
        return Failed(TerminalShellError::ConfiguredNotFound);
    }
    std::string path = finder(std::string(exePart));
    if (path.empty()) {
        return Failed(TerminalShellError::ConfiguredNotFound);
    }
    return Found(std::move(path), std::move(args));
}

} // namespace mye
