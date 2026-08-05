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

// 末尾一致 (大文字小文字を無視)。Windows のパスなので ".PREFAB.JSON" も拾う
bool EndsWithNoCase(const std::wstring& s, const std::wstring& suffix)
{
    if (s.size() < suffix.size()) {
        return false;
    }
    size_t i = s.size() - suffix.size();
    for (size_t j = 0; j < suffix.size(); ++j, ++i) {
        if (towlower(s[i]) != towlower(suffix[j])) {
            return false;
        }
    }
    return true;
}

// ".actor.json" / ".prefab.json" / ".json" を落としたファイル名 (表示名)
std::string NameFromPath(const std::wstring& path)
{
    std::string name = WideToUtf8(fs::path(path).stem().wstring()); // "X.actor.json" -> "X.actor"
    for (const char* suf : { ".actor", ".prefab" }) {
        const size_t n = std::strlen(suf);
        if (name.size() > n && name.compare(name.size() - n, n, suf) == 0) {
            name.resize(name.size() - n); // -> "X"
            break;
        }
    }
    return name;
}

// entities 配列 (SubtreeToJson 形式) から fileId==localId の item を返す (無ければ nullptr)。
// **localId==0 は「ベースに対応物なし」の予約値** (複数ルートを包むラッパー、M48d) なので
// 必ず nullptr。これで diff / Revert / 伝播からラッパーが自然に外れる
const json* FindLocal(const json& entities, uint64_t localId)
{
    if (!entities.is_array() || localId == 0) {
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

bool WritePrefabFile(const std::wstring& path, const std::string& name, const json& entities,
                     bool actorFormat)
{
    json root;
    root["engine"] = "MyEngine";
    root[actorFormat ? "actor" : "prefab"] = 1; // 読み込んだときのキーを維持する (M48d)
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
//  - 自レベル: 外側のタグはベースの定義には要らない (インスタンス化時に付く) → 剥がす。
//    **override リストも剥がす** (M48e) — 「外側インスタンスでの上書き」であって、
//    その値がそのまま新しいベース定義になる以上、新ベースに対する上書きは 0 件
//  - 入れ子ルート: 内側の識別 (prefabHash / 内側 localId) は保ったまま、
//    外側ドメインでの位置 outerLocalId だけを新しいローカル fileId に付け替える。
//    override リストは内側ベースに対するものなので保持する
//  - 入れ子の配下: 触らない (localId は内側ベースのドメイン。外側は関知しない)
void RebaseTags(nlohmann::json& item, uint64_t sceneFid, uint64_t newLocalId,
                const std::unordered_set<uint64_t>& ownLevel,
                const std::unordered_set<uint64_t>& innerRoots)
{
    nlohmann::json& components = item["components"];
    if (ownLevel.count(sceneFid) != 0) {
        components.erase("PrefabInstance");
        components.erase("PrefabLink");
        item.erase("overrides");
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

// ---- override リスト (M48e) ----

// override リストのキー。名前だけは特別扱いで "name" (コンポーネントではないため)
constexpr const char* kNameOverrideKey = "name";

std::string OverrideKey(const std::string& compName, const char* fieldName)
{
    return compName + "." + fieldName;
}

// e の現在値を base (ベースのエンティティ JSON) と突き合わせた上書きキー集合 = ライブ diff。
// **ベースに存在するフィールドだけ**を対象にする — ロード時の更新はベース値の書き戻しなので、
// ベースに無いフィールド (インスタンスで足したコンポーネント等) は上書きとして記録しても
// 使い道がなく、リストを無駄に膨らませるだけ。
// EntityRef は remap 判定が不確実なので従来どおり対象外、隠しコンポーネント (プレハブタグ)
// はデータではないので対象外
std::set<std::string> OverridesAgainstBase(World& w, EntityID e, const json& base)
{
    std::set<std::string> keys;
    if (GetNameStr(w, e) != base.value("name", std::string())) {
        keys.insert(kNameOverrideKey);
    }
    if (!base.contains("components")) {
        return keys;
    }
    const ComponentRegistry& reg = ComponentRegistry::Get();
    for (const auto& [compName, fields] : base["components"].items()) {
        const ComponentTypeId t = reg.FindByName(compName);
        if (t == kInvalidComponentType || (reg.Desc(t).flags & kComponentHidden) != 0) {
            continue;
        }
        const void* comp = w.GetComponentRaw(e, t);
        if (!comp) {
            continue; // インスタンスから外されたコンポーネント (v1 では構造変更を追跡しない)
        }
        for (const FieldDesc& f : reg.Desc(t).fields) {
            if (f.type == FieldType::EntityRef || (f.flags & kFieldNoSerialize)) {
                continue;
            }
            if (!fields.contains(f.name)) {
                continue;
            }
            if (FieldToJson(comp, f) != fields[f.name]) {
                keys.insert(OverrideKey(compName, f.name));
            }
        }
    }
    return keys;
}

} // namespace

// ==== PrefabLibrary ====

bool PrefabLibrary::IsComposePath(const std::wstring& path)
{
    return EndsWithNoCase(path, kActorSuffix) || EndsWithNoCase(path, kPrefabSuffix);
}

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
    a.actorFormat = EndsWithNoCase(path, kActorSuffix); // 既定は拡張子。LoadFromFile が宣言キーで上書き
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
        MYE_LOG_WARN("compose asset parse failed: %s (%s)", WideToUtf8(path).c_str(), ex.what());
        return 0;
    }
    // 宣言キーで種別を検証する (M48d)。**actor:1 と prefab:1 の両方を受理** し、
    // どちらも無いものは弾く — 従来はキーを一切見ずに entities だけ拾っていたので、
    // 別種の .json (シーンやマテリアル) を渡しても「エンティティ 0 件のプレハブ」として
    // 静かに登録されてしまっていた
    const bool isActor = root.value("actor", 0) == 1;
    const bool isPrefab = root.value("prefab", 0) == 1;
    if (!isActor && !isPrefab) {
        MYE_LOG_WARN("compose asset load: neither \"actor\":1 nor \"prefab\":1 in %s",
                     WideToUtf8(path).c_str());
        return 0;
    }
    if (!root.contains("entities") || !root["entities"].is_array()) {
        MYE_LOG_WARN("compose asset load: no entities array in %s", WideToUtf8(path).c_str());
        return 0;
    }
    std::string name = root.value("name", NameFromPath(path));
    const uint64_t hash = Register(path, std::move(name), root["entities"]);
    SetActorFormat(hash, isActor); // 拡張子ではなく宣言キーが正 (書き戻しで維持)
    return hash;
}

void PrefabLibrary::SetActorFormat(uint64_t hash, bool actorFormat)
{
    if (auto it = assets_.find(hash); it != assets_.end()) {
        it->second.actorFormat = actorFormat;
    }
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
            RebaseTags(ni, f, newLocal, ownLevel, innerRoots);
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
            if (claimed == 0) {
                // 複数ルートを包むラッパー (M48d) — ベースに対応物が無いので新ベースにも出さない。
                // **skipped には入れない** — 子は残し、親解決に失敗して再びルートに戻す
                continue;
            }
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
            RebaseTags(ni, f, it->second, ownLevel, innerRoots);
        }
        out.push_back(std::move(ni));
    }
    return out;
}

