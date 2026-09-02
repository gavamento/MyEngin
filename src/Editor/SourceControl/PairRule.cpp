#include "Editor/SourceControl/PairRule.h"

#include <algorithm>
#include <set>

#include "Engine/Engine/AssetDatabase.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace pairrule {

namespace {

bool EndsWith(const std::string& s, const char* suffix)
{
    const size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool Contains(const std::vector<std::string>& v, const std::string& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

} // namespace

std::vector<std::string> SidecarCandidates(const std::string& primary)
{
    std::vector<std::string> out;
    if (primary.empty()) {
        return out;
    }
    out.push_back(primary + ".meta");
    if (EndsWith(primary, ".terrain.json")) {
        // ".json" (5 文字) を ".edit" へ差し替える
        std::string edit = primary.substr(0, primary.size() - 5) + ".edit";
        out.push_back(edit + ".meta");
        out.push_back(std::move(edit));
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool IsAssetPath(const std::string& path)
{
    // ClassifyPath は拡張子しか見ないので、toplevel 相対のまま渡してよい
    return AssetDatabase::ClassifyPath(Utf8ToWide(path)) != AssetType::Unknown;
}

PairPlan Collect(const std::vector<PairedEntry>& rows, const PathExistsFn& exists)
{
    // std::set = 重複除去 + 昇順。件数はユーザーの選択なので log n で十分
    std::set<std::string> stage;
    std::set<std::string> ensure;
    for (const PairedEntry& row : rows) {
        if (row.path.empty()) {
            continue;
        }
        // 本体: status に出ているか、ディスクに実在するときだけ渡す。
        // ★どちらでもないパスを渡すと git は pathspec エラーで**その呼び出しごと**
        //   失敗する = 一緒に選んだ他のファイルまで stage されない
        if (row.primaryListed || (exists && exists(row.path))) {
            stage.insert(row.path);
        }
        for (const std::string& side : SidecarCandidates(row.path)) {
            if (Contains(row.sidecars, side) || (exists && exists(side))) {
                stage.insert(side);
            }
        }
        // status に出ているサイドカーは、綴り候補に無いものでも取りこぼさない
        // (BuildModel が束ねた根拠は PrimaryPathFor で、候補表とは別方向の写像)
        for (const std::string& side : row.sidecars) {
            stage.insert(side);
        }
        // ★.meta の欠落補完。「資産なのに .meta がまだ無い」= エディタの外で
        //   置かれた新規アセット。ここで作らずに commit すると、他人のマシンで
        //   GUID が作り直されてシーンの参照が静かに壊れる
        const std::string meta = row.path + ".meta";
        if (IsAssetPath(row.path) && exists && exists(row.path) && !exists(meta)) {
            ensure.insert(row.path);
            stage.insert(meta); // EnsureMeta の後には必ず存在する
        }
    }
    PairPlan plan;
    plan.toStage.assign(stage.begin(), stage.end());
    plan.toEnsureMeta.assign(ensure.begin(), ensure.end());
    return plan;
}

std::vector<std::string> ListedPaths(const std::vector<PairedEntry>& rows)
{
    std::set<std::string> out;
    for (const PairedEntry& row : rows) {
        if (row.primaryListed && !row.path.empty()) {
            out.insert(row.path);
        }
        for (const std::string& side : row.sidecars) {
            out.insert(side);
        }
    }
    return std::vector<std::string>(out.begin(), out.end());
}

} // namespace pairrule
} // namespace mye
