#include "Engine/Engine/Physics/PhysMatLibrary.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

#include "Engine/Core/AssetKeyResolver.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace fs = std::filesystem;

namespace mye {

using nlohmann::json;

namespace {

std::string NameFromPath(const std::wstring& path)
{
    std::string name = WideToUtf8(fs::path(path).stem().wstring()); // "X.physmat.json" → "X.physmat"
    const std::string suf = ".physmat";
    if (name.size() > suf.size() && name.compare(name.size() - suf.size(), suf.size(), suf) == 0) {
        name.resize(name.size() - suf.size());
    }
    return name;
}

float ReadFloat(const json& j, const char* key, float def)
{
    if (!j.contains(key) || !j[key].is_number()) {
        return def;
    }
    return j[key].get<float>();
}

// 1 値ぶんのサニタイズ: 非有限 (NaN/inf) は既定値へ、有限は [lo, hi] へクランプ。
// 「NaN は 0 でなく既定値」なのは意図的 — density の NaN を 0 に落とすと
// M59a2 の質量導出 (ρ×体積) がゼロ除算相当になり、別の壊れ方に化けるだけになる
float SanitizeValue(float v, float def, float lo, float hi)
{
    if (!std::isfinite(v)) {
        return def;
    }
    return std::clamp(v, lo, hi);
}

} // namespace

// ==== PhysMatLibrary ====

uint64_t PhysMatLibrary::HashForPath(const std::wstring& path)
{
    // M30c: 移動/リネーム済みアセットは .meta の GUID がキーになる (未移動は path-hash と同値)。
    // ★path-hash は **正規化した絶対パス**のハッシュ — これを AssetRef で参照するシーン JSON
    //   はチェックアウト先に依存するのでコミット不可 (物理デモは DemoContent 正本方式で回避)
    return assetkey::Resolve(NormalizePathKey(path));
}

uint64_t PhysMatLibrary::Register(const std::wstring& path, PhysMat mat)
{
    const uint64_t hash = HashForPath(path);
    mat.hash = hash;
    mat.path = path;
    if (mat.name.empty()) {
        mat.name = NameFromPath(path);
    }
    mats_[hash] = std::move(mat);
    return hash;
}

uint64_t PhysMatLibrary::LoadFromFile(const std::wstring& path)
{
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) {
        return 0;
    }
    json j;
    try {
        f >> j;
    } catch (const json::exception&) {
        MYE_LOG_WARN("[physmat] JSON parse failed: %s", WideToUtf8(path).c_str());
        return 0;
    }
    PhysMat m;
    if (!FromJson(j, m)) {
        MYE_LOG_WARN("[physmat] not a physics material: %s", WideToUtf8(path).c_str());
        return 0;
    }
    m.name = NameFromPath(path);
    return Register(path, std::move(m));
}

const PhysMat* PhysMatLibrary::Get(uint64_t hash) const
{
    const auto it = mats_.find(hash);
    return it != mats_.end() ? &it->second : nullptr;
}

std::vector<PhysMatEntry> PhysMatLibrary::Enumerate() const
{
    std::vector<PhysMatEntry> out;
    out.reserve(mats_.size());
    for (auto it = mats_.begin(); it != mats_.end(); ++it) {
        out.push_back(PhysMatEntry{ it->first, it->second.name });
    }
    // 明示キーで並べる (spec 11.2 規則 7: ハッシュの反復順を表に出さない)
    std::sort(out.begin(), out.end(), [](const PhysMatEntry& a, const PhysMatEntry& b) {
        if (a.name != b.name) {
            return a.name < b.name;
        }
        return a.hash < b.hash;
    });
    return out;
}

json PhysMatLibrary::ToJson(const PhysMat& m)
{
    json j;
    j["engine"] = "MyEngine";
    j["physmat"] = 1;
    j["name"] = m.name;
    j["density"] = m.density;
    j["staticFriction"] = m.staticFriction;
    j["dynamicFriction"] = m.dynamicFriction;
    j["restitution"] = m.restitution;
    j["rollingResistance"] = m.rollingResistance;
    j["dragCoefficient"] = m.dragCoefficient;
    j["adhesion"] = m.adhesion; // M60d
    // M65c: 音響。**書くのは常に 3 本とも** — 0 でも書いておかないと、エディタで
    // 保存した資産だけキーが欠けて「読み直すと無音に戻る」種類の事故になる
    j["acousticLoudness"] = m.acousticLoudness;
    j["acousticRadiusM"] = m.acousticRadiusM;
    j["acousticTone"] = m.acousticTone;
    return j;
}

