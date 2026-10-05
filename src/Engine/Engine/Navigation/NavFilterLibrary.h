//====================================================================================
//                          NavFilterLibrary.h
//  MyEngin/ 秋田蓮音                                                     10/05/2026
//                                          エリアのフィルタ資産 (.navfilter.json、M84c)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"

#include "Engine/Core/Ecs/EntityID.h"

namespace mye {

// エリアのフィルタ (UE の NavigationQueryFilter / Unity の NavMeshAgent.SetAreaCost 相当)。
// NavMeshAgent.navFilter とナビのクエリが参照し、Surface の areaCosts を上書きし、通れないエリアを足す。
// 実効の通れるエリア = Agent / クエリの areaMask から excludedAreas を除いたもの。
// 実効のコスト = areaCosts[i] > 0 ならその値、0 なら Surface の areaCosts[i]。
// 決定論: 値はワールドハッシュに入れない (物理マテリアルと同じ「再生時に同じ資産がある」前提。
// ファイルの中身は provenance の contentHash が守る)。読み書きは起動走査 / ReloadHub / エディタのメインスレッドのみ
struct NavAreaFilter {
    static constexpr int kAreaCount = 16; // kNavAreaCount と同じ

    uint64_t hash = 0; // = GUID (NavFilterLibrary のキー)
    std::string name;
    std::wstring path;

    float areaCosts[kAreaCount] = {}; // 0 = Surface のコストのまま。1 以上 = 上書き
    uint32_t excludedAreas = 0;       // ビット i = エリア i を通らない
};

// 列挙 1 件 (AssetRef ピッカー用)
struct NavFilterEntry {
    uint64_t hash = 0;
    std::string name;
};

// 登録済み .navfilter.json の管理 (PhysMatLibrary と同じ形)。EngineLoop が所有し navfilter:: で注入
class NavFilterLibrary {
public:
    static uint64_t HashForPath(const std::wstring& path);

    uint64_t LoadFromFile(const std::wstring& path); // 失敗時 0
    uint64_t Register(const std::wstring& path, NavAreaFilter filter); // 返り値 = hash

    const NavAreaFilter* Get(uint64_t hash) const;
    bool Contains(uint64_t hash) const { return filters_.find(hash) != filters_.end(); }
    std::vector<NavFilterEntry> Enumerate() const; // 名前昇順 (ハッシュの反復順を表に出さない)

    static nlohmann::json ToJson(const NavAreaFilter& f);
    // 読み値は Sanitize 済みで返す。フィルタでない JSON ("navfilter" キー無し) は false
    static bool FromJson(const nlohmann::json& j, NavAreaFilter& out);
    // 非有限・範囲外の防波堤 (コストは 0 か 1..1000、excludedAreas は 16 ビット)
    static void Sanitize(NavAreaFilter& f);

private:
    std::unordered_map<uint64_t, NavAreaFilter> filters_;
};

// モジュール注入 (physmat:: と同じ流儀)。EngineLoop が起動時に Install し、終了時にライブラリ破棄前に外す。メインスレッド専用
namespace navfilter {
void Install(NavFilterLibrary* lib);
NavFilterLibrary* Library();                // 未接続 = nullptr
const NavAreaFilter* Resolve(AssetID id);   // 未接続 / 未登録 / null ID = nullptr
const NavAreaFilter* Resolve(uint64_t guid);
} // namespace navfilter

} // namespace mye
