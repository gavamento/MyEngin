#include "Engine/Engine/Prefab.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <unordered_set>

#include "Engine/Core/ComponentRegistry.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/AssetKeyResolver.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/JsonUtil.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/EntityNaming.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/SceneSerializer.h"
#include "Engine/Platform/PathUtil.h"

namespace fs = std::filesystem;

namespace mye {

using nlohmann::json;

namespace {

// ".prefab.json" / ".json" を落としたファイル名 (表示名)
std::string NameFromPath(const std::wstring& path)
{
    std::string name = WideToUtf8(fs::path(path).stem().wstring()); // "X.prefab.json" -> "X.prefab"
    const std::string suf = ".prefab";
    if (name.size() > suf.size() && name.compare(name.size() - suf.size(), suf.size(), suf) == 0) {
        name.resize(name.size() - suf.size()); // -> "X"
    }
    return name;
}

// entities 配列 (SubtreeToJson 形式) から fileId==localId の item を返す (無ければ nullptr)
const json* FindLocal(const json& entities, uint64_t localId)
{
    if (!entities.is_array()) {
        return nullptr;
    }
    for (const json& item : entities) {
        if (item.value("fileId", 0ull) == localId) {
            return &item;
        }
    }
    return nullptr;
}

// フラット展開されたベース JSON を階層で分類する (M48c)。ECS 側の SplitLevels と同じ境界:
//   ownLevel   = 自レベル (外側のタグを付ける対象。入れ子ルートは含まない)
//   innerRoots = 入れ子インスタンスのルート (照合キーではあるが外側タグは付けない)
//   どちらにも入らない = 内側インスタンスの持ち物 (外側は一切触らない)
// **判定は PrefabInstance の位置だけ**。「PrefabLink の有無」で代用してはいけない —
// 内側インスタンスの配下にユーザーが手で足したタグ無しの子を取りこぼし、
// 外側ドメインの localId を付けてしまう (FindInstanceRoot は内側を返すので別物に化ける)
struct BaseLevels {
    std::unordered_set<uint64_t> ownLevel;
    std::unordered_set<uint64_t> innerRoots;
};

BaseLevels ClassifyBase(const json& entities)
{
    BaseLevels out;
    if (!entities.is_array()) {
        return out;
    }
    std::unordered_map<uint64_t, uint64_t> parentOf;
    std::unordered_set<uint64_t> hasInstance;
    std::vector<uint64_t> ids;
    for (const json& item : entities) {
        const uint64_t local = item.value("fileId", 0ull);
        if (local == 0) {
            continue;
        }
        ids.push_back(local);
        parentOf[local] = item.value("parent", 0ull);
        if (item.contains("components") && item["components"].contains("PrefabInstance")) {
            hasInstance.insert(local);
        }
    }
    for (uint64_t local : ids) {
        bool inner = false;
        uint64_t cur = local;
        for (size_t guard = 0; guard <= ids.size(); ++guard) { // 手書きファイルの循環に備える
            auto it = parentOf.find(cur);
            if (it == parentOf.end() || it->second == 0 || parentOf.count(it->second) == 0) {
                break; // ベースのルートまで到達
            }
            cur = it->second;
            if (hasInstance.count(cur) != 0) {
                inner = true;
                break;
            }
        }
        if (inner) {
            continue; // 内側の持ち物 — どちらの集合にも入れない
        }
        if (hasInstance.count(local) != 0) {
            out.innerRoots.insert(local);
        } else {
            out.ownLevel.insert(local);
        }
    }
    return out;
}

bool WritePrefabFile(const std::wstring& path, const std::string& name, const json& entities)
{
    json root;
    root["engine"] = "MyEngine";
    root["prefab"] = 1;
    root["name"] = name;
    root["entities"] = entities;
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream f(fs::path(path), std::ios::binary);
    if (!f) {
        return false;
    }
    const std::string text = root.dump(2);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return true;
}

// e の fileId (無ければ 0)
uint64_t FidOf(World& w, EntityID e)
{
    auto* f = w.GetComponent<FileIdComponent>(e);
    return f ? f->value : 0;
}

// root サブツリーを「root と同じインスタンス階層 (= 自レベル)」と「入れ子インスタンスのルート」に
// 切り分け、それぞれの fileId 集合を返す (M48c)。境界は FindInstanceRoot と厳密に一致する:
// root 以外で PrefabInstanceComponent を持つノードは自レベルではなく、その配下も自レベルではない。
// **fileId を鍵にする** — 呼び出し側は直前に SubtreeToJson を通しており全 fileId が確定している
void SplitLevels(World& w, EntityID root, std::unordered_set<uint64_t>& ownLevel,
                 std::unordered_set<uint64_t>& innerRoots)
{
    std::function<void(EntityID, bool)> visit = [&](EntityID e, bool isRoot) {
        const uint64_t fid = FidOf(w, e);
        if (!isRoot && w.GetComponent<PrefabInstanceComponent>(e)) {
            if (fid != 0) {
                innerRoots.insert(fid);
            }
            return; // 入れ子の境界 — ここから下は内側インスタンスの持ち物
        }
        if (fid != 0) {
            ownLevel.insert(fid);
        }
        auto* h = w.GetComponent<HierarchyComponent>(e);
        EntityID c = h ? h->firstChild : kNullEntity;
        while (!c.IsNull()) {
            auto* ch = w.GetComponent<HierarchyComponent>(c);
            const EntityID next = ch ? ch->nextSibling : kNullEntity;
            visit(c, false);
            c = next;
        }
    };
    visit(root, true);
}

// 抽出したエンティティ item のプレハブタグをベース用に整える (M48c)。
//  - 自レベル: 外側のタグはベースの定義には要らない (インスタンス化時に付く) → 剥がす
//  - 入れ子ルート: 内側の識別 (prefabHash / 内側 localId) は保ったまま、
//    外側ドメインでの位置 outerLocalId だけを新しいローカル fileId に付け替える
//  - 入れ子の配下: 触らない (localId は内側ベースのドメイン。外側は関知しない)
void RebaseTags(nlohmann::json& components, uint64_t sceneFid, uint64_t newLocalId,
                const std::unordered_set<uint64_t>& ownLevel,
                const std::unordered_set<uint64_t>& innerRoots)
{
    if (ownLevel.count(sceneFid) != 0) {
        components.erase("PrefabInstance");
        components.erase("PrefabLink");
    } else if (innerRoots.count(sceneFid) != 0 && components.contains("PrefabInstance")) {
        components["PrefabInstance"]["outerLocalId"] = newLocalId;
    }
}

// entities 内の localId を引く。ただし **入れ子インスタンスの配下に当たったら nullptr** (M48c)。
// メンバの localId は必ず「自分のベースの自レベル」を指すはずで、内側配下のエントリに
// 当たるのは番号の使い回しによる誤対応 (別インスタンスで削除された ID が内側配下へ再割り当て
// された等)。そのまま使うと Revert / 伝播が **無関係なエンティティのベース値で上書き** する
const json* FindOwnLevelLocal(const json& entities, uint64_t localId)
{
    const json* item = FindLocal(entities, localId);
    if (!item) {
        return nullptr;
    }
    const json* cur = item;
    const size_t count = entities.is_array() ? entities.size() : 0;
    for (size_t guard = 0; guard <= count; ++guard) {
        const uint64_t p = cur->value("parent", 0ull);
        if (p == 0) {
            return item; // ベースのルートまで到達 = 自レベル
        }
        const json* pe = FindLocal(entities, p);
        if (!pe) {
            return item; // 集合外の親 (通常は起きない)
        }
        if (pe->contains("components") && (*pe)["components"].contains("PrefabInstance")) {
            return nullptr; // 祖先に入れ子ルートがある = 内側の持ち物
        }
        cur = pe;
    }
    return item;
}

// e のプレハブベース item (無ければ nullptr)。ライブラリと PrefabLink.localId で解決
const json* ResolveBase(World& w, const PrefabLibrary& lib, EntityID e)
{
    auto* link = w.GetComponent<PrefabLinkComponent>(e);
    if (!link) {
        return nullptr;
    }
    const EntityID root = Prefab::FindInstanceRoot(w, e);
    if (root.IsNull()) {
        return nullptr;
    }
    auto* inst = w.GetComponent<PrefabInstanceComponent>(root);
    if (!inst) {
        return nullptr;
    }
    const PrefabAsset* a = lib.Get(inst->prefabHash);
    if (!a) {
        return nullptr;
    }
    return FindOwnLevelLocal(a->entities, link->localId);
}


std::string GetNameStr(World& w, EntityID e)
{
    auto* nc = w.GetComponent<NameComponent>(e);
    return nc ? std::string(nc->value, strnlen(nc->value, sizeof(nc->value))) : std::string();
}

} // namespace

// ==== PrefabLibrary ====

uint64_t PrefabLibrary::HashForPath(const std::wstring& path)
{
    // M30c: 移動/リネーム済みアセットは .meta の GUID がキーになる (未移動は path-hash と同値)
    return assetkey::Resolve(NormalizePathKey(path));
}

uint64_t PrefabLibrary::Register(const std::wstring& path, std::string name, json entities)
{
    const uint64_t hash = HashForPath(path);
    PrefabAsset& a = assets_[hash];
    a.hash = hash;
    a.name = std::move(name);
    a.path = path;
    a.entities = std::move(entities);
    return hash;
}

uint64_t PrefabLibrary::LoadFromFile(const std::wstring& path)
{
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) {
        return 0;
    }
    json root;
    try {
        f >> root;
    } catch (const json::exception& ex) {
        MYE_LOG_WARN("prefab parse failed: %s (%s)", WideToUtf8(path).c_str(), ex.what());
        return 0;
    }
    if (!root.contains("entities") || !root["entities"].is_array()) {
        MYE_LOG_WARN("prefab load: no entities array in %s", WideToUtf8(path).c_str());
        return 0;
    }
    std::string name = root.value("name", NameFromPath(path));
    return Register(path, std::move(name), root["entities"]);
}

