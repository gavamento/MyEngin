#include "Engine/Engine/SaveGame.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "nlohmann/json.hpp"

#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace mye::SaveGameFile {
namespace {

using nlohmann::json;

constexpr int kVersion = 1;

std::string BytesToHex(const std::vector<uint8_t>& v)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string s;
    s.reserve(v.size() * 2);
    for (uint8_t b : v) {
        s.push_back(kHex[b >> 4]);
        s.push_back(kHex[b & 0xF]);
    }
    return s;
}

int HexVal(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

bool HexToBytes(const std::string& s, std::vector<uint8_t>& out)
{
    if (s.size() % 2 != 0) {
        return false;
    }
    out.clear();
    out.reserve(s.size() / 2);
    for (size_t i = 0; i < s.size(); i += 2) {
        const int hi = HexVal(s[i]);
        const int lo = HexVal(s[i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

bool HexToU64(const std::string& s, uint64_t& out)
{
    if (s.size() != 16) {
        return false; // キーは %016llX 固定幅で書く (辞書順 = 数値順の担保でもある)
    }
    uint64_t v = 0;
    for (char c : s) {
        const int d = HexVal(c);
        if (d < 0) {
            return false;
        }
        v = (v << 4) | static_cast<uint64_t>(d);
    }
    out = v;
    return true;
}

} // namespace

std::wstring PathForSlot(const std::wstring& saveDir, int slot)
{
    return saveDir + L"\\slot" + std::to_wstring(slot) + L".json";
}

bool Write(const std::wstring& path, const std::wstring& scenePath, const PersistStore& persist)
{
    json p = json::object();
    char key[17];
    for (const auto& [k, v] : persist.Entries()) { // std::map = キー昇順 (決定論的な出力)
        std::snprintf(key, sizeof(key), "%016llX", static_cast<unsigned long long>(k));
        p[key] = BytesToHex(v);
    }
    json root;
    root["version"] = kVersion;
    root["scene"] = WideToUtf8(scenePath);
    root["persist"] = std::move(p);

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) {
        MYE_LOG_WARN("[save] cannot open for write: %s", WideToUtf8(path).c_str());
        return false;
    }
    const std::string text = root.dump(2);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) {
        MYE_LOG_WARN("[save] write failed: %s", WideToUtf8(path).c_str());
        return false;
    }
    return true;
}

bool Read(const std::wstring& path, SaveGameData& out)
{
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) {
        return false; // 不在は静かに false (WARN は呼び出し側 — スロット空きは正常系)
    }
    const json root = json::parse(f, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object()) {
        MYE_LOG_WARN("[save] parse failed: %s", WideToUtf8(path).c_str());
        return false;
    }
    if (!root.contains("version") || !root["version"].is_number_integer()
        || root["version"].get<int>() != kVersion) {
        MYE_LOG_WARN("[save] unsupported version: %s", WideToUtf8(path).c_str());
        return false;
    }
    if (!root.contains("scene") || !root["scene"].is_string() || !root.contains("persist")
        || !root["persist"].is_object()) {
        MYE_LOG_WARN("[save] malformed save: %s", WideToUtf8(path).c_str());
        return false;
    }
    SaveGameData data;
    data.scenePath = Utf8ToWide(root["scene"].get<std::string>());
    for (const auto& [k, v] : root["persist"].items()) {
        uint64_t keyHash = 0;
        std::vector<uint8_t> bytes;
        if (!HexToU64(k, keyHash) || !v.is_string()
            || !HexToBytes(v.get<std::string>(), bytes)) {
            MYE_LOG_WARN("[save] malformed persist entry '%s': %s", k.c_str(),
                         WideToUtf8(path).c_str());
            return false;
        }
        data.persist[keyHash] = std::move(bytes);
    }
    out = std::move(data);
    return true;
}

} // namespace mye::SaveGameFile
