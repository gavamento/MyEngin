//====================================================================================
//                          RestoreJournal.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          復元ジャーナル (適用前の内容と進み具合を、復元のたびにディスクへ残す)
//====================================================================================
#include "Editor/ChangeTracking/RestoreJournal.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include <Windows.h>
#include <bcrypt.h>

#include "Editor/ChangeTracking/PathGuard.h"
#include "Engine/Platform/PathUtil.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace mye {

namespace {

constexpr const char* kDismissedFile = "dismissed.txt";
constexpr const char* kManifestFile = "manifest.json";

const char* KindName(RestoreOp::Kind kind)
{
    switch (kind) {
    case RestoreOp::Kind::Overwrite: return "overwrite";
    case RestoreOp::Kind::Delete: return "delete";
    case RestoreOp::Kind::Create: return "create";
    case RestoreOp::Kind::Rename: return "rename";
    }
    return "overwrite";
}

const char* SourceName(RestoreOp::Source source)
{
    switch (source) {
    case RestoreOp::Source::None: return "none";
    case RestoreOp::Source::BeforeBlob: return "before_blob";
    case RestoreOp::Source::Bytes: return "bytes";
    }
    return "none";
}

template <typename T>
bool GetField(const json& object, const char* key, T& out)
{
    if (!object.is_object() || !object.contains(key)) {
        return false;
    }
    try {
        out = object[key].get<T>();
    } catch (const json::exception&) {
        return false;
    }
    return true;
}

} // namespace

const char* JournalStateName(JournalState state)
{
    switch (state) {
    case JournalState::Prepared: return "prepared";
    case JournalState::Applying: return "applying";
    case JournalState::Committed: return "committed";
    case JournalState::RollingBack: return "rolling_back";
    case JournalState::RolledBack: return "rolled_back";
    case JournalState::RecoveryRequired: return "recovery_required";
    case JournalState::Recovered: return "recovered";
    }
    return "prepared";
}

bool ParseJournalState(const std::string& text, JournalState& out)
{
    for (const JournalState state : { JournalState::Prepared, JournalState::Applying, JournalState::Committed, JournalState::RollingBack,
                                      JournalState::RolledBack, JournalState::RecoveryRequired, JournalState::Recovered }) {
        if (text == JournalStateName(state)) {
            out = state;
            return true;
        }
    }
    return false;
}

bool NeedsRecovery(JournalState state)
{
    return state == JournalState::Applying || state == JournalState::RollingBack || state == JournalState::RecoveryRequired;
}

json ManifestToJson(const JournalManifest& m)
{
    json ops = json::array();
    for (const JournalOp& op : m.ops) {
        ops.push_back({ { "kind", op.kind }, { "path", op.path }, { "toPath", op.toPath }, { "source", op.source }, { "hash", op.hash }, { "size", op.size } });
    }
    json files = json::array();
    for (const JournalFile& f : m.files) {
        files.push_back({ { "path", f.path }, { "existed", f.existed }, { "preHash", f.preHash }, { "preSize", f.preSize }, { "preFile", f.preFile }, { "states", f.states } });
    }
    return { { "version", m.version }, { "restoreId", m.restoreId }, { "state", JournalStateName(m.state) }, { "revisionId", m.revisionId }, { "scope", m.scope },
             { "hunkIds", m.hunkIds }, { "startedAt", m.startedAt }, { "updatedAt", m.updatedAt }, { "ops", ops }, { "files", files },
             { "completedOps", m.completedOps }, { "error", m.error }, { "resolution", m.resolution } };
}

