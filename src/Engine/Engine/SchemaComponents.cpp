#include "Engine/Engine/SchemaComponents.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "Engine/Core/ComponentRegistry.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"

#include "nlohmann/json.hpp"

using json = nlohmann::json;

namespace mye::schema {
namespace {

// ---- 既定値ブロブと construct サムネイル -------------------------------------
//
// ComponentDesc::construct は `void(*)(void*)` の素の関数ポインタで、ユーザーデータを
// 渡す口が無い。スキーマは実行時に決まるので、**スロット番号をテンプレート引数に焼いた
// 関数を kMaxSchemaComponents 個生成**しておき、i 番目のスキーマに i 番目を配る。
std::vector<std::vector<uint8_t>>& Blobs()
{
    static std::vector<std::vector<uint8_t>> blobs;
    return blobs;
}

template <size_t I>
void SchemaCtor(void* dst)
{
    const std::vector<uint8_t>& b = Blobs()[I];
    std::memcpy(dst, b.data(), b.size());
}

using CtorFn = void (*)(void*);

template <size_t... Is>
std::array<CtorFn, sizeof...(Is)> MakeCtors(std::index_sequence<Is...>)
{
    return { &SchemaCtor<Is>... };
}

const std::array<CtorFn, kMaxSchemaComponents>& Ctors()
{
    static const std::array<CtorFn, kMaxSchemaComponents> table =
        MakeCtors(std::make_index_sequence<kMaxSchemaComponents>{});
    return table;
}

// 登録済みスキーマの安定 ID (重複検出用)。プロセス内で一意
std::vector<uint64_t>& UsedIds()
{
    static std::vector<uint64_t> ids;
    return ids;
}

// スキーマ由来で登録された TypeId の記録 (codegen が参照する)
std::vector<uint32_t>& RegisteredIds()
{
    static std::vector<uint32_t> ids;
    return ids;
}

// ---- 型マップ ---------------------------------------------------------------
// **FieldType は閉集合** (Reflection.h)。新しい型はここでも増やさない — スキーマで
// 表現できないものは「既存 15 種の組み合わせ」に落とす (固定長配列はフィールド展開)
struct TypeEntry {
    const char* name;
    FieldType type;
    uint32_t align;
};
constexpr TypeEntry kTypes[] = {
    { "Float", FieldType::Float, 4 },        { "Int32", FieldType::Int32, 4 },
    { "UInt32", FieldType::UInt32, 4 },      { "UInt64", FieldType::UInt64, 8 },
    { "Bool", FieldType::Bool, 1 },          { "Float2", FieldType::Float2, 4 },
    { "Float3", FieldType::Float3, 4 },      { "Float4", FieldType::Float4, 4 },
    { "Quat", FieldType::Quat, 4 },          { "Color", FieldType::Color, 4 },
    { "EntityRef", FieldType::EntityRef, 8 }, { "AssetRef", FieldType::AssetRef, 8 },
    { "String64", FieldType::String64, 1 },  { "Float4x4", FieldType::Float4x4, 4 },
    { "String256", FieldType::String256, 1 },
};

const TypeEntry* FindType(const std::string& name)
{
    for (const TypeEntry& e : kTypes) {
        if (name == e.name) {
            return &e;
        }
    }
    return nullptr;
}

// ---- 既定値の書き込み -------------------------------------------------------
// 型ごとに JSON の "default" を生バイトへ落とす。未指定/型違いはゼロ (= C++ 側の
// 既定コンストラクタと同じ「ゼロ初期化」) にする。Quat だけは w=1 が自然なので既定を持つ
void WriteDefault(uint8_t* dst, FieldType type, const json& v)
{
    auto num = [&](size_t i, float fallback) -> float {
        if (v.is_array() && i < v.size() && v[i].is_number()) {
            return v[i].get<float>();
        }
        return fallback;
    };
    auto str = [&](size_t cap) {
        if (v.is_string()) {
            const std::string s = v.get<std::string>();
            const size_t n = std::min(s.size(), cap - 1);
            std::memcpy(dst, s.data(), n); // 残りは呼び出し側でゼロ済み
        }
    };
    switch (type) {
    case FieldType::Float: {
        const float f = v.is_number() ? v.get<float>() : 0.0f;
        std::memcpy(dst, &f, sizeof(f));
        break;
    }
    case FieldType::Int32: {
        const int32_t i = v.is_number_integer() ? v.get<int32_t>() : 0;
        std::memcpy(dst, &i, sizeof(i));
        break;
    }
    case FieldType::UInt32: {
        const uint32_t u = v.is_number_integer() ? v.get<uint32_t>() : 0u;
        std::memcpy(dst, &u, sizeof(u));
        break;
    }
    case FieldType::UInt64:
    case FieldType::AssetRef: {
        const uint64_t u = v.is_number_integer() ? v.get<uint64_t>() : 0ull;
        std::memcpy(dst, &u, sizeof(u));
        break;
    }
    case FieldType::Bool:
        dst[0] = (v.is_boolean() && v.get<bool>()) ? 1 : 0;
        break;
    case FieldType::Float2: {
        const float f[2] = { num(0, 0.0f), num(1, 0.0f) };
        std::memcpy(dst, f, sizeof(f));
        break;
    }
    case FieldType::Float3: {
        const float f[3] = { num(0, 0.0f), num(1, 0.0f), num(2, 0.0f) };
        std::memcpy(dst, f, sizeof(f));
        break;
    }
    case FieldType::Float4: {
        const float f[4] = { num(0, 0.0f), num(1, 0.0f), num(2, 0.0f), num(3, 0.0f) };
        std::memcpy(dst, f, sizeof(f));
        break;
    }
    case FieldType::Quat: {
        const float f[4] = { num(0, 0.0f), num(1, 0.0f), num(2, 0.0f), num(3, 1.0f) };
        std::memcpy(dst, f, sizeof(f));
        break;
    }
    case FieldType::Color: {
        const float f[4] = { num(0, 1.0f), num(1, 1.0f), num(2, 1.0f), num(3, 1.0f) };
        std::memcpy(dst, f, sizeof(f));
        break;
    }
    case FieldType::EntityRef: {
        // 参照はスキーマからは張れない (エンティティはシーン側の存在) — 常に null
        const uint32_t nullId[2] = { 0xFFFFFFFFu, 0u };
        std::memcpy(dst, nullId, sizeof(nullId));
        break;
    }
    case FieldType::String64: str(64); break;
    case FieldType::String256: str(256); break;
    case FieldType::Float4x4: {
        float m[16] = {};
        for (int i = 0; i < 16; ++i) {
            m[i] = num(static_cast<size_t>(i), (i % 5 == 0) ? 1.0f : 0.0f); // 既定は単位行列
        }
        std::memcpy(dst, m, sizeof(m));
        break;
    }
    }
}

// ---- 1 ファイル分の登録 -----------------------------------------------------
// 失敗はすべて「この 1 件を捨てて次へ」— 1 個の壊れたスキーマで起動を止めない
bool RegisterOne(const std::filesystem::path& path)
{
    const std::string pathU = WideToUtf8(path.wstring());
    std::ifstream f(path);
    if (!f) {
        MYE_LOG_WARN("[schema] cannot open %s", pathU.c_str());
        return false;
    }
    json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        MYE_LOG_ERROR("[schema] %s: parse failed (%s)", pathU.c_str(), e.what());
        return false;
    }
    if (!j.value("componentSchema", 0)) {
        MYE_LOG_WARN("[schema] %s: not a component schema (\"componentSchema\": 1 required)",
                     pathU.c_str());
        return false;
    }
    const std::string name = j.value("name", std::string());
    if (name.empty()) {
        MYE_LOG_ERROR("[schema] %s: \"name\" is required", pathU.c_str());
        return false;
    }
    // 安定 ID: v1 では一意性の検証と将来のバイナリ形式向けの予約のみ (実行時の同一性は
    // 家風どおり nameHash)。0 は「未指定」と区別できないので禁止
    const uint64_t stableId = j.value("id", 0ull);
    if (stableId == 0) {
        MYE_LOG_ERROR("[schema] %s: a non-zero \"id\" is required", pathU.c_str());
        return false;
    }
    if (std::find(UsedIds().begin(), UsedIds().end(), stableId) != UsedIds().end()) {
        MYE_LOG_ERROR("[schema] %s: duplicate id %llu", pathU.c_str(),
                      static_cast<unsigned long long>(stableId));
        return false;
    }
    // ★同名の型が既にあるなら**絶対に登録しない**。ComponentRegistry::Register は
    //   nameHash 一致で既存 TypeId を返すだけなので、素通しするとサイズもフィールドも
    //   違う別物 (組込みや C++ スクリプト) にスキーマ名が乗っ取られる
    if (ComponentRegistry::Get().FindByName(name) != kInvalidComponentType) {
        MYE_LOG_WARN("[schema] %s: component '%s' is already registered - skipped", pathU.c_str(),
                     name.c_str());
        return false;
    }
    const json* fields = j.contains("fields") && j["fields"].is_array() ? &j["fields"] : nullptr;
    if (!fields || fields->empty()) {
        MYE_LOG_ERROR("[schema] %s: \"fields\" must be a non-empty array", pathU.c_str());
        return false;
    }
    if (Blobs().size() >= kMaxSchemaComponents) {
        MYE_LOG_ERROR("[schema] %s: too many schema components (max %zu)", pathU.c_str(),
                      kMaxSchemaComponents);
        return false;
    }