uint64_t InstantiateEntities(Scene& scene, const json& localEntities, uint64_t prefabHash,
                             uint64_t parentFileId, uint64_t forcedRootFileId,
                             const char* wrapperName)
{
    if (!localEntities.is_array() || localEntities.empty()) {
        return 0;
    }
    // 集合内に親を持たないエントリ = ベースのルート。
    // **複数ある場合 (ミニシーン型の .actor.json) はラッパーで包む** (M48d) —
    // インスタンスの実体が単一ルートでないと、FindInstanceRoot / 選択 / Undo が
    // 「どれがルートか」で割れる。従来は最後のルートだけをタグ付けして残りが野良になっていた
    std::unordered_set<uint64_t> localIds;
    for (const json& item : localEntities) {
        const uint64_t local = item.value("fileId", 0ull);
        if (local != 0) {
            localIds.insert(local);
        }
    }
    std::vector<uint64_t> roots; // 配列順 (= ExtractLocal の DFS 順)
    for (const json& item : localEntities) {
        const uint64_t local = item.value("fileId", 0ull);
        if (local == 0) {
            continue;
        }
        const uint64_t p = item.value("parent", 0ull);
        if (p == 0 || localIds.count(p) == 0) {
            roots.push_back(local);
        }
    }
    if (roots.empty()) {
        MYE_LOG_WARN("compose asset instantiate: no root entity (cyclic parents?)");
        return 0;
    }
    const bool wrapped = roots.size() >= 2;
    const uint64_t rootLocal = wrapped ? 0 : roots.front();

    // ラッパーの fileId はメンバより先に確保する (単一ルート時にルートが最初の ID を取るのと同じ順)。
    // forcedRootFileId (v7 Instantiate の予約 ID、M37) はインスタンスのルートに割り当てる契約なので、
    // 包む場合はラッパーが受け取る
    const uint64_t wrapperFid =
        wrapped ? ((forcedRootFileId != 0) ? forcedRootFileId : scene.NextFileId()) : 0;
    const uint64_t forcedRootLocal = (!wrapped && forcedRootFileId != 0) ? rootLocal : 0;

    // localId -> 新 scene fileId (ルートは予約 ID、他は新規採番)
    std::unordered_map<uint64_t, uint64_t> remap;
    for (const json& item : localEntities) {
        const uint64_t local = item.value("fileId", 0ull);
        if (local != 0) {
            remap[local] = (forcedRootLocal != 0 && local == forcedRootLocal)
                ? forcedRootFileId
                : scene.NextFileId();
        }
    }
    json out = json::array();
    if (wrapped) {
        // ベースに対応物が無いので PrefabLink.localId は 0 (FindLocal(0) は必ず nullptr)。
        // components は空 = 生成時の既定 (LocalTransform 等) をそのまま使う
        json wrap;
        wrap["fileId"] = wrapperFid;
        wrap["name"] = (wrapperName && *wrapperName) ? std::string(wrapperName) : std::string("Actor");
        wrap["childIndex"] = 0;
        wrap["components"] = json::object();
        if (parentFileId != 0) {
            wrap["parent"] = parentFileId;
        }
        out.push_back(std::move(wrap));
    }
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
            if (wrapped) {
                ni["parent"] = wrapperFid; // ベースのルート群はラッパーの子になる
            } else if (parentFileId != 0) {
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
    const uint64_t newRootFid =
        wrapped ? wrapperFid : (remap.count(rootLocal) ? remap[rootLocal] : 0);
    if (wrapped) {
        if (GameObject g = scene.FindByFileId(wrapperFid)) {
            auto* inst = g.AddComponent<PrefabInstanceComponent>();
            inst->prefabHash = prefabHash;
            inst->outerLocalId = 0;
            g.AddComponent<PrefabLinkComponent>()->localId = 0; // ベース対応物なし
            scene.SetOverrides(wrapperFid, {}); // ラッパーはベース対応物なし = 上書きも無い
        }
    }
    for (const auto& [local, fid] : pairs) {
        GameObject g = scene.FindByFileId(fid);
        if (!g) {
            continue;
        }
        // 新規インスタンスは全メンバが「新形式・上書き無し」。ベース JSON が overrides キーを
        // 持っていた分 (入れ子インスタンスの上書き) は ApplyPartial の復元フックが既に
        // 積んでいるので、**記録が無いものだけ**空で埋める (M48e)
        if (!scene.HasOverrideRecord(fid)) {
            scene.SetOverrides(fid, {});
        }
        if (!wrapped && local == rootLocal) {
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
        MYE_LOG_WARN("compose asset instantiate: unknown hash %llu",
                     static_cast<unsigned long long>(prefabHash));
        return 0;
    }
    return InstantiateEntities(scene, a->entities, prefabHash, parentFileId, forcedRootFileId,
                               a->name.c_str()); // 複数ルートを包むときの名前 = アセット名
}

uint64_t CreateAsset(Scene& scene, PrefabLibrary& lib, const std::wstring& path, EntityID root)
{
    const json entities = ExtractLocal(scene, root);
    if (entities.empty()) {
        return 0;
    }
    const std::string name = NameFromPath(path);
    // 新規作成の宣言キーは拡張子に従う (.actor.json → actor:1)
    if (!WritePrefabFile(path, name, entities, EndsWithNoCase(path, PrefabLibrary::kActorSuffix))) {
        MYE_LOG_ERROR("compose asset create: cannot write %s", WideToUtf8(path).c_str());
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
    // 保存済み override リストが一次情報 (M48e)。記録が無いのはレガシーシーンだけなので、
    // そのときだけ従来のライブ diff に落ちる
    if (const Scene::OverrideSet* rec = scene.GetOverrides(FidOf(w, e))) {
        return rec->count(OverrideKey(compName, field.name)) != 0;
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
    if (const Scene::OverrideSet* rec = scene.GetOverrides(FidOf(w, e))) {
        return rec->count(kNameOverrideKey) != 0;
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
        scene.UnmarkOverride(FidOf(w, e), OverrideKey(compName, field.name)); // M48e
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
        // ベース値へ戻し切るので上書きは 0 件になる (M48e)。
        // **ベースに無いフィールドは元々リストに載らない**ので空集合で正しい
        scene.SetOverrides(FidOf(w, m), {});
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
            const json nbComps = nb->contains("components") ? (*nb)["components"] : json::object();
            const json obComps = (ob && ob->contains("components")) ? (*ob)["components"]
                                                                    : json::object();
            for (const auto& [compName, nfields] : nbComps.items()) {
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
            // 伝播が終わった今この瞬間だけ「ベース == 現行」なので、override リストを
            // ライブ diff で撮り直す (M48e)。ここを飛ばすと、Apply / ホットリロードで
            // ベースが動いたあとにリストが古い判定のまま残る
            scene.SetOverrides(FidOf(w, m), OverridesAgainstBase(w, m, *nb));
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
    const bool actorFormat = a->actorFormat; // 元の宣言キーを維持 (強制移行しない)

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
    if (!WritePrefabFile(path, name, newBase, actorFormat)) {
        MYE_LOG_WARN("compose asset apply: file write failed for %s", WideToUtf8(path).c_str());
    }
    lib.Register(path, name, newBase); // library 更新 (hash 不変)
    lib.SetActorFormat(hash, actorFormat); // Register は拡張子から推定するので宣言キーを戻す
    // 伝播対象には Apply 元のインスタンス自身も含まれる (同じ prefabHash) ので、
    // override リストの撮り直しもここで一緒に済む (M48e)
    PropagateBaseChange(scene, oldBase, newBase, hash); // 他インスタンスへ伝播
    MYE_LOG_INFO("prefab apply: '%s' base updated + propagated", name.c_str());
    return true;
}

// ==== override リストの記録 (M48e) ====

std::set<std::string> ComputeOverrides(Scene& scene, const PrefabLibrary& lib, EntityID e)
{
    World& w = scene.GetWorld();
    const json* base = ResolveBase(w, lib, e);
    return base ? OverridesAgainstBase(w, e, *base) : std::set<std::string>();
}

void RecordOverrides(Scene& scene, const PrefabLibrary& lib, EntityID e)
{
    World& w = scene.GetWorld();
    const uint64_t fid = FidOf(w, e);
    if (fid == 0) {
        return;
    }
    if (!w.GetComponent<PrefabLinkComponent>(e)) {
        scene.ClearOverrides(fid); // プレハブメンバでなくなった (タグ剥がし / 単なる非メンバ)
        return;
    }
    const json* base = ResolveBase(w, lib, e);
    if (!base) {
        // アセット未登録 / ベースに対応物なし (複数ルートのラッパー等)。
        // **記録は消さない** — アセットが戻ったときに上書きを失うのは復旧不能な破壊
        return;
    }
    scene.SetOverrides(fid, OverridesAgainstBase(w, e, *base));
}

void RecordOverridesSubtree(Scene& scene, const PrefabLibrary& lib, EntityID root)
{
    World& w = scene.GetWorld();
    std::function<void(EntityID)> visit = [&](EntityID e) {
        RecordOverrides(scene, lib, e);
        auto* h = w.GetComponent<HierarchyComponent>(e);
        EntityID c = h ? h->firstChild : kNullEntity;
        while (!c.IsNull()) {
            auto* ch = w.GetComponent<HierarchyComponent>(c);
            const EntityID next = ch ? ch->nextSibling : kNullEntity;
            visit(c);
            c = next;
        }
    };
    if (w.IsAlive(root)) {
        visit(root);
    }
}

void RefreshNonOverridden(Scene& scene, const PrefabLibrary& lib)
{
    World& w = scene.GetWorld();
    const ComponentRegistry& reg = ComponentRegistry::Get();

    // インスタンスルートを fileId 昇順で処理する。ForEachArchetype の走査順はアーキタイプの
    // 生成履歴に依存するため、**そのまま使うと決定論を落とす** (同じ JSON でも順序が変わりうる)
    std::vector<std::pair<uint64_t, EntityID>> roots;
    {
        const ComponentTypeId req[] = { PrefabInstanceComponent::sTypeId };
        w.ForEachArchetype(req, [&](Archetype& arch) {
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const EntityID e = arch.EntityAt(row);
                roots.emplace_back(FidOf(w, e), e);
            }
        });
    }
    std::sort(roots.begin(), roots.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    int refreshed = 0, migrated = 0;
    for (const auto& rootPair : roots) {
        const EntityID root = rootPair.second;
        auto* inst = w.GetComponent<PrefabInstanceComponent>(root);
        const PrefabAsset* a = inst ? lib.Get(inst->prefabHash) : nullptr;
        if (!a) {
            continue; // アセット欠落 — 値には触らない (欠落アセットで壊すより据え置きが安全)
        }
        std::vector<EntityID> members;
        CollectInstanceMembers(w, root, members); // 入れ子は境界で止まる (内側は内側のルートで処理)
        std::vector<std::pair<uint64_t, EntityID>> ordered; // localId 昇順
        ordered.reserve(members.size());
        for (EntityID m : members) {
            if (auto* link = w.GetComponent<PrefabLinkComponent>(m)) {
                ordered.emplace_back(link->localId, m);
            }
        }
        std::sort(ordered.begin(), ordered.end(),
                  [](const auto& x, const auto& y) { return x.first < y.first; });

        for (const auto& [localId, m] : ordered) {
            const json* base = FindOwnLevelLocal(a->entities, localId);
            if (!base) {
                continue; // ベースに対応物なし (ラッパー localId=0 / 削除されたメンバ)
            }
            const uint64_t fid = FidOf(w, m);
            if (fid == 0) {
                continue;
            }
            const Scene::OverrideSet* rec = scene.GetOverrides(fid);
            if (!rec) {
                // レガシー (M48d 以前に保存されたシーン)。ライブ diff から記録だけ起こし、
                // **値には一切触らない** = 旧エンジンで保存したシーンはビット不変にロードされる。
                // 次の保存で overrides キーが付き、以後は新形式として refresh の対象になる
                scene.SetOverrides(fid, OverridesAgainstBase(w, m, *base));
                ++migrated;
                continue;
            }
            const Scene::OverrideSet keys = *rec; // 以降の Set 呼び出しでポインタが失効しうる
            if (keys.count(kNameOverrideKey) == 0) {
                // ★MakeUniqueSiblingName は通さない — ロード経路で名前を変えると
                //   WorldHash が変わり既存シーンの決定論が壊れる (EntityNaming.h の規約)
                SetEntityName(w, m, base->value("name", std::string()));
            }
            if (!base->contains("components")) {
                continue;
            }
            for (const auto& [compName, fields] : (*base)["components"].items()) {
                const ComponentTypeId t = reg.FindByName(compName);
                if (t == kInvalidComponentType || (reg.Desc(t).flags & kComponentHidden) != 0) {
                    continue; // プレハブタグはデータではない (M48c)
                }
                void* comp = w.GetComponentRaw(m, t);
                if (!comp) {
                    continue; // v1 は構造変更を追跡しない — 無いコンポーネントを生やさない
                }
                for (const FieldDesc& f : reg.Desc(t).fields) {
                    if (f.type == FieldType::EntityRef || (f.flags & kFieldNoSerialize)) {
                        continue;
                    }
                    if (!fields.contains(f.name) || keys.count(OverrideKey(compName, f.name)) != 0) {
                        continue;
                    }
                    FieldFromJson(comp, f, fields[f.name]);
                }
            }
            ++refreshed;
        }
    }
    w.ApplyStructuralChanges();
    if (refreshed != 0 || migrated != 0) {
        MYE_LOG_INFO("[prefab] refreshed %d instance member(s) from base, migrated %d legacy",
                     refreshed, migrated);
    }
}

// ---- ミニシーン編集モード (M48k) ----

json MakeEditDocument(const PrefabAsset& asset)
{
    json doc;
    doc["engine"] = "MyEngine";
    doc["version"] = 2;
    doc["sceneName"] = asset.name;
    // ★ここが要点: LoadFromJson は nextFileId 既定を 1 にするので、そのままだと
    //   編集中に追加したエンティティが既存の localId (1..N) と衝突し、配置済み
    //   インスタンスの PrefabLink が別メンバを指すようになる
    uint64_t maxFid = 0;
    if (asset.entities.is_array()) {
        for (const json& item : asset.entities) {
            maxFid = std::max(maxFid, item.value("fileId", 0ull));
        }
    }
    doc["nextFileId"] = maxFid + 1;
    doc["entities"] = asset.entities.is_array() ? asset.entities : json::array();
    return doc;
}

bool SaveEdited(Scene& miniScene, PrefabLibrary& lib, const std::wstring& path,
                const std::string& name, bool actorFormat)
{
    const json doc = SceneSerializer::SaveToJson(miniScene);
    if (!doc.contains("entities") || !doc["entities"].is_array()) {
        return false;
    }
    const json& entities = doc["entities"];
    if (entities.empty()) {
        MYE_LOG_WARN("[prefab] refusing to save an empty asset: %s", WideToUtf8(path).c_str());
        return false; // 空で上書きすると配置済みインスタンスのベースが消える
    }
    if (!WritePrefabFile(path, name, entities, actorFormat)) {
        MYE_LOG_ERROR("[prefab] could not write %s", WideToUtf8(path).c_str());
        return false;
    }
    // ライブラリも即更新する (ファイル監視が拾う前に Inspector 等が古いベースを見ないように)。
    // 配置済みインスタンスへの伝播は ReloadHub の既存経路に任せる
    const uint64_t hash = lib.Register(path, name, entities);
    lib.SetActorFormat(hash, actorFormat);
    return hash != 0;
}

} // namespace Prefab
} // namespace mye
