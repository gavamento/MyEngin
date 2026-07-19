#include "Engine/Engine/SceneSelfTest.h"

#include <unordered_map>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/SceneSerializer.h"

namespace mye {

bool RunSceneSerializerSelfTest()
{
    MYE_LOG_INFO("==== Scene serializer self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- テストシーン構築 ----
    Scene scene;
    scene.SetName("SerializerTest");

    GameObject parent = scene.CreateGameObject("Parent");
    parent.SetLocalPosition(1.0f, 2.0f, 3.0f);
    GameObject child = scene.CreateGameObject("Child");
    child.SetParent(parent);
    child.SetLocalPosition(0.5f, 0.0f, 0.0f);
    auto* mr = child.AddComponent<MeshRendererComponent>();
    mr->mesh = AssetID{ 0xABCDull };
    mr->material = AssetID{ 0x1234ull };
    GameObject cam = scene.CreateGameObject("Camera");
    auto* cc = cam.AddComponent<CameraComponent>();
    cc->fovYDeg = 42.0f;
    scene.GetWorld().ApplyStructuralChanges(); // SetParent 反映

    // ---- 保存 → 読込 → 再保存 ----
    const nlohmann::json first = SceneSerializer::SaveToJson(scene);
    check(first["entities"].size() == 3, "3 entities saved");

    check(SceneSerializer::LoadFromJson(scene, first), "load succeeds");
    check(scene.GetWorld().AliveCount() == 3, "3 entities after load");

    GameObject child2 = scene.Find("Child");
    check(static_cast<bool>(child2), "Child found after load");
    if (child2) {
        auto* lt = child2.GetComponent<LocalTransform>();
        check(lt && lt->position.x == 0.5f, "Child position restored");
        auto* mr2 = child2.GetComponent<MeshRendererComponent>();
        check(mr2 && mr2->mesh.value == 0xABCDull, "Child MeshRenderer restored");
        GameObject parent2 = scene.Find("Parent");
        check(parent2 && child2.GetWorld()->GetParent(child2.Id()) == parent2.Id(),
              "parent relation restored");
    }
    GameObject cam2 = scene.Find("Camera");
    if (cam2) {
        auto* cc2 = cam2.GetComponent<CameraComponent>();
        check(cc2 && cc2->fovYDeg == 42.0f, "Camera fov restored");
    }

    // ---- ラウンドトリップ一致 (fileId 単位で比較 — 配列順はアーキタイプ順に依存するため) ----
    const nlohmann::json second = SceneSerializer::SaveToJson(scene);
    auto mapByFileId = [](const nlohmann::json& root) {
        std::unordered_map<uint64_t, nlohmann::json> map;
        for (const auto& item : root["entities"]) {
            map[item.value("fileId", 0ull)] = item;
        }
        return map;
    };
    const auto a = mapByFileId(first);
    const auto b = mapByFileId(second);
    bool identical = (a.size() == b.size());
    if (identical) {
        for (const auto& [fid, item] : a) {
            auto it = b.find(fid);
            if (it == b.end() || it->second != item) {
                identical = false;
                MYE_LOG_ERROR("  roundtrip mismatch at fileId=%llu",
                              static_cast<unsigned long long>(fid));
                break;
            }
        }
    }
    check(identical, "save -> load -> save roundtrip identical");

    if (failCount == 0) {
        MYE_LOG_INFO("==== Scene serializer self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Scene serializer self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
