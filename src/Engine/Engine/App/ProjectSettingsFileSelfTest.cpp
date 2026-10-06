//====================================================================================
//                          ProjectSettingsFileSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/06/2026
//                                          project_settings.json の読み書きの回帰テスト
//====================================================================================
#include "Engine/Engine/App/ProjectSettingsFileSelfTest.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <process.h>
#include <string>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/App/ProjectSettingsFile.h"
#include "Engine/Engine/Scene/TagNames.h"
#include "Engine/Engine/UI/UIProjectSettings.h"

namespace mye {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string ReadAll(const fs::path& file)
{
    std::ifstream f(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void WriteAll(const fs::path& file, const std::string& text)
{
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// 一時ファイル (.tmp) の取り残しが無いことを見る
int FileCount(const fs::path& dir)
{
    int n = 0;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
        (void)e;
        ++n;
    }
    return n;
}

} // namespace

bool RunProjectSettingsFileSelfTest()
{
    MYE_LOG_INFO("==== project_settings.json read / update self test ====");
    int failCount = 0;
    const auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // Debug と Release の selftest を同時に回しても消し合わないよう PID を混ぜる
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / (L"mye_project_settings_selftest_" + std::to_wstring(_getpid()));
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path file = dir / L"project_settings.json";
    const std::wstring path = ProjectSettingsPath(dir.wstring());
    check(fs::path(path) == file, "path: assetsRoot + project_settings.json");

    // ---- ファイルが無い: 読みは Missing、保存は新規作成 ----
    {
        json j = json::array();
        check(ReadProjectSettingsFile(path, j) == ProjectSettingsRead::Missing && j.is_object() && j.empty(),
              "missing: read reports Missing and yields an empty object");
        check(UpdateProjectSettingsFile(path, [](json& r) { r["tags"] = json::array({ "Enemy" }); }),
              "missing: update creates the file");
        const std::string text = ReadAll(file);
        check(ReadProjectSettingsFile(path, j) == ProjectSettingsRead::Ok && j["tags"].size() == 1,
              "missing: the created file reads back");
        check(!text.empty() && text.back() == '\n' && text.find('\r') == std::string::npos,
              "missing: a new file ends with a newline and uses LF");
        check(FileCount(dir) == 1, "missing: no temporary file is left behind");
    }

    // ---- 既存のキーが残る ----
    {
        WriteAll(file, R"({"physicsLayers":["Default","Player"],"particleBackend":"gpu","ui":{"referenceW":1280}})");
        check(UpdateProjectSettingsFile(path, [](json& r) { r["navAreas"] = json::array({ "Walkable" }); }),
              "merge: update succeeds on a valid file");
        json j;
        check(ReadProjectSettingsFile(path, j) == ProjectSettingsRead::Ok && j["physicsLayers"].size() == 2
                  && j["particleBackend"] == "gpu" && j["ui"]["referenceW"] == 1280 && j["navAreas"].size() == 1,
              "merge: the other owners' keys survive, the new key is written");
    }

    // ---- 壊れたファイルは上書きしない (空から書き直すと他のキーが全部消える) ----
    {
        const std::string broken = "{\"physicsLayers\": [\"Default\", \"Player\"";
        WriteAll(file, broken);
        json j = json::array();
        check(ReadProjectSettingsFile(path, j) == ProjectSettingsRead::Corrupt && j.is_object() && j.empty(),
              "corrupt: truncated JSON reads as Corrupt");
        check(!UpdateProjectSettingsFile(path, [](json& r) { r["tags"] = json::array(); }),
              "corrupt: update refuses to save");
        check(ReadAll(file) == broken && FileCount(dir) == 1,
              "corrupt: the file is byte-identical and no temporary file is left");

        const std::string rootArray = "[1, 2, 3]\n";
        WriteAll(file, rootArray);
        check(ReadProjectSettingsFile(path, j) == ProjectSettingsRead::Corrupt
                  && !UpdateProjectSettingsFile(path, [](json& r) { r["tags"] = json::array(); })
                  && ReadAll(file) == rootArray,
              "corrupt: a non-object root is refused the same way");
    }

    // ---- 空白だけのファイルは失うものが無いので保存できる ----
    {
        WriteAll(file, " \r\n");
        json j;
        check(ReadProjectSettingsFile(path, j) == ProjectSettingsRead::Ok && j.empty(),
              "blank: a whitespace-only file reads as an empty object");
        check(UpdateProjectSettingsFile(path, [](json& r) { r["tags"] = json::array(); })
                  && ReadProjectSettingsFile(path, j) == ProjectSettingsRead::Ok && j.contains("tags"),
              "blank: update succeeds");
    }

    // ---- 改行コードは既存ファイルに合わせる ----
    {
        WriteAll(file, "{\r\n  \"physicsLayers\": [\r\n    \"Default\"\r\n  ]\r\n}\r\n");
        check(UpdateProjectSettingsFile(path, [](json& r) { r["tags"] = json::array({ "A", "B" }); }),
              "newline: update succeeds on a CRLF file");
        const std::string text = ReadAll(file);
        bool loneLf = false;
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) {
                loneLf = true;
            }
        }
        check(!loneLf && text.find("\r\n") != std::string::npos, "newline: a CRLF file stays CRLF on every line");

        WriteAll(file, "{\n  \"physicsLayers\": []\n}\n");
        check(UpdateProjectSettingsFile(path, [](json& r) { r["tags"] = json::array({ "A" }); })
                  && ReadAll(file).find('\r') == std::string::npos,
              "newline: an LF file stays LF");
    }

    // ---- 実際の書き手を通す: 壊れたファイルで保存が失敗し、中身が変わらない ----
    {
        const std::string broken = "{\"navAgentTypes\": [ {\"id\": 0, ";
        WriteAll(file, broken);
        TagNames names;
        names.Load(dir.wstring(), true);
        std::snprintf(names.EditBuffer(0), TagNames::kNameCapacity, "%s", "Enemy");
        check(!names.Save(dir.wstring()) && ReadAll(file) == broken,
              "writers: TagNames::Save fails on a corrupt file and leaves it untouched");
        RtTagRules rules;
        rules.sceneOn = 1;
        check(!SaveRtTagRules(dir.wstring(), rules) && ReadAll(file) == broken,
              "writers: SaveRtTagRules fails on a corrupt file and leaves it untouched");
        uilayout::ProjectUiSettings ui;
        check(!uilayout::SaveProjectUiSettings(dir.wstring(), ui) && ReadAll(file) == broken,
              "writers: SaveProjectUiSettings fails on a corrupt file and leaves it untouched");
    }

    // ---- "ui" がオブジェクト以外でも落ちずに保存でき、他のキーが残る ----
    {
        WriteAll(file, R"({"ui": 5, "tags": ["Enemy"]})");
        uilayout::ProjectUiSettings ui;
        ui.referenceW = 1280;
        ui.referenceH = 720;
        json j;
        check(uilayout::SaveProjectUiSettings(dir.wstring(), ui)
                  && ReadProjectSettingsFile(path, j) == ProjectSettingsRead::Ok && j["ui"]["referenceW"] == 1280
                  && j["ui"]["referenceH"] == 720 && j["tags"].size() == 1,
              "writers: a non-object \"ui\" value is replaced without losing other keys");
    }

    fs::remove_all(dir, ec);
    MYE_LOG_INFO("project_settings.json self test: %s (%d failure(s))", failCount == 0 ? "OK" : "FAILED", failCount);
    return failCount == 0;
}

} // namespace mye
