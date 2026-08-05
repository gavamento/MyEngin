#include "Engine/Engine/SchemaSelfTest.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "Engine/Core/ComponentRegistry.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/SceneSerializer.h"
#include "Engine/Engine/SchemaComponents.h"

#include "nlohmann/json.hpp"

namespace mye {
namespace {

void WriteFile(const std::filesystem::path& p, const char* text)
{
    std::ofstream f(p);
    f << text;
}

// desc からフィールドを名前で引く (スキーマ型は C++ の struct を持たないので、
// テストも実行時メタデータ経由で読む = Inspector / シリアライザと同じ道)
const FieldDesc* Field(const ComponentDesc& d, const char* name)
{
    for (const FieldDesc& f : d.fields) {
        if (std::strcmp(f.name, name) == 0) {
            return &f;
        }
    }
    return nullptr;
}

template <typename T>
T Read(const void* comp, const FieldDesc& f)
{
    T v{};
    std::memcpy(&v, static_cast<const uint8_t*>(comp) + f.offset, sizeof(T));
    return v;
}

} // namespace

bool RunSchemaSelfTest()
{
    MYE_LOG_INFO("==== Schema component self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path() / L"mye_selftest_schema";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // ---- 正しいスキーマ 2 件 (ファイル名昇順 = 登録順) ----
    // a_ の方でアラインメント (Bool の後ろの UInt64) も見る
    WriteFile(dir / L"a_stats.component.schema.json", R"({
  "engine": "MyEngine", "componentSchema": 1, "id": 90001, "name": "MyeTestStats",
  "fields": [
    { "name": "hp",    "type": "Float",  "default": 100.0, "min": 0, "max": 999, "display": "体力" },
    { "name": "team",  "type": "Int32",  "default": 2 },
    { "name": "alive", "type": "Bool",   "default": true },
    { "name": "tag",   "type": "UInt64", "default": 7 }
  ]
})");
    WriteFile(dir / L"b_marker.component.schema.json", R"({
  "engine": "MyEngine", "componentSchema": 1, "id": 90002, "name": "MyeTestMarker",
  "fields": [
    { "name": "label", "type": "String64", "default": "spawn" },
    { "name": "color", "type": "Color",    "default": [1.0, 0.0, 0.0, 1.0] }
  ]
})");
    // ---- 弾かれるべきスキーマ ----
    WriteFile(dir / L"c_dupid.component.schema.json", R"({
  "componentSchema": 1, "id": 90001, "name": "MyeTestDupId",
  "fields": [ { "name": "x", "type": "Float" } ]
})");
    // ★これが素通りすると Collider の TypeId を乗っ取り、サイズ違いの別物に化ける
    WriteFile(dir / L"d_collide.component.schema.json", R"({
  "componentSchema": 1, "id": 90003, "name": "Collider",
  "fields": [ { "name": "x", "type": "Float" } ]
})");
    WriteFile(dir / L"e_badtype.component.schema.json", R"({
  "componentSchema": 1, "id": 90004, "name": "MyeTestBadType",
  "fields": [ { "name": "x", "type": "Vector9" } ]
})");
    WriteFile(dir / L"f_noid.component.schema.json", R"({
  "componentSchema": 1, "name": "MyeTestNoId",
  "fields": [ { "name": "x", "type": "Float" } ]
})");
    WriteFile(dir / L"g_nofields.component.schema.json", R"({
  "componentSchema": 1, "id": 90005, "name": "MyeTestNoFields", "fields": []
})");
    WriteFile(dir / L"h_broken.component.schema.json", "{ this is not json");
    // 拡張子違いは走査対象外 (誤って拾うと無関係な .json で型が生えてしまう)
    WriteFile(dir / L"i_ignored.json", R"({
  "componentSchema": 1, "id": 90006, "name": "MyeTestIgnored",
  "fields": [ { "name": "x", "type": "Float" } ]
})");

    ComponentRegistry& reg = ComponentRegistry::Get();
    World bootstrap; // 組込み型の登録 (RegisterBuiltinComponents) を済ませてから測る
    (void)bootstrap;
    const uint32_t countBefore = reg.Count();
    const ComponentTypeId colliderBefore = reg.FindByName("Collider");

    const size_t registered = schema::RegisterSchemaComponentsFrom(dir.wstring());
    check(registered == 2, "load: only the two valid schemas are registered");

    const ComponentTypeId statsId = reg.FindByName("MyeTestStats");
    const ComponentTypeId markerId = reg.FindByName("MyeTestMarker");
    check(statsId != kInvalidComponentType && markerId != kInvalidComponentType,
          "load: both valid schemas produced component types");
    check(statsId < markerId,
          "order: registration follows the sorted file path (a_ before b_) - TypeIds are stable");
    check(reg.Count() == countBefore + 2, "load: exactly two types were added");

    check(reg.FindByName("MyeTestDupId") == kInvalidComponentType,
          "reject: a duplicate stable id is refused");
    check(reg.FindByName("MyeTestBadType") == kInvalidComponentType,
          "reject: an unknown field type is refused (FieldType stays a closed set)");
    check(reg.FindByName("MyeTestNoId") == kInvalidComponentType,
          "reject: a schema without a non-zero id is refused");
    check(reg.FindByName("MyeTestNoFields") == kInvalidComponentType,
          "reject: a schema with no fields is refused");
    check(reg.FindByName("MyeTestIgnored") == kInvalidComponentType,
          "reject: only *.component.schema.json is scanned");
    // ★名前衝突が最重要: 既存型を乗っ取っていないこと
    check(reg.FindByName("Collider") == colliderBefore
              && reg.Desc(colliderBefore).size == sizeof(ColliderComponent),
          "reject: a schema cannot hijack an existing component name");

    // ---- レイアウト (宣言順 + 自然アラインメント) ----
    const ComponentDesc& stats = reg.Desc(statsId);
    const FieldDesc* fHp = Field(stats, "hp");
    const FieldDesc* fTeam = Field(stats, "team");
    const FieldDesc* fAlive = Field(stats, "alive");
    const FieldDesc* fTag = Field(stats, "tag");
    check(fHp && fTeam && fAlive && fTag && fHp->offset == 0 && fTeam->offset == 4
              && fAlive->offset == 8 && fTag->offset == 16,
          "layout: fields are packed in order with natural alignment (u64 lands on 8)");
    check(stats.size == 24 && stats.align == 8, "layout: size is rounded up to the type alignment");
    check(fHp->minVal == 0.0f && fHp->maxVal == 999.0f && fHp->displayName != nullptr
              && std::strcmp(fHp->displayName, "体力") == 0,
          "meta: range and Japanese display name reach FieldDesc");

    // ---- 既定値 (construct) + ECS 往復 ----
    Scene scene;
    World& w = scene.GetWorld();
    GameObject e = scene.CreateGameObjectTracked("SchemaHost");
    w.ApplyStructuralChanges();
    void* comp = w.AddComponentRaw(e.Id(), statsId);
    void* mk = w.AddComponentRaw(e.Id(), markerId);
    w.ApplyStructuralChanges();
    comp = w.GetComponentRaw(e.Id(), statsId); // アーキタイプ移動でポインタは変わる
    mk = w.GetComponentRaw(e.Id(), markerId);
    check(comp != nullptr && mk != nullptr, "ecs: AddComponentRaw works for schema types");
    check(comp && Read<float>(comp, *fHp) == 100.0f && Read<int32_t>(comp, *fTeam) == 2
              && Read<uint8_t>(comp, *fAlive) == 1 && Read<uint64_t>(comp, *fTag) == 7ull,
          "default: construct writes the schema defaults");
    const ComponentDesc& marker = reg.Desc(markerId);
    const FieldDesc* fLabel = Field(marker, "label");
    const FieldDesc* fColor = Field(marker, "color");
    check(mk && fLabel && fColor
              && std::strcmp(static_cast<const char*>(mk) + fLabel->offset, "spawn") == 0
              && Read<float>(mk, *fColor) == 1.0f,
          "default: string and color defaults are written too");

    // ---- ハッシュ被覆 + 保存往復 ----
    {
        const uint64_t h0 = HashWorld(w, nullptr);
        const float newHp = 42.5f;
        std::memcpy(static_cast<uint8_t*>(comp) + fHp->offset, &newHp, sizeof(newHp));
        const uint64_t h1 = HashWorld(w, nullptr);
        check(h0 != h1, "hash: schema fields are covered by the world hash");

        const nlohmann::json saved = SceneSerializer::SaveToJson(scene);
        Scene s2;
        SceneSerializer::LoadFromJson(s2, saved);
        World& w2 = s2.GetWorld();
        const void* c2 = w2.GetComponentRaw(s2.Find("SchemaHost").Id(), statsId);
        const void* m2 = w2.GetComponentRaw(s2.Find("SchemaHost").Id(), markerId);
        check(c2 && Read<float>(c2, *fHp) == newHp && Read<uint64_t>(c2, *fTag) == 7ull,
              "serialize: schema values survive a save/load round trip");
        check(m2 && std::strcmp(static_cast<const char*>(m2) + fLabel->offset, "spawn") == 0,
              "serialize: schema strings survive the round trip");
        check(HashWorld(w2, nullptr) == h1, "serialize: the round trip is hash-identical");
    }

    // ---- 冪等性 (= 2 回起動しても TypeId が同じことの検査可能な代理) ----
    {
        const uint32_t before = reg.Count();
        const size_t again = schema::RegisterSchemaComponentsFrom(dir.wstring());
        check(again == 0 && reg.Count() == before && reg.FindByName("MyeTestStats") == statsId
                  && reg.FindByName("MyeTestMarker") == markerId,
              "idempotent: re-registering the same schemas adds nothing and keeps the TypeIds");
    }

    fs::remove_all(dir, ec);
    if (failCount == 0) {
        MYE_LOG_INFO("==== Schema component self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Schema component self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
