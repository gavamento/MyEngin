#include "Engine/Engine/Physics/ConvexColliderLibrary.h"

#include <algorithm>
#include <cstring>

#include "Engine/Core/Log.h"
#include "Engine/Engine/Asset/CookedCache.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GpuResources.h"

using namespace DirectX;

namespace mye {
namespace {

// CookedCache に渡す拡張子 (.mmdl / .mpcm に続く第 3 の種)
constexpr const wchar_t* kConvexExt = L".mcvx";
constexpr uint32_t kTableVersion = 1;
constexpr uint32_t kTableSanityCap = 4096; // 1 モデルの凸包数の上限 (壊れた blob 対策)

void AppendPod32(std::vector<uint8_t>& buf, uint32_t v)
{
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&v);
    buf.insert(buf.end(), b, b + sizeof(v));
}

} // namespace

std::wstring ConvexCookSourcePath(const std::string& meshName)
{
    const size_t hash = meshName.find('#');
    if (hash == std::string::npos || hash == 0) {
        return {}; // "builtin://cube" など = クック対象外
    }
    return Utf8ToWide(meshName.substr(0, hash));
}

void SerializeConvexTable(const std::vector<std::pair<std::string, ConvexHullData>>& table,
                          std::vector<uint8_t>& out)
{
    AppendPod32(out, kTableVersion);
    AppendPod32(out, static_cast<uint32_t>(table.size()));
    for (const auto& [key, hull] : table) {
        AppendPod32(out, static_cast<uint32_t>(key.size()));
        out.insert(out.end(), key.begin(), key.end());
        SerializeConvexHull(hull, out);
    }
}

bool DeserializeConvexTable(const std::vector<uint8_t>& in,
                            std::vector<std::pair<std::string, ConvexHullData>>& out)
{
    out.clear();
    size_t pos = 0;
    auto readU32 = [&](uint32_t& v) {
        if (pos + sizeof(uint32_t) > in.size()) {
            return false;
        }
        std::memcpy(&v, in.data() + pos, sizeof(uint32_t));
        pos += sizeof(uint32_t);
        return true;
    };
    uint32_t version = 0, count = 0;
    if (!readU32(version) || version != kTableVersion || !readU32(count)
        || count > kTableSanityCap) {
        return false;
    }
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t keyLen = 0;
        if (!readU32(keyLen) || pos + keyLen > in.size()) {
            out.clear();
            return false;
        }
        std::string key(reinterpret_cast<const char*>(in.data() + pos), keyLen);
        pos += keyLen;
        ConvexHullData hull;
        if (!DeserializeConvexHull(in.data(), in.size(), pos, hull)) {
            out.clear();
            return false;
        }
        out.emplace_back(std::move(key), std::move(hull));
    }
    return true;
}

ConvexColliderLibrary::CookTable& ConvexColliderLibrary::LoadTable(const std::wstring& srcPath)
{
    auto it = tables_.find(srcPath);
    if (it != tables_.end()) {
        return it->second;
    }
    CookTable table;
    std::vector<uint8_t> payload;
    if (CookedCache::Enabled() && CookedCache::ReadValidated(srcPath, kConvexExt, payload)) {
        if (!DeserializeConvexTable(payload, table)) {
            // 壊れた blob は黙って捨てる (次の Save で作り直される)。ここで諦めると
            // 「一度壊れたキャッシュのせいで凸包が永久に出ない」になる
            MYE_LOG_WARN("[cook] .mcvx is corrupt - rebuilding hulls (%s)",
                         WideToUtf8(srcPath).c_str());
            table.clear();
        }
    }
    return tables_.emplace(srcPath, std::move(table)).first->second;
}

void ConvexColliderLibrary::SaveTable(const std::wstring& srcPath, const CookTable& table)
{
    // 封印キャッシュ (配布ビルド) には書き足さない — 配布物の cooked を実行時に書き換えると
    // 「同じ配布物なのに起動ごとに中身が違う」になるし、そもそも書けない環境がある
    if (!CookedCache::Enabled() || CookedCache::Sealed() || table.empty()) {
        return;
    }
    std::vector<uint8_t> payload;
    SerializeConvexTable(table, payload);
    CookedCache::Write(srcPath, kConvexExt, payload.data(), payload.size());
}

const ConvexHullData* ConvexColliderLibrary::Get(AssetID meshAsset)
{
    if (meshAsset.IsNull()) {
        return nullptr;
    }
    auto it = cache_.find(meshAsset.value);
    if (it != cache_.end()) {
        return it->second.get();
    }
    if (resources_ == nullptr) {
        return nullptr;
    }
    Mesh* mesh = resources_->meshes.Get(meshAsset);
    if (!mesh || mesh->positions.empty()) {
        return nullptr; // 未登録 / CPU 頂点なし。キャッシュしない (後からロードされ得る)
    }
    const std::string* name = resources_->meshes.NameOf(meshAsset);
    const std::wstring srcPath = name ? ConvexCookSourcePath(*name) : std::wstring{};

    auto hull = std::make_unique<ConvexHullData>();
    bool fromCook = false;
    if (!srcPath.empty()) {
        CookTable& table = LoadTable(srcPath);
        const auto hit = std::lower_bound(
            table.begin(), table.end(), *name,
            [](const std::pair<std::string, ConvexHullData>& e, const std::string& k) {
                return e.first < k;
            });
        if (hit != table.end() && hit->first == *name) {
            *hull = hit->second;
            fromCook = true;
        } else {
            // 生成してから表へ挿す。key 昇順を保つのは blob を決定的にするため
            BuildConvexHull(mesh->positions, *hull);
            table.emplace(hit, *name, *hull);
            SaveTable(srcPath, table);
        }
    } else {
        BuildConvexHull(mesh->positions, *hull);
    }
    if (!fromCook) {
        MYE_LOG_INFO("[phys] convex hull built: %zu verts / %zu faces (%s)", hull->verts.size(),
                     hull->faces.size(), name ? name->c_str() : "<unnamed>");
    }
    const ConvexHullData* raw = hull.get();
    cache_[meshAsset.value] = std::move(hull);
    return raw;
}

void ConvexColliderLibrary::Register(AssetID id, ConvexHullData data)
{
    if (id.IsNull()) {
        return;
    }
    cache_[id.value] = std::make_unique<ConvexHullData>(std::move(data));
}

void ConvexColliderLibrary::Clear()
{
    cache_.clear();
    tables_.clear();
}

namespace convexcol {
namespace {
ConvexColliderLibrary* g_lib = nullptr;
} // namespace

void Install(ConvexColliderLibrary* lib)
{
    g_lib = lib;
}

const ConvexHullData* Resolve(AssetID meshAsset)
{
    return g_lib ? g_lib->Get(meshAsset) : nullptr;
}

} // namespace convexcol
} // namespace mye
