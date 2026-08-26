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
#include "Engine/Engine/SchemaCodegen.h"
#include "Engine/Engine/SchemaComponents.h"
#include "Engine/Engine/Script/EngineApiTable.h" // v11 汎用フィールドスロットの実配線 (M50d)
#include "Shared/ScriptAPI.h"                    // MyeNameHash (Shared 再掲 FNV の機械検査)

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
        const uint64_t h0 = HashWorld(w);
        const float newHp = 42.5f;
        std::memcpy(static_cast<uint8_t*>(comp) + fHp->offset, &newHp, sizeof(newHp));
        const uint64_t h1 = HashWorld(w);
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
        check(HashWorld(w2) == h1, "serialize: the round trip is hash-identical");
    }

    // ---- 冪等性 (= 2 回起動しても TypeId が同じことの検査可能な代理) ----
    {
        const uint32_t before = reg.Count();
        const size_t again = schema::RegisterSchemaComponentsFrom(dir.wstring());
        check(again == 0 && reg.Count() == before && reg.FindByName("MyeTestStats") == statsId
                  && reg.FindByName("MyeTestMarker") == markerId,
              "idempotent: re-registering the same schemas adds nothing and keeps the TypeIds");
    }

    // ---- 汎用フィールド ABI (v11、M50d): C ABI 経路の Get/Set ----
    // スキーマ型は C++ の struct を持たない = このレーンの主客。テーブル経由で
    // 「GameLogic.dll / C# が実際に呼ぶ道」を検査する (PartSelfTest の ABI 節と同じ流儀)
    {
        ScriptApiContext apiCtx;
        apiCtx.scene = &scene;
        MyeEngineApi api = {};
        BuildEngineApi(api, &apiCtx);
        const MyeEntityId se = { e.Id().index, e.Id().generation };
        const uint64_t compH = HashStr("MyeTestStats");
        const uint64_t hpH = HashStr("hp");

        // Shared 再掲の FNV とエンジン HashStr の一致 (MyePartTag と同じ機械検査)
        check(MyeNameHash("MyeTestStats") == compH && MyeNameHash("hp") == hpH
                  && MyeNameHash("") == HashStr(""),
              "abi: MyeNameHash (Shared) equals HashStr (engine)");

        float hp = 0.0f;
        int32_t type = -1;
        check(api.GetComponentField(&apiCtx, se, compH, hpH, &hp, 4, &type) == 4 && hp == 42.5f
                  && type == MYE_FIELD_FLOAT,
              "abi: GetComponentField returns size, value and type");
        const float newHp = 55.25f;
        check(api.SetComponentField(&apiCtx, se, compH, hpH, &newHp, 4) == 1
                  && Read<float>(w.GetComponentRaw(e.Id(), statsId), *fHp) == 55.25f,
              "abi: SetComponentField writes the sim state (hash-covered lane)");
        double big = 0.0;
        check(api.GetComponentField(&apiCtx, se, compH, hpH, &big, 2, nullptr) == 0,
              "abi: a too-small buffer is refused");
        check(api.SetComponentField(&apiCtx, se, compH, hpH, &big, 8) == 0,
              "abi: a size mismatch on set is refused (no silent type punning)");
        check(api.GetComponentField(&apiCtx, se, compH, HashStr("nope"), &hp, 4, nullptr) == 0
                  && api.GetComponentField(&apiCtx, se, HashStr("NopeComp"), hpH, &hp, 4,
                                           nullptr) == 0,
              "abi: unknown component / field hashes yield 0");
        const MyeEntityId dead = { 0xFFFFFFFFu, 0u }; // null id (MyeEntityIdIsNull)
        check(api.GetComponentField(&apiCtx, dead, compH, hpH, &hp, 4, nullptr) == 0,
              "abi: a dead entity yields 0");

        // ---- String64: Set が尾部ゼロ埋め + 終端を保証する (M48i のハッシュ罠を境界で断つ) ----
        {
            const uint64_t markH = HashStr("MyeTestMarker");
            const uint64_t labelH = HashStr("label");
            uint8_t* lbl =
                static_cast<uint8_t*>(w.GetComponentRaw(e.Id(), markerId)) + fLabel->offset;
            std::memset(lbl, 0, 64);
            std::memcpy(lbl, "abc", 3);
            const uint64_t cleanHash = HashWorld(w);
            lbl[10] = 'Z'; // 終端より後ろの残骸 (文字列としては同じ "abc")
            check(HashWorld(w) != cleanHash,
                  "abi: tail garbage is hash-visible (precondition for the next check)");
            check(api.SetComponentField(&apiCtx, se, markH, labelH, "abc", 4) == 1
                      && HashWorld(w) == cleanHash,
                  "abi: a short string set zeroes the tail (String64 trap closed at the boundary)");
            char sout[64] = {};
            int32_t st = -1;
            check(api.GetComponentField(&apiCtx, se, markH, labelH, sout, 64, &st) == 64
                      && std::strcmp(sout, "abc") == 0 && st == MYE_FIELD_STRING64,
                  "abi: a string get returns the full fixed buffer and its type");
            check(api.SetComponentField(&apiCtx, se, markH, labelH, sout, 65) == 0,
                  "abi: an oversized string set is refused");
        }

        // ---- 非所持 / NoHash (C# レーン) の遮断 ----
        {
            GameObject bare = scene.CreateGameObjectTracked("SchemaBare");
            w.ApplyStructuralChanges();
            const MyeEntityId sb = { bare.Id().index, bare.Id().generation };
            check(api.GetComponentField(&apiCtx, sb, compH, hpH, &hp, 4, nullptr) == 0,
                  "abi: an entity without the component yields 0");

            // NoHash (C# スクリプト状態 = 非決定論レーン) は読み書きとも遮断される。
            // 実物の C# 型はヘッドレス selftest に居ないので、旗だけ立てた代理を登録する
            ComponentDesc nh;
            nh.name = "MyeTestNoHash";
            nh.nameHash = HashStr("MyeTestNoHash");
            nh.size = 4;
            nh.align = 4;
            nh.flags = kComponentNoHash;
            nh.construct = [](void* dst) { std::memset(dst, 0, 4); };
            FieldDesc nf;
            nf.name = "v";
            nf.type = FieldType::Float;
            nf.offset = 0;
            nh.fields.push_back(nf);
            const ComponentTypeId nhId = ComponentRegistry::Get().Register(std::move(nh));
            w.AddComponentRaw(e.Id(), nhId);
            w.ApplyStructuralChanges();
            const MyeEntityId se2 = { e.Id().index, e.Id().generation };
            float v = 1.0f;
            check(api.GetComponentField(&apiCtx, se2, HashStr("MyeTestNoHash"), HashStr("v"), &v,
                                        4, nullptr) == 0
                      && api.SetComponentField(&apiCtx, se2, HashStr("MyeTestNoHash"),
                                               HashStr("v"), &v, 4) == 0,
                  "abi: the NoHash (C#) lane is blocked in both directions");
        }
    }

    // ---- codegen (M50d): モデルは実行時登録の完全ミラー ----
    {
        // 登録は通るが生成では落ちる名前 (C++/C# キーワード) — 実行時アクセスは
        // ハッシュ経由なので登録自体は正しい
        const fs::path dir2 = fs::temp_directory_path() / L"mye_selftest_schema_cg";
        fs::remove_all(dir2, ec);
        fs::create_directories(dir2, ec);
        WriteFile(dir2 / L"kw.component.schema.json", R"({
  "componentSchema": 1, "id": 90100, "name": "MyeTestKeyword",
  "fields": [ { "name": "class", "type": "Float" } ]
})");
        // 全型網羅 (EntityRef の align 8 / Float4x4 / String256 の詰め物ミラーを生成で通す)
        WriteFile(dir2 / L"all.component.schema.json", R"({
  "componentSchema": 1, "id": 90101, "name": "MyeTestAllTypes",
  "fields": [
    { "name": "f2",    "type": "Float2" },
    { "name": "who",   "type": "EntityRef" },
    { "name": "f3",    "type": "Float3" },
    { "name": "asset", "type": "AssetRef" },
    { "name": "u32",   "type": "UInt32" },
    { "name": "xf",    "type": "Float4x4" },
    { "name": "note",  "type": "String256" },
    { "name": "rot",   "type": "Quat" },
    { "name": "f4",    "type": "Float4" }
  ]
})");
        check(schema::RegisterSchemaComponentsFrom(dir2.wstring()) == 2,
              "codegen: a keyword field name still registers (runtime access is hash-based)");

        const std::vector<schema::CodegenComponent> model = schema::BuildCodegenModel();
        const schema::CodegenComponent* cgStats = nullptr;
        const schema::CodegenComponent* cgAll = nullptr;
        bool kwPresent = false;
        for (const schema::CodegenComponent& c : model) {
            if (c.name == "MyeTestStats") {
                cgStats = &c;
            }
            if (c.name == "MyeTestAllTypes") {
                cgAll = &c;
            }
            if (c.name == "MyeTestKeyword") {
                kwPresent = true;
            }
        }
        check(cgStats != nullptr && cgAll != nullptr,
              "codegen: registered schemas appear in the model");
        check(!kwPresent, "codegen: a keyword field name drops the component from generation");
        check(cgAll != nullptr && cgAll->size == 400 && cgAll->fields.size() == 9
                  && cgAll->fields[1].name == "who" && cgAll->fields[1].offset == 8
                  && cgAll->fields[3].name == "asset" && cgAll->fields[3].offset == 32
                  && cgAll->fields[5].name == "xf" && cgAll->fields[5].offset == 44
                  && cgAll->fields[5].size == 64 && cgAll->fields[6].name == "note"
                  && cgAll->fields[6].offset == 108 && cgAll->fields[6].size == 256,
              "codegen: the all-types schema mirrors alignment-heavy offsets");
        check(cgStats != nullptr && cgStats->nameHash == HashStr("MyeTestStats")
                  && cgStats->size == 24 && cgStats->fields.size() == 4
                  && cgStats->fields[0].name == "hp" && cgStats->fields[0].offset == 0
                  && cgStats->fields[0].type == FieldType::Float && cgStats->fields[0].size == 4
                  && cgStats->fields[2].name == "alive" && cgStats->fields[2].offset == 8
                  && cgStats->fields[3].name == "tag" && cgStats->fields[3].offset == 16
                  && cgStats->fields[3].size == 8,
              "codegen: offsets/sizes/hashes mirror the runtime registry");

        // 生成物のスモーク。レイアウトの最終検査は生成ヘッダ内の static_assert が
        // プロジェクトのコンパイル時に行う (ここでは「詰め物とサイズが焼かれた」ことを見る)
        const std::string hpp = schema::EmitCppHeader(model);
        check(hpp.find("struct MyeTestStatsSchema {") != std::string::npos
                  && hpp.find("uint8_t _pad0_[7];") != std::string::npos
                  && hpp.find("static_assert(sizeof(MyeTestStatsSchema) == 24,")
                         != std::string::npos
                  && hpp.find("MyeNameHash(\"MyeTestStats\")") != std::string::npos,
              "codegen: the C++ emission bakes padding, size and hash cross-checks");
        const std::string cs = schema::EmitCSharpSource(model);
        check(cs.find("public static class MyeTestStatsSchema") != std::string::npos
                  && cs.find("public static bool GetHp(MyeEntity e, out float v)")
                         != std::string::npos
                  && cs.find("TryGetFieldString") != std::string::npos,
              "codegen: the C# emission contains classes and typed accessors");
        // 生成ヘッダを温存ダンプ (%TEMP%\mye_selftest_schema_gen.h)。ABI/emitter を触った
        // ときの手動コンパイルスモーク用 — cl /std:c++20 /I src /I %TEMP% で include すれば
        // 生成物内の static_assert (offset/size/FNV) が実コンパイラで検査される
        schema::WriteIfChanged(
            (fs::temp_directory_path() / L"mye_selftest_schema_gen.h").wstring(), hpp);
        fs::remove_all(dir2, ec);
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