bool PhysMatLibrary::FromJson(const json& j, PhysMat& out)
{
    if (!j.is_object() || !j.contains("physmat")) {
        return false; // 種別キー必須 (.mat.json 等の別資産を黙って材料に読まない)
    }
    if (j.contains("name") && j["name"].is_string()) {
        out.name = j["name"].get<std::string>();
    }
    const PhysMat def; // 既定値の一次情報は構造体の既定メンバ初期化子 1 箇所だけ
    out.density = ReadFloat(j, "density", def.density);
    out.staticFriction = ReadFloat(j, "staticFriction", def.staticFriction);
    out.dynamicFriction = ReadFloat(j, "dynamicFriction", def.dynamicFriction);
    out.restitution = ReadFloat(j, "restitution", def.restitution);
    out.rollingResistance = ReadFloat(j, "rollingResistance", def.rollingResistance);
    out.dragCoefficient = ReadFloat(j, "dragCoefficient", def.dragCoefficient);
    out.adhesion = ReadFloat(j, "adhesion", def.adhesion); // M60d (旧ファイルは 0)
    // M65c (旧ファイルは 3 本とも既定 = 無音)。tone だけ整数なので ReadFloat を通さない —
    // 通すと 0..3 の意味が float の丸めに乗ってしまう
    out.acousticLoudness = ReadFloat(j, "acousticLoudness", def.acousticLoudness);
    out.acousticRadiusM = ReadFloat(j, "acousticRadiusM", def.acousticRadiusM);
    if (j.contains("acousticTone") && j["acousticTone"].is_number_integer()) {
        out.acousticTone = j["acousticTone"].get<int32_t>();
    }
    Sanitize(out);
    return true;
}

void PhysMatLibrary::Sanitize(PhysMat& m)
{
    const PhysMat def;
    // density の下限 0.001 はゼロ除算 (質量導出 ρ×体積 → 1/m) の防波堤。
    // 上限はオスミウム (22590) の 100 倍程度あれば表現上困らない
    m.density = SanitizeValue(m.density, def.density, 0.001f, 1.0e6f);
    m.staticFriction = SanitizeValue(m.staticFriction, def.staticFriction, 0.0f, 100.0f);
    m.dynamicFriction = SanitizeValue(m.dynamicFriction, def.dynamicFriction, 0.0f, 100.0f);
    m.restitution = SanitizeValue(m.restitution, def.restitution, 0.0f, 1.0f);
    m.rollingResistance = SanitizeValue(m.rollingResistance, def.rollingResistance, 0.0f, 10.0f);
    m.dragCoefficient = SanitizeValue(m.dragCoefficient, def.dragCoefficient, 0.0f, 100.0f);
    // 粘着力は N。上限 1e6 は「1t の物体を 100G で保持できる」程度あれば表現上困らない
    m.adhesion = SanitizeValue(m.adhesion, def.adhesion, 0.0f, 1.0e6f);
    // M65c: 振幅の上限 100 / 半径の上限 1000m は「表現上困らない」以上の意味は無い。
    // ★重要なのは**下限 0** — 負の振幅は EnergyAt の単調減少を破り、負の半径は
    //   maxRing の切り捨てで 0 になって「鳴ったのに 1 リングも進まない波」を作る
    m.acousticLoudness = SanitizeValue(m.acousticLoudness, def.acousticLoudness, 0.0f, 100.0f);
    m.acousticRadiusM = SanitizeValue(m.acousticRadiusM, def.acousticRadiusM, 0.0f, 1000.0f);
    m.acousticTone = std::clamp(m.acousticTone, 0, 3);
}

// ==== physmat:: モジュール注入 ====

namespace physmat {

namespace {
PhysMatLibrary* sLibrary = nullptr;
} // namespace

void Install(PhysMatLibrary* lib)
{
    sLibrary = lib;
}

PhysMatLibrary* Library()
{
    return sLibrary;
}

const PhysMat* Resolve(AssetID id)
{
    if (sLibrary == nullptr || id.IsNull()) {
        return nullptr;
    }
    return sLibrary->Get(id.value);
}

} // namespace physmat

} // namespace mye
