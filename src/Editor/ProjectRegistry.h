#pragma once
#include <string>
#include <vector>

namespace mye {

// プロジェクト履歴の 1 エントリ (%LOCALAPPDATA%\MyEngine\projects.json)
struct ProjectRegistryEntry {
    std::wstring path;         // プロジェクトルートの絶対パス
    std::string name;          // マニフェストの name (表示用キャッシュ)
    std::string lastOpenedIso; // ISO8601 UTC ("2026-07-21T12:34:56Z")
    bool pinned = false;
    bool missing = false; // Load 時の存在確認結果 (非永続)
};

// Unity Hub 相当のプロジェクト履歴。マシンローカル (VCS 外) に永続化する。
// 表示順: pinned 優先 → lastOpened 降順。重複判定は NormalizePathKey。
class ProjectRegistry {
public:
    void Load();
    void Save() const;

    // 追加 or lastOpened 更新 (開いた時に呼ぶ)。name はマニフェストから
    void Touch(const std::wstring& projectRoot, const std::string& name);
    void SetPinned(const std::wstring& projectRoot, bool pinned);
    // 表示名のみ更新して保存 (Touch と違い lastOpened を触らない = 並び順を変えない)
    void SetName(const std::wstring& projectRoot, const std::string& name);
    void Remove(const std::wstring& projectRoot);

    const std::vector<ProjectRegistryEntry>& Entries() const { return entries_; }

    // %LOCALAPPDATA%\MyEngine\projects.json (LOCALAPPDATA 不在時は exe 隣にフォールバック)
    static std::wstring RegistryPath();

private:
    void SortEntries();
    ProjectRegistryEntry* FindByPath(const std::wstring& projectRoot);

    std::vector<ProjectRegistryEntry> entries_;
};

} // namespace mye
