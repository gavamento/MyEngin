#include "Engine/Engine/HotReload/ReloadHubSelfTest.h"

#include <string>

#include "Engine/Core/Log.h"
#include "Engine/Engine/HotReload/ReloadHub.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {

bool RunReloadHubSelfTest()
{
    MYE_LOG_INFO("==== ReloadHub (asset kind table) self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const std::string& what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what.c_str());
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what.c_str());
            ++failCount;
        }
    };

    // パスは NormalizePathKey 済みの形 (小文字 + '\\')
    struct Case {
        const wchar_t* path;
        ReloadKind kind;
        int rank;
    };
    const Case cases[] = {
        { L"c:\\p\\assets\\shaders\\lit.hlsl", ReloadKind::Shader, 0 },
        { L"c:\\p\\assets\\shaders\\common.hlsli", ReloadKind::Shader, 0 },
        { L"c:\\p\\assets\\textures\\wall.png", ReloadKind::Texture, 1 },
        { L"c:\\p\\assets\\textures\\wall.tga", ReloadKind::Texture, 1 },
        { L"c:\\p\\assets\\textures\\wall.jpg", ReloadKind::Texture, 1 },
        { L"c:\\p\\assets\\textures\\wall.jpeg", ReloadKind::Texture, 1 },
        { L"c:\\p\\assets\\textures\\wall.dds", ReloadKind::Texture, 1 },
        { L"c:\\p\\assets\\audio\\step.wav", ReloadKind::AudioClip, 2 },
        { L"c:\\p\\assets\\audio\\step.ogg", ReloadKind::AudioClip, 2 },
        { L"c:\\p\\assets\\model\\hero.glb", ReloadKind::Gltf, 4 },
        { L"c:\\p\\assets\\model\\hero.gltf", ReloadKind::Gltf, 4 },
        { L"c:\\p\\assets\\model\\hero.fbx", ReloadKind::Fbx, 4 },
        { L"c:\\p\\assets\\materials\\wall.mat.json", ReloadKind::Material, 3 },
        { L"c:\\p\\assets\\anims\\walk.anim.json", ReloadKind::Anim, 5 },
        { L"c:\\p\\assets\\audio\\door.sound.json", ReloadKind::Sound, 6 },
        { L"c:\\p\\assets\\audio\\impact\\glass.impact.json", ReloadKind::ImpactSound, 6 },
        { L"c:\\p\\assets\\audio\\default.mixer.json", ReloadKind::Mixer, 6 },
        { L"c:\\p\\assets\\physics\\metal.physmat.json", ReloadKind::PhysMat, 6 },
        { L"c:\\p\\assets\\deepmodal\\deepmodal.dmnet", ReloadKind::ModalNet, 6 },
        { L"c:\\p\\assets\\actors\\hero.actor.json", ReloadKind::Compose, 7 },
        { L"c:\\p\\assets\\prefabs\\door.prefab.json", ReloadKind::Compose, 7 },
        { L"c:\\p\\assets\\scenes\\main.scene.json", ReloadKind::Scene, 8 },
        // 未知の .json も「開いているシーンなら差分適用」の対象。順位は最後
        { L"c:\\p\\assets\\notes.json", ReloadKind::Scene, 9 },
        // どの行にも当たらない (.hlsl.bak は末尾が .hlsl ではない)
        { L"c:\\p\\assets\\readme.txt", ReloadKind::None, 9 },
        { L"c:\\p\\assets\\shaders\\lit.hlsl.bak", ReloadKind::None, 9 },
        { L"c:\\p\\assets\\noext", ReloadKind::None, 9 },
    };
    for (const Case& c : cases) {
        const std::wstring path = c.path;
        const ReloadKind kind = ReloadKindOf(path);
        const int rank = ReloadRank(path);
        check(kind == c.kind && rank == c.rank,
              "kind / rank of " + WideToUtf8(path) + " (kind=" + std::to_string(static_cast<int>(kind))
                  + " rank=" + std::to_string(rank) + ")");
    }

    MYE_LOG_INFO("ReloadHub self test: %s (%d failure(s))", failCount == 0 ? "OK" : "FAILED", failCount);
    return failCount == 0;
}

} // namespace mye