bool ManifestFromJson(const json& j, JournalManifest& out)
{
    JournalManifest m;
    std::string state;
    if (!GetField(j, "restoreId", m.restoreId) || m.restoreId.empty() || !GetField(j, "state", state) || !ParseJournalState(state, m.state)) {
        return false;
    }
    GetField(j, "version", m.version);
    GetField(j, "revisionId", m.revisionId);
    GetField(j, "scope", m.scope);
    GetField(j, "hunkIds", m.hunkIds);
    GetField(j, "startedAt", m.startedAt);
    GetField(j, "updatedAt", m.updatedAt);
    GetField(j, "completedOps", m.completedOps);
    GetField(j, "error", m.error);
    GetField(j, "resolution", m.resolution);
    if (!j.contains("ops") || !j["ops"].is_array() || !j.contains("files") || !j["files"].is_array()) {
        return false;
    }
    for (const json& o : j["ops"]) {
        JournalOp op;
        if (!GetField(o, "kind", op.kind) || !GetField(o, "path", op.path)) {
            return false;
        }
        GetField(o, "toPath", op.toPath);
        GetField(o, "source", op.source);
        GetField(o, "hash", op.hash);
        GetField(o, "size", op.size);
        m.ops.push_back(std::move(op));
    }
    for (const json& f : j["files"]) {
        JournalFile file;
        if (!GetField(f, "path", file.path) || !GetField(f, "states", file.states) || file.states.empty()) {
            return false;
        }
        GetField(f, "existed", file.existed);
        GetField(f, "preHash", file.preHash);
        GetField(f, "preSize", file.preSize);
        GetField(f, "preFile", file.preFile);
        // 軌跡の長さは操作の数 + 1。違う manifest は信用しない (巻き戻しの判定が狂う)
        if (file.states.size() != m.ops.size() + 1) {
            return false;
        }
        m.files.push_back(std::move(file));
    }
    out = std::move(m);
    return true;
}

std::vector<JournalOp> ToJournalOps(const std::vector<RestoreOp>& ops)
{
    std::vector<JournalOp> out;
    for (const RestoreOp& op : ops) {
        JournalOp j;
        j.kind = KindName(op.kind);
        j.path = op.path;
        j.toPath = op.toPath;
        j.source = SourceName(op.source);
        j.hash = op.hash;
        j.size = op.size;
        out.push_back(std::move(j));
    }
    return out;
}

std::vector<std::string> TouchedPaths(const std::vector<RestoreOp>& ops)
{
    std::vector<std::string> paths;
    auto add = [&](const std::string& path) {
        if (path.empty()) {
            return;
        }
        const std::string key = RestorePathKey(path);
        if (std::none_of(paths.begin(), paths.end(), [&](const std::string& p) { return RestorePathKey(p) == key; })) {
            paths.push_back(path);
        }
    };
    for (const RestoreOp& op : ops) {
        add(op.path);
        if (op.kind == RestoreOp::Kind::Rename) {
            add(op.toPath);
        }
    }
    return paths;
}

std::vector<JournalFile> BuildJournalFiles(const std::vector<RestoreOp>& ops, const std::vector<std::pair<std::string, std::string>>& initial)
{
    std::vector<JournalFile> files;
    std::map<std::string, size_t> index; // 鍵 → files の添字
    std::vector<std::string> current;    // files と同じ並びの「今の」ハッシュ
    for (const auto& [path, hash] : initial) {
        index[RestorePathKey(path)] = files.size();
        JournalFile f;
        f.path = path;
        f.existed = !hash.empty();
        f.preHash = hash;
        f.states.push_back(hash);
        files.push_back(std::move(f));
        current.push_back(hash);
    }
    auto at = [&](const std::string& path) { return index.at(RestorePathKey(path)); };
    for (const RestoreOp& op : ops) {
        switch (op.kind) {
        case RestoreOp::Kind::Overwrite:
        case RestoreOp::Kind::Create: current[at(op.path)] = op.hash; break;
        case RestoreOp::Kind::Delete: current[at(op.path)].clear(); break;
        case RestoreOp::Kind::Rename: {
            const std::string moved = current[at(op.path)];
            current[at(op.toPath)] = moved;
            current[at(op.path)].clear();
            break;
        }
        }
        for (size_t i = 0; i < files.size(); ++i) {
            files[i].states.push_back(current[i]);
        }
    }
    return files;
}