    // ---- レイアウトを組む (宣言順・自然アラインメント) ----
    struct Built {
        std::string name;
        std::string display;
        std::string tooltip;
        FieldType type = FieldType::Float;
        uint32_t offset = 0;
        float dragSpeed = 0.0f;
        float minVal = 0.0f;
        float maxVal = 0.0f;
        const json* def = nullptr;
    };
    std::vector<Built> built;
    uint32_t offset = 0;
    uint32_t maxAlign = 1;
    for (const json& fj : *fields) {
        Built b;
        b.name = fj.value("name", std::string());
        const std::string typeName = fj.value("type", std::string());
        const TypeEntry* te = FindType(typeName);
        if (b.name.empty() || te == nullptr) {
            MYE_LOG_ERROR("[schema] %s: field needs a \"name\" and a known \"type\" (got '%s')",
                          pathU.c_str(), typeName.c_str());
            return false;
        }
        for (const Built& prev : built) {
            if (prev.name == b.name) {
                MYE_LOG_ERROR("[schema] %s: duplicate field name '%s'", pathU.c_str(),
                              b.name.c_str());
                return false;
            }
        }
        b.type = te->type;
        offset = (offset + te->align - 1) / te->align * te->align;
        b.offset = offset;
        offset += FieldTypeSize(te->type);
        maxAlign = std::max(maxAlign, te->align);
        b.display = fj.value("display", std::string());
        b.tooltip = fj.value("tooltip", std::string());
        b.dragSpeed = fj.value("speed", 0.0f);
        b.minVal = fj.value("min", 0.0f);
        b.maxVal = fj.value("max", 0.0f);
        if (fj.contains("default")) {
            b.def = &fj["default"];
        }
        built.push_back(std::move(b));
    }
    const uint32_t size = (offset + maxAlign - 1) / maxAlign * maxAlign;

