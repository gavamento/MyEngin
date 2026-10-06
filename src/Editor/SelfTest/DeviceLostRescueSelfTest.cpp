//====================================================================================
//                          DeviceLostRescueSelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      10/07/2026
//                                          デバイス消失時の退避保存の回帰テスト
//====================================================================================
#include "Editor/SelfTest/DeviceLostRescueSelfTest.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#include "Editor/App/DeviceLostRescue.h"
#include "Editor/Scene/PlayModeController.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Scene/SceneSerializer.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {

namespace {

std::string ReadAll(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// path のシーンを新しい Scene へ読み、シリアライズし直した JSON を返す (読めなければ null)
nlohmann::json ReloadAsJson(const std::wstring& path)
{
    Scene loaded;
    if (!SceneSerializer::LoadFromFile(loaded, path)) {
        return nullptr;
    }
    return SceneSerializer::SaveToJson(loaded);
}

} // namespace

bool RunDeviceLostRescueSelfTest()
{
    MYE_LOG_INFO("==== DeviceLostRescue self test ====");
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
    const fs::path tempRoot = fs::temp_directory_path(ec) / L"mye_devicelost_selftest";
    fs::remove_all(tempRoot, ec);
    fs::create_directories(tempRoot, ec);

    // ---- ファイル名の基部 ----
    check(RescueSceneName(L"C:\\proj\\assets\\scenes\\main.scene.json") == L"main", "scene name drops the extensions");
    check(RescueSceneName(L"") == L"scene", "empty scene path falls back to \"scene\"");

    // 元のシーンファイル (退避が上書きしてはならない)
    const fs::path original = tempRoot / L"assets" / L"main.scene.json";
    fs::create_directories(original.parent_path(), ec);
    {
        std::ofstream f(original, std::ios::binary);
        f << "original bytes, not a scene";
    }
    const std::string originalBytes = ReadAll(original);

    // ---- 編集状態 (Play 中でない) の退避 → 再読込 ----
    Scene scene;
    scene.CreateGameObjectTracked("alpha");
    scene.CreateGameObjectTracked("beta");
    scene.GetWorld().ApplyStructuralChanges();
    const nlohmann::json editing = SceneSerializer::SaveToJson(scene);
    {
        const RescueSaveResult r = SaveRescueScene(editing, tempRoot.wstring(), RescueSceneName(original.wstring()),
                                                   RescueTimestamp());
        check(r.ok, "editing state is saved");
        check(r.ok && fs::path(r.path).is_absolute() && fs::exists(r.path), "rescue path is an existing absolute path");
        check(r.ok && fs::path(r.path) != original, "rescue file is not the original file");
        check(ReloadAsJson(r.path) == editing, "editing state: save -> reload round-trips");
    }
    check(ReadAll(original) == originalBytes, "original scene file is untouched");

    // ---- Play 中: Play 前の状態が保存され、動いている世界の変更は入らない ----
    {
        PlayModeController play;
        check(play.PrePlaySnapshot() == nullptr, "no pre-play snapshot while editing");
        play.Play(scene);
        const nlohmann::json* prePlay = play.PrePlaySnapshot();
        check(prePlay != nullptr, "pre-play snapshot exists while playing");
        scene.CreateGameObjectTracked("spawned-while-playing");
        scene.GetWorld().ApplyStructuralChanges();
        const nlohmann::json playing = SceneSerializer::SaveToJson(scene);
        check(playing != editing, "the running world differs from the editing state");
        if (prePlay != nullptr) {
            const RescueSaveResult r = SaveRescueScene(*prePlay, tempRoot.wstring(), L"play",
                                                       RescueTimestamp() + "-play");
            check(r.ok, "pre-play state is saved");
            const nlohmann::json reloaded = r.ok ? ReloadAsJson(r.path) : nlohmann::json();
            check(reloaded == editing, "play: the saved file reloads as the pre-play editing state");
            check(reloaded != playing, "play: the running world's changes are not saved");
        }
        play.Stop(scene);
    }

    // ---- 書けない場所では失敗を返す (落とさない) ----
    {
        const fs::path blocker = tempRoot / L"blocker";
        {
            std::ofstream f(blocker, std::ios::binary);
            f << "a file where a directory is needed";
        }
        const RescueSaveResult r = SaveRescueScene(editing, blocker.wstring(), L"main", "20260101-000000");
        check(!r.ok && !r.error.empty(), "an unwritable location fails with a reason");
    }

    fs::remove_all(tempRoot, ec);
    MYE_LOG_INFO("DeviceLostRescue self test: %s", failCount == 0 ? "ALL PASS" : "FAILED");
    return failCount == 0;
}

} // namespace mye