int FindInTrajectory(const JournalFile& file, const std::string& currentHash)
{
    for (size_t i = 0; i < file.states.size(); ++i) {
        if (file.states[i] == currentHash) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::string NewRestoreId()
{
    unsigned char bytes[16] = {};
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        const uint64_t t = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        for (size_t i = 0; i < sizeof(bytes); ++i) {
            bytes[i] = static_cast<unsigned char>((t >> ((i % 8) * 8)) ^ (i * 29));
        }
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out += '-';
        }
        out += kHex[bytes[i] >> 4];
        out += kHex[bytes[i] & 15];
    }
    return out;
}

int64_t WallClockMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// ---- RestoreJournalStore ----

std::wstring RestoreJournalStore::RecoveryRoot() const
{
    return root_ + L"\\.mye\\ai_changes\\recovery";
}

std::wstring RestoreJournalStore::DirOf(const std::string& id) const
{
    return RecoveryRoot() + L"\\" + Utf8ToWide(id);
}

std::wstring RestoreJournalStore::FileIn(const std::string& id, const std::string& rel) const
{
    std::wstring wide = Utf8ToWide(rel);
    std::replace(wide.begin(), wide.end(), L'/', L'\\');
    return DirOf(id) + L"\\" + wide;
}

bool RestoreJournalStore::Begin(const JournalManifest& manifest)
{
    return io_->MakeDirs(DirOf(manifest.restoreId) + L"\\pre") && Save(manifest);
}

bool RestoreJournalStore::Save(const JournalManifest& manifest)
{
    return io_->Write(DirOf(manifest.restoreId) + L"\\" + Utf8ToWide(kManifestFile), ManifestToJson(manifest).dump(2));
}

bool RestoreJournalStore::Discard(const std::string& id)
{
    return io_->RemoveTree(DirOf(id));
}

bool RestoreJournalStore::MarkDismissed(const std::string& id)
{
    return io_->Write(DirOf(id) + L"\\" + Utf8ToWide(kDismissedFile), "dismissed");
}

std::vector<RestoreJournalStore::Entry> RestoreJournalStore::Scan() const
{
    std::vector<Entry> entries;
    std::error_code ec;
    const fs::path base(RecoveryRoot());
    if (!fs::is_directory(base, ec)) {
        return entries;
    }
    for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entryEc;
        if (!it->is_directory(entryEc)) {
            continue;
        }
        if (fs::exists(it->path() / kDismissedFile, entryEc)) {
            continue;
        }
        Entry entry;
        entry.id = WideToUtf8(it->path().filename().wstring());
        std::ifstream in(it->path() / kManifestFile, std::ios::binary);
        if (!in) {
            entry.broken = true;
            entry.brokenReason = "manifest.json missing or unreadable";
        } else {
            std::stringstream text;
            text << in.rdbuf();
            const json parsed = json::parse(text.str(), nullptr, false);
            if (parsed.is_discarded() || !ManifestFromJson(parsed, entry.manifest)) {
                entry.broken = true;
                entry.brokenReason = "manifest.json is damaged";
            }
        }
        entries.push_back(std::move(entry));
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.id < b.id; });
    return entries;
}

int RestoreJournalStore::Prune(int64_t nowMs, int64_t keepMs)
{
    int removed = 0;
    for (const Entry& entry : Scan()) {
        if (entry.broken) {
            continue; // 壊れたものは利用者が見るまで残す
        }
        const JournalManifest& m = entry.manifest;
        const bool stale = m.state == JournalState::Prepared
            || ((m.state == JournalState::Committed || m.state == JournalState::RolledBack || m.state == JournalState::Recovered) && m.updatedAt + keepMs < nowMs);
        if (stale && Discard(entry.id)) {
            ++removed;
        }
    }
    return removed;
}

} // namespace mye
