//====================================================================================
//                          EngineIndexDumpSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/15/2026
//                                          エンジン索引ダンプの回帰テスト実装
//====================================================================================
#include "Editor/Tools/EngineIndexDumpSelfTest.h"

#include <set>
#include <string>

#include "Editor/SelfTest/SelfTestSuites.h"
#include "Editor/Tools/EngineIndexDump.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/ComponentRegistry.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Engine/App/EngineCli.h"
#include "Engine/Engine/Demo/ShowcaseScenes.h"
#include "nlohmann/json.hpp"

namespace mye {

bool RunEngineIndexDumpSelfTest()
{
    MYE_LOG_INFO("==== EngineIndexDump (M87a) self test ====");
    RegisterBuiltinComponents();
    int failCount = 0;
    const auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // 登録表の選択規則 (専用スイートは作らない方針なので、M95 が触らないこのスイートへ間借りする)
    for (const std::string& failure : VerifySelfTestSelectionRules()) check(false, failure.c_str());
    check(true, "selftest 選択規則");

    const std::string text = BuildEngineIndexDumpJson();
    check(text == BuildEngineIndexDumpJson(), "決定性: 2 回作って同一");

    nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    check(!root.is_discarded() && root.is_object(), "JSON として読める");
    if (root.is_discarded() || !root.is_object()) {
        MYE_LOG_ERROR("==== EngineIndexDump self test: %d FAILED ====", failCount + 1);
        return false;
    }

    // 組み込み登録数 (スクリプト状態の型は含めない)
    uint32_t builtinCount = 0;
    const ComponentRegistry& registry = ComponentRegistry::Get();
    for (ComponentTypeId id = 0; id < registry.Count(); ++id) {
        if ((registry.Desc(id).flags & kComponentScriptState) == 0) {
            ++builtinCount;
        }
    }
    const nlohmann::json& components = root["components"];
    check(components.is_array() && components.size() == builtinCount && builtinCount > 0,
          "components 件数 = 組み込み登録数");

    bool allTypesKnown = true;
    bool namesUnique = true;
    bool idsAscending = true;
    std::set<std::string> names;
    int64_t lastId = -1;
    const nlohmann::json* transform = nullptr;
    for (const nlohmann::json& c : components) {
        const std::string name = c.value("name", "");
        namesUnique &= names.insert(name).second;
        const int64_t id = c.value("typeId", int64_t(-1));
        idsAscending &= id > lastId;
        lastId = id;
        if (name == "LocalTransform") {
            transform = &c;
        }
        for (const nlohmann::json& f : c["fields"]) {
            allTypesKnown &= f.value("type", "Unknown") != "Unknown";
        }
    }
    check(allTypesKnown, "全フィールドの型名が解決できている (FieldType 追加の取りこぼし検出)");
    check(namesUnique, "component 名が一意");
    check(idsAscending, "typeId が登録順 (昇順)");

    bool positionOk = false;
    if (transform != nullptr) {
        for (const nlohmann::json& f : (*transform)["fields"]) {
            if (f.value("name", "") == "position") {
                positionOk = f.value("type", "") == "Float3" && f.contains("jp");
            }
        }
    }
    check(positionOk, "LocalTransform.position は Float3 で jp 付き");

    check(root["cliEngine"].size() == ListEngineCliFlags().size() && !root["cliEngine"].empty(),
          "cliEngine 件数 = 共通フラグ表");
    check(root["cliShowcase"].size() == ListShowcases().size() && !root["cliShowcase"].empty(),
          "cliShowcase 件数 = showcase 表");

    if (failCount == 0) {
        MYE_LOG_INFO("==== EngineIndexDump self test: ALL PASS ====");
    } else {
        MYE_LOG_ERROR("==== EngineIndexDump self test: %d FAILED ====", failCount);
    }
    return failCount == 0;
}

} // namespace mye
