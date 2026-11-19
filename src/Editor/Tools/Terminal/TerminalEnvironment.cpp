//====================================================================================
//                          TerminalEnvironment.cpp
//  MyEngine/ 秋田蓮音                                                      11/19/2026
//                                          ターミナルの子プロセスへ渡す環境変数の組み立て (Windows 環境ブロックの規則)
//====================================================================================
#include "Editor/Tools/Terminal/TerminalEnvironment.h"

#include <algorithm>

namespace mye {

namespace {

char FoldAscii(char c)
{
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

bool IsValidName(const std::string& name)
{
    // 先頭の '=' は "=C:" などドライブ別カレントディレクトリの隠し変数。2 文字目以降の '=' は区切りと区別できない
    return !name.empty() && name.find('=', 1) == std::string::npos;
}

TerminalEnvVar* FindByName(std::vector<TerminalEnvVar>& env, std::string_view name)
{
    for (TerminalEnvVar& var : env) {
        if (CompareTerminalEnvName(var.name, name) == 0) {
            return &var;
        }
    }
    return nullptr;
}

} // namespace

int CompareTerminalEnvName(std::string_view a, std::string_view b)
{
    const size_t common = std::min(a.size(), b.size());
    for (size_t i = 0; i < common; ++i) {
        // 符号なしで比べる (UTF-8 の多バイト部分を負の char として並べない)
        const unsigned char ca = static_cast<unsigned char>(FoldAscii(a[i]));
        const unsigned char cb = static_cast<unsigned char>(FoldAscii(b[i]));
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
    }
    if (a.size() == b.size()) {
        return 0;
    }
    return a.size() < b.size() ? -1 : 1;
}

std::vector<TerminalEnvVar> BuildTerminalEnvironment(const std::vector<TerminalEnvVar>& parent,
                                                     const std::vector<TerminalEnvVar>& additions,
                                                     std::string_view pathPrefix)
{
    std::vector<TerminalEnvVar> env;
    env.reserve(parent.size() + additions.size() + 1);
    for (const TerminalEnvVar& var : parent) {
        if (IsValidName(var.name) && FindByName(env, var.name) == nullptr) {
            env.push_back(var);
        }
    }
    for (const TerminalEnvVar& var : additions) {
        if (IsValidName(var.name) && FindByName(env, var.name) == nullptr) {
            env.push_back(var);
        }
    }
    if (!pathPrefix.empty()) {
        if (TerminalEnvVar* path = FindByName(env, "PATH")) {
            path->value = path->value.empty() ? std::string(pathPrefix) : std::string(pathPrefix) + ";" + path->value;
        } else {
            env.push_back(TerminalEnvVar{ "PATH", std::string(pathPrefix) });
        }
    }
    // 重複は上で除いたので名前は全て異なり、比較は全順序 (stable である必要は無いが、入力順を残して読みやすくする)
    std::stable_sort(env.begin(), env.end(), [](const TerminalEnvVar& a, const TerminalEnvVar& b) {
        return CompareTerminalEnvName(a.name, b.name) < 0;
    });
    return env;
}

std::string FormatTerminalEnvironmentBlock(const std::vector<TerminalEnvVar>& env)
{
    std::string block;
    for (const TerminalEnvVar& var : env) {
        block += var.name;
        block += '=';
        block += var.value;
        block += '\0';
    }
    if (env.empty()) {
        // 空のブロックも終端の 2 個の NUL が要る (CreateProcess の lpEnvironment の規則)
        block += '\0';
    }
    block += '\0';
    return block;
}

} // namespace mye