const PrefabAsset* PrefabLibrary::Get(uint64_t hash) const
{
    auto it = assets_.find(hash);
    return (it != assets_.end()) ? &it->second : nullptr;
}

std::vector<PrefabEntry> PrefabLibrary::Enumerate() const
{
    std::vector<PrefabEntry> out;
    out.reserve(assets_.size());
    for (const auto& [h, a] : assets_) {
        out.push_back({ a.hash, a.name, a.path });
    }
    std::sort(out.begin(), out.end(),
              [](const PrefabEntry& x, const PrefabEntry& y) { return x.name < y.name; });
    return out;
}

// ==== Prefab operations ====

namespace Prefab {

EntityID FindInstanceRoot(World& world, EntityID e)
{
    EntityID cur = e;
    while (!cur.IsNull() && world.IsAlive(cur)) {
        if (world.GetComponent<PrefabInstanceComponent>(cur)) {
            return cur;
        }
        cur = world.GetParent(cur);
    }
    return kNullEntity;
}

void CollectInstanceMembers(World& world, EntityID root, std::vector<EntityID>& out,
                            std::vector<EntityID>* innerRoots)
{
    std::function<void(EntityID, bool)> visit = [&](EntityID e, bool isRoot) {
        if (!isRoot && world.GetComponent<PrefabInstanceComponent>(e)) {
            if (innerRoots) {
                innerRoots->push_back(e); // 入れ子の境界 — 内側は外側のメンバではない
            }
            return;
        }
        if (world.GetComponent<PrefabLinkComponent>(e)) {
            out.push_back(e);
        }
        auto* h = world.GetComponent<HierarchyComponent>(e);
        EntityID c = h ? h->firstChild : kNullEntity;
        while (!c.IsNull()) {
            auto* ch = world.GetComponent<HierarchyComponent>(c);
            const EntityID next = ch ? ch->nextSibling : kNullEntity;
            visit(c, false);
            c = next;
        }
    };
    visit(root, true);
}

json ExtractLocal(Scene& scene, EntityID root)
{
    World& w = scene.GetWorld();
    const json sub = SceneSerializer::SubtreeToJson(scene, root); // scene fileId、DFS (root 先頭)
    json out = json::array();
    if (!sub.is_array() || sub.empty()) {
        return out;
    }
    std::unordered_set<uint64_t> ownLevel;
    std::unordered_set<uint64_t> innerRoots;
    SplitLevels(w, root, ownLevel, innerRoots);

    // scene fileId -> ローカル id (配列順 = DFS。root=1)
    std::unordered_map<uint64_t, uint64_t> remap;
    uint64_t next = 1;
    for (const json& item : sub) {
        const uint64_t f = item.value("fileId", 0ull);
        if (f != 0) {
            remap[f] = next++;
        }
    }
    for (const json& item : sub) {
        json ni = item;
        const uint64_t f = item.value("fileId", 0ull);
        uint64_t newLocal = 0;
        if (auto it = remap.find(f); it != remap.end()) {
            newLocal = it->second;
            ni["fileId"] = newLocal;
        }
        if (ni.contains("parent")) {
            const uint64_t p = ni.value("parent", 0ull);
            if (auto it = remap.find(p); it != remap.end()) {
                ni["parent"] = it->second;
            } else {
                ni.erase("parent"); // 集合外の親 (= root の元親) → プレハブルート化
            }
        }
        if (ni.contains("components")) {
            SceneSerializer::RemapEntityRefsInComponents(ni["components"], remap, /*zeroExternal=*/true);
            RebaseTags(ni["components"], f, newLocal, ownLevel, innerRoots);
        }
        out.push_back(std::move(ni));
    }
    return out;
}

// PrefabLink.localId を保った抽出 (Apply 用 — ベースの localId 対応を崩さない)。
// **入れ子 (M48c)**: 内側メンバの PrefabLink は内側ベースのドメインなので、外側ベースの
// fileId にそのまま使うと外側メンバと衝突する (1 と 1、2 と 2 …)。外側での ID は
//   - 自レベル      : PrefabLink.localId
//   - 入れ子ルート  : PrefabInstance.outerLocalId
//   - 入れ子の配下  : 外側 ID を持たないので DFS 順に「最小の空き番号」を割り当てる
// で決める。最後の規則は ExtractLocal の連番と一致するため、構造が変わっていなければ
// Apply しても内側配下の ID は据え置きになる (ファイル diff が暴れない)。
// **oldBase の「照合キーになりうる ID」(自レベル + 入れ子ルート) は空きでも再利用しない** —
// このインスタンスで削除されたメンバの番号を内側配下に配ると、同じプレハブの別インスタンスが
// まだその番号を名乗っているため、直後の PropagateBaseChange が localId 照合だけで
// 別エンティティのベース値を書き込み、名前ごと化けさせてしまう
static json ExtractLocalByLinks(Scene& scene, EntityID root, const json& oldBase)
{
    World& w = scene.GetWorld();
    const json sub = SceneSerializer::SubtreeToJson(scene, root);
    json out = json::array();
    if (!sub.is_array() || sub.empty()) {
        return out;
    }
    std::unordered_set<uint64_t> ownLevel;
    std::unordered_set<uint64_t> innerRoots;
    SplitLevels(w, root, ownLevel, innerRoots);

    std::unordered_map<uint64_t, uint64_t> remap; // scene fileId -> 外側ベースの localId
    std::unordered_set<uint64_t> skipped;         // 新ベースに含めないサブツリー
    std::unordered_set<uint64_t> used;            // このインスタンスが実際に名乗った localId
    std::vector<uint64_t> pending;                // 外側 ID を持たないもの (DFS 順)
    for (const json& item : sub) {
        const uint64_t f = item.value("fileId", 0ull);
        if (f == 0) {
            continue;
        }
        const uint64_t p = item.value("parent", 0ull);
        if (p != 0 && skipped.count(p) != 0) {
            skipped.insert(f); // 除外した親のサブツリーごと落とす (木を分断しない)
            continue;
        }
        GameObject g = scene.FindByFileId(f);
        if (!g) {
            skipped.insert(f);
            continue;
        }
        // 既に埋まっている番号は名乗らせない。localId の重複は本来起きないが、他インスタンスの
        // メンバをここへ D&D した等で起きうる。素通しすると **fileId が重複したベース** が
        // 書き出され、FindLocal が別エンティティを解決して静かに壊れる
        uint64_t claimed = 0;
        if (ownLevel.count(f) != 0) {
            auto* link = w.GetComponent<PrefabLinkComponent>(g.Id());
            if (!link) {
                skipped.insert(f); // ユーザーが追加した非プレハブの子は新ベースに含めない
                continue;
            }
            claimed = link->localId;
        } else if (innerRoots.count(f) != 0) {
            auto* inst = w.GetComponent<PrefabInstanceComponent>(g.Id());
            claimed = inst ? inst->outerLocalId : 0;
            if (claimed == 0) {
                // ベースに対応の無い入れ子 = エディタで後から差し込んだインスタンス。
                // タグ無しのユーザー追加子と同じ扱いで新ベースに含めない
                // (v1 は構造変更の上書き非対応。included にすると片方だけ特別扱いになる)
                skipped.insert(f);
                continue;
            }
        }
        // claimed==0 = 内側インスタンスの配下 (外側 ID を持たない) → 後で採番
        if (claimed != 0 && used.insert(claimed).second) {
            remap[f] = claimed;
        } else {
            pending.push_back(f);
        }
    }
    std::unordered_set<uint64_t> reserved = used;
    {
        const BaseLevels oldLevels = ClassifyBase(oldBase);
        for (uint64_t id : oldLevels.ownLevel) {
            reserved.insert(id);
        }
        for (uint64_t id : oldLevels.innerRoots) {
            reserved.insert(id);
        }
    }
    uint64_t nextFree = 1;
    for (uint64_t f : pending) {
        while (reserved.count(nextFree) != 0) {
            ++nextFree;
        }
        remap[f] = nextFree;
        reserved.insert(nextFree);
    }

    for (const json& item : sub) {
        const uint64_t f = item.value("fileId", 0ull);
        auto it = remap.find(f);
        if (it == remap.end()) {
            continue;
        }
        json ni = item;
        ni["fileId"] = it->second;
        if (ni.contains("parent")) {
            const uint64_t p = ni.value("parent", 0ull);
            if (auto pit = remap.find(p); pit != remap.end()) {
                ni["parent"] = pit->second;
            } else {
                ni.erase("parent");
            }
        }
        if (ni.contains("components")) {
            SceneSerializer::RemapEntityRefsInComponents(ni["components"], remap, /*zeroExternal=*/true);
            RebaseTags(ni["components"], f, it->second, ownLevel, innerRoots);
        }
        out.push_back(std::move(ni));
    }
    return out;
}

uint64_t InstantiateEntities(Scene& scene, const json& localEntities, uint64_t prefabHash,
                             uint64_t parentFileId, uint64_t forcedRootFileId)
{
    if (!localEntities.is_array() || localEntities.empty()) {
        return 0;
    }
    // ルート (集合内に親を持たない最後のエンティティ — 下の走査と同じ規則) を先に特定する。
    // forcedRootFileId (v7 Instantiate の予約 ID、M37) をルートに割り当てるため
    uint64_t forcedRootLocal = 0;
    if (forcedRootFileId != 0) {
        std::unordered_set<uint64_t> localIds;
        for (const json& item : localEntities) {
            const uint64_t local = item.value("fileId", 0ull);
            if (local != 0) {
                localIds.insert(local);
            }
        }
        for (const json& item : localEntities) {
            const uint64_t local = item.value("fileId", 0ull);
            if (local == 0) {
                continue;
            }
            const uint64_t p = item.contains("parent") ? item.value("parent", 0ull) : 0ull;
            if (p == 0 || localIds.count(p) == 0) {
                forcedRootLocal = local;
            }
        }
    }
    // localId -> 新 scene fileId (ルートは予約 ID、他は新規採番)
    std::unordered_map<uint64_t, uint64_t> remap;
    for (const json& item : localEntities) {
        const uint64_t local = item.value("fileId", 0ull);
        if (local != 0) {
            remap[local] = (forcedRootFileId != 0 && local == forcedRootLocal)
                ? forcedRootFileId
                : scene.NextFileId();
        }
    }
    uint64_t rootLocal = 0;
    json out = json::array();
    for (const json& item : localEntities) {
        json ni = item;
        const uint64_t local = item.value("fileId", 0ull);
        if (auto it = remap.find(local); it != remap.end()) {
            ni["fileId"] = it->second;
        }
        bool hasParent = false;
        if (ni.contains("parent")) {
            const uint64_t p = ni.value("parent", 0ull);
            if (auto it = remap.find(p); it != remap.end()) {
                ni["parent"] = it->second;
                hasParent = true;
            } else {
                ni.erase("parent");
            }
        }
        if (!hasParent) {
            rootLocal = local; // プレハブルート (集合内に親を持たない)
            if (parentFileId != 0) {
                ni["parent"] = parentFileId;
            }
        }
        if (ni.contains("components")) {
            SceneSerializer::RemapEntityRefsInComponents(ni["components"], remap, /*zeroExternal=*/true);
        }
        out.push_back(std::move(ni));
    }

    SceneSerializer::ApplyPartial(scene, out);
    scene.GetWorld().ApplyStructuralChanges();

    // 入れ子インスタンスの持ち物には外側のタグを付けない (M48c)。その localId は内側ベースの
    // ドメインなので外側の連番で上書きしてはいけない。ApplyPartial が JSON からタグごと
    // 復元済みなので、ここでは触らないだけでよい (再帰は不要)。
    // 境界判定は **ベース JSON の PrefabInstance の位置** で行う — 「PrefabLink を持つか」で
    // 代用すると、内側の配下にユーザーが足したタグ無しの子に外側ドメインの localId が付き、
    // FindInstanceRoot が返す内側ベースで解決されて別エンティティに化ける
    const BaseLevels levels = ClassifyBase(localEntities);

    // タグ付け (localId 昇順で決定論的に処理。順序はハッシュに影響しないが規約に合わせる)
    std::vector<std::pair<uint64_t, uint64_t>> pairs; // (localId, newFid)
    pairs.reserve(remap.size());
    for (const auto& [local, fid] : remap) {
        pairs.emplace_back(local, fid);
    }
    std::sort(pairs.begin(), pairs.end());
    const uint64_t newRootFid = remap.count(rootLocal) ? remap[rootLocal] : 0;
    for (const auto& [local, fid] : pairs) {
        GameObject g = scene.FindByFileId(fid);
        if (!g) {
            continue;
        }
        if (local == rootLocal) {
            // ルートは分類がどうであれ必ず外側として付け直す (手書きアセットへの保険)。
            // **PrefabInstance のフィールドを先に書く** — 続く AddComponent が
            // アーキタイプを動かしてポインタを無効化するため
            auto* inst = g.AddComponent<PrefabInstanceComponent>();
            inst->prefabHash = prefabHash;
            inst->outerLocalId = 0; // 単体配置。入れ子は展開済みなので再帰インスタンス化しない
            g.AddComponent<PrefabLinkComponent>()->localId = local;
        } else if (levels.ownLevel.count(local) != 0) {
            g.AddComponent<PrefabLinkComponent>()->localId = local;
        }
    }
    scene.GetWorld().ApplyStructuralChanges();
    return newRootFid;
}

uint64_t Instantiate(Scene& scene, const PrefabLibrary& lib, uint64_t prefabHash,
                     uint64_t parentFileId, uint64_t forcedRootFileId)
{
    const PrefabAsset* a = lib.Get(prefabHash);
    if (!a) {
        MYE_LOG_WARN("prefab instantiate: unknown hash %llu",
                     static_cast<unsigned long long>(prefabHash));
        return 0;
    }
    return InstantiateEntities(scene, a->entities, prefabHash, parentFileId, forcedRootFileId);
}

uint64_t CreateAsset(Scene& scene, PrefabLibrary& lib, const std::wstring& path, EntityID root)
{
    const json entities = ExtractLocal(scene, root);
    if (entities.empty()) {
        return 0;
    }
    const std::string name = NameFromPath(path);
    if (!WritePrefabFile(path, name, entities)) {
        MYE_LOG_ERROR("prefab create: cannot write %s", WideToUtf8(path).c_str());
        return 0;
    }
    const uint64_t hash = lib.Register(path, name, entities);

    // 既存の root サブツリーをこのプレハブのインスタンスとしてタグ付けする
    // (localId は ExtractLocal と同じ DFS 順で 1..N = ベースと一致)。
    // **入れ子 (M48c)**: 内側インスタンスには外側の PrefabLink を付けない — 内側メンバの
    // localId は内側ベースのドメインだから。内側ルートには外側での位置だけを記録する
    World& w = scene.GetWorld();
    const json sub = SceneSerializer::SubtreeToJson(scene, root);
    std::unordered_set<uint64_t> ownLevel;
    std::unordered_set<uint64_t> innerRoots;
    SplitLevels(w, root, ownLevel, innerRoots);
    uint64_t local = 0;
    for (const json& item : sub) {
        const uint64_t f = item.value("fileId", 0ull);
        if (f == 0) {
            continue;
        }
        ++local; // ExtractLocal の連番と同じ規則 (fileId 付きの item を DFS 順に 1..N)
        GameObject g = scene.FindByFileId(f);
        if (!g) {
            continue;
        }
        if (ownLevel.count(f) != 0) {
            if (local == 1) {
                auto* inst = g.AddComponent<PrefabInstanceComponent>();
                inst->prefabHash = hash; // outerLocalId は据え置き (root 自身が入れ子なら有効値)
            }
            g.AddComponent<PrefabLinkComponent>()->localId = local;
        } else if (innerRoots.count(f) != 0) {
            if (auto* inst = w.GetComponent<PrefabInstanceComponent>(g.Id())) {
                inst->outerLocalId = local;
            }
        }
    }
    w.ApplyStructuralChanges();
    MYE_LOG_INFO("prefab created: %s (%zu entities)", WideToUtf8(path).c_str(), entities.size());
    return hash;
}

bool IsFieldOverridden(Scene& scene, const PrefabLibrary& lib, EntityID e, const char* compName,
                       const FieldDesc& field)
{
    if (field.type == FieldType::EntityRef) {
        return false; // remap 判定が不確実なため対象外
    }
    World& w = scene.GetWorld();
    const json* base = ResolveBase(w, lib, e);
    if (!base) {
        return false;
    }
    const json comps = base->contains("components") ? (*base)["components"] : json::object();
    if (!comps.contains(compName)) {
        return true; // インスタンスで追加されたコンポーネント
    }
    const json& bc = comps[compName];
    if (!bc.contains(field.name)) {
        return true;
    }
    const void* comp = w.GetComponentRaw(e, ComponentRegistry::Get().FindByName(compName));
    if (!comp) {
        return false;
    }
    return FieldToJson(comp, field) != bc[field.name];
}

bool IsNameOverridden(Scene& scene, const PrefabLibrary& lib, EntityID e)
{
    World& w = scene.GetWorld();
    const json* base = ResolveBase(w, lib, e);
    if (!base) {
        return false;
    }
    return GetNameStr(w, e) != base->value("name", std::string());
}

void RevertField(Scene& scene, const PrefabLibrary& lib, EntityID e, const char* compName,
                 const FieldDesc& field)
{
    if (field.type == FieldType::EntityRef) {
        return;
    }
    World& w = scene.GetWorld();
    const json* base = ResolveBase(w, lib, e);
    if (!base || !base->contains("components")) {
        return;
    }
    const json& comps = (*base)["components"];
    if (!comps.contains(compName) || !comps[compName].contains(field.name)) {
        return;
    }
    void* comp = w.GetComponentRaw(e, ComponentRegistry::Get().FindByName(compName));
    if (comp) {
        FieldFromJson(comp, field, comps[compName][field.name]);
    }
}

void RevertInstance(Scene& scene, const PrefabLibrary& lib, uint64_t rootFileId)
{
    World& w = scene.GetWorld();
    GameObject rootGo = scene.FindByFileId(rootFileId);
    if (!rootGo) {
        return;
    }
    auto* inst = w.GetComponent<PrefabInstanceComponent>(rootGo.Id());
    if (!inst) {
        return;
    }
    const PrefabAsset* a = lib.Get(inst->prefabHash);
    if (!a) {
        return;
    }
    std::vector<EntityID> members;
    CollectInstanceMembers(w, rootGo.Id(), members);
    const ComponentRegistry& reg = ComponentRegistry::Get();
    for (EntityID m : members) {
        auto* link = w.GetComponent<PrefabLinkComponent>(m);
        if (!link) {
            continue;
        }
        const json* base = FindOwnLevelLocal(a->entities, link->localId);
        if (!base) {
            continue;
        }
        SetEntityName(w, m, base->value("name", std::string()));
        if (!base->contains("components")) {
            continue;
        }
        for (const auto& [compName, fields] : (*base)["components"].items()) {
            const ComponentTypeId t = reg.FindByName(compName);
            if (t == kInvalidComponentType) {
                continue;
            }
            if ((reg.Desc(t).flags & kComponentHidden) != 0) {
                continue; // プレハブタグ自体はデータではない — Revert/伝播の対象外 (M48c)
            }
            void* comp = w.GetComponentRaw(m, t);
            if (!comp) {
                continue; // インスタンスに無いコンポーネントは足さない (フィールドのみ復元)
            }
            for (const FieldDesc& f : reg.Desc(t).fields) {
                if (f.type == FieldType::EntityRef || (f.flags & kFieldNoSerialize)) {
                    continue;
                }
                if (fields.contains(f.name)) {
                    FieldFromJson(comp, f, fields[f.name]);
                }
            }
        }
    }
    w.ApplyStructuralChanges();
}

void PropagateBaseChange(Scene& scene, const json& oldBase, const json& newBase, uint64_t prefabHash)
{
    World& w = scene.GetWorld();
    const ComponentRegistry& reg = ComponentRegistry::Get();

    // 対象プレハブの全インスタンスルートを収集
    std::vector<EntityID> roots;
    {
        const ComponentTypeId req[] = { PrefabInstanceComponent::sTypeId };
        w.ForEachArchetype(req, [&](Archetype& arch) {
            const int fi = arch.FindTypeIndex(PrefabInstanceComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                if (static_cast<const PrefabInstanceComponent*>(arch.GetPtr(fi, row))->prefabHash
                    == prefabHash) {
                    roots.push_back(arch.EntityAt(row));
                }
            }
        });
    }

    for (EntityID root : roots) {
        std::vector<EntityID> members;
        CollectInstanceMembers(w, root, members);
        for (EntityID m : members) {
            auto* link = w.GetComponent<PrefabLinkComponent>(m);
            if (!link) {
                continue;
            }
            // **自レベルのエントリしか当てない** (M48c)。番号が使い回されて内側配下の
            // エントリに当たった場合、そのまま伝播すると別エンティティに化ける
            const json* ob = FindOwnLevelLocal(oldBase, link->localId);
            const json* nb = FindOwnLevelLocal(newBase, link->localId);
            if (!nb) {
                continue; // 新ベースに存在しない → 触らない
            }
            // 名前 (非オーバーライドのみ伝播)
            {
                const std::string oldName = ob ? ob->value("name", std::string()) : std::string();
                const std::string newName = nb->value("name", std::string());
                if (oldName != newName && GetNameStr(w, m) == oldName) {
                    SetEntityName(w, m, newName);
                }
            }
            if (!nb->contains("components")) {
                continue;
            }
            const json obComps = (ob && ob->contains("components")) ? (*ob)["components"]
                                                                    : json::object();
            for (const auto& [compName, nfields] : (*nb)["components"].items()) {
                const ComponentTypeId t = reg.FindByName(compName);
                if (t == kInvalidComponentType) {
                    continue;
                }
                if ((reg.Desc(t).flags & kComponentHidden) != 0) {
                    continue; // プレハブタグを「普通のコンポーネント」として生やさない (M48c)
                }
                void* comp = w.GetComponentRaw(m, t);
                bool added = false;
                if (!comp) {
                    comp = w.AddComponentRaw(m, t); // 新規にベースへ加わったコンポーネント
                    added = true;
                }
                if (!comp) {
                    continue;
                }
                const json obFields = obComps.contains(compName) ? obComps[compName] : json::object();
                for (const FieldDesc& f : reg.Desc(t).fields) {
                    if (f.type == FieldType::EntityRef || (f.flags & kFieldNoSerialize)) {
                        continue;
                    }
                    if (!nfields.contains(f.name)) {
                        continue;
                    }
                    const json& newVal = nfields[f.name];
                    const bool hadOld = obFields.contains(f.name);
                    if (added) {
                        FieldFromJson(comp, f, newVal); // 新規コンポーネント → ベース値で埋める
                        continue;
                    }
                    if (hadOld && obFields[f.name] == newVal) {
                        continue; // ベース側で変化なし → 触らない
                    }
                    // 非オーバーライド (現在値が旧ベースと一致、または旧ベースに無い) のみ伝播
                    const json cur = FieldToJson(comp, f);
                    if (!hadOld || cur == obFields[f.name]) {
                        FieldFromJson(comp, f, newVal);
                    }
                }
            }
        }
    }
    w.ApplyStructuralChanges();
}

bool ApplyInstance(Scene& scene, PrefabLibrary& lib, uint64_t rootFileId)
{
    World& w = scene.GetWorld();
    GameObject rootGo = scene.FindByFileId(rootFileId);
    if (!rootGo) {
        return false;
    }
    auto* inst = w.GetComponent<PrefabInstanceComponent>(rootGo.Id());
    if (!inst) {
        return false;
    }
    const PrefabAsset* a = lib.Get(inst->prefabHash);
    if (!a) {
        return false;
    }
    const uint64_t hash = inst->prefabHash;
    const json oldBase = a->entities; // 伝播用にコピー
    const std::wstring path = a->path;
    const std::string name = a->name;

    json newBase = ExtractLocalByLinks(scene, rootGo.Id(), oldBase);
    if (newBase.empty()) {
        return false;
    }
    // ★インスタンスルートの名前は Apply でベースへ書き戻さない (M48b)。
    //   兄弟名の一意化で 2 個目の配置が "Enemy (1)" になるため、そのまま焼くと
    //   (1) .prefab.json のルート名が "Enemy (1)" に汚染され、
    //   (2) 直後の PropagateBaseChange が「旧名 Enemy のまま」の 1 個目まで "Enemy (1)" に改名し、
    //       兄弟が両方 "Enemy (1)" になって一意性が壊れる。
    //   さらに次の配置は "Enemy (1) (1)" と際限なく育つ。Unity も同じ理由でインスタンスの
    //   ルート名を Apply の対象外にしている。ルート以外のメンバ名は従来どおり Apply する
    if (const json* oldRoot = FindLocal(oldBase, 1); oldRoot && oldRoot->contains("name")) {
        for (json& item : newBase) {
            if (item.value("fileId", 0ull) == 1ull) {
                item["name"] = (*oldRoot)["name"];
                break;
            }
        }
    }
    if (!WritePrefabFile(path, name, newBase)) {
        MYE_LOG_WARN("prefab apply: file write failed for %s", WideToUtf8(path).c_str());
    }
    lib.Register(path, name, newBase); // library 更新 (hash 不変)
    PropagateBaseChange(scene, oldBase, newBase, hash); // 他インスタンスへ伝播
    MYE_LOG_INFO("prefab apply: '%s' base updated + propagated", name.c_str());
    return true;
}

} // namespace Prefab
} // namespace mye