    // ---- 既定値ブロブ (construct が memcpy する実体) ----
    const size_t slot = Blobs().size();
    std::vector<uint8_t> blob(size, 0); // 未指定フィールドと詰め物はゼロ = 決定論
    static const json kNull;
    for (const Built& b : built) {
        WriteDefault(blob.data() + b.offset, b.type, b.def ? *b.def : kNull);
    }
    Blobs().push_back(std::move(blob));

    // ---- ComponentDesc ----
    // 文字列は ComponentRegistry が寿命を保証しないので永続コピーを作る
    // (ScriptHost の _strdup と同じ扱い。スキーマ数は小さいので解放しない)
    ComponentDesc cd;
    cd.name = _strdup(name.c_str());
    cd.nameHash = HashStr(name);
    cd.size = size;
    cd.align = maxAlign;
    cd.flags = j.value("hidden", false) ? kComponentHidden : kComponentNone;
    cd.construct = Ctors()[slot];
    cd.fields.reserve(built.size());
    for (const Built& b : built) {
        FieldDesc fd;
        fd.name = _strdup(b.name.c_str());
        fd.type = b.type;
        fd.offset = b.offset;
        fd.flags = kFieldNone;
        fd.dragSpeed = b.dragSpeed;
        fd.minVal = b.minVal;
        fd.maxVal = b.maxVal;
        fd.tooltip = b.tooltip.empty() ? nullptr : _strdup(b.tooltip.c_str());
        fd.displayName = b.display.empty() ? nullptr : _strdup(b.display.c_str());
        cd.fields.push_back(fd);
    }

    UsedIds().push_back(stableId);
    const ComponentTypeId id = ComponentRegistry::Get().Register(std::move(cd));
    RegisteredIds().push_back(id);
    MYE_LOG_INFO("[schema] registered '%s' (id %llu, %u bytes, %zu fields) -> TypeId %u",
                 name.c_str(), static_cast<unsigned long long>(stableId), size, built.size(), id);
    return true;
}

} // namespace

size_t RegisterSchemaComponentsFrom(const std::wstring& schemaDir)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(schemaDir, ec)) {
        return 0;
    }
    // ★走査順は OS 依存 → 相対パス昇順に**明示ソート**してから登録する。
    //   ここが揺れると TypeId の割り当てが run 間で変わり、リプレイが壊れる
    std::vector<fs::path> files;
    for (const fs::directory_entry& e : fs::recursive_directory_iterator(schemaDir, ec)) {
        if (!e.is_regular_file()) {
            continue;
        }
        const std::wstring p = e.path().wstring();
        constexpr size_t kSuffixLen = 22; // ".component.schema.json"
        if (p.size() >= kSuffixLen
            && p.compare(p.size() - kSuffixLen, kSuffixLen, L".component.schema.json") == 0) {
            files.push_back(e.path());
        }
    }
    std::sort(files.begin(), files.end(),
              [](const fs::path& a, const fs::path& b) { return a.wstring() < b.wstring(); });

    size_t count = 0;
    for (const fs::path& p : files) {
        if (RegisterOne(p)) {
            ++count;
        }
    }
    return count;
}

size_t RegisterSchemaComponents(const std::wstring& assetsRoot)
{
    return RegisterSchemaComponentsFrom(assetsRoot + L"\\schemas");
}

const std::vector<uint32_t>& RegisteredSchemaTypeIds()
{
    return RegisteredIds();
}

} // namespace mye::schema
