#include "Editor/EditorComponentCatalog.h"

#include <cstring>
#include <string>
#include <unordered_map>

#include "Engine/Core/ComponentRegistry.h"
#include "Engine/Core/World.h"

#include "fontawesome/IconsFontAwesome6.h"

namespace mye {

namespace {

const std::unordered_map<std::string, ComponentUiInfo>& Table()
{
    static const std::unordered_map<std::string, ComponentUiInfo> t = {
        // General
        { "Name", { ICON_FA_FONT, "General" } },
        { "LocalTransform", { ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT, "General" } },
        { "Active", { ICON_FA_EYE, "General" } },
        // Rendering
        { "MeshRenderer", { ICON_FA_CUBE, "Rendering" } },
        { "SkinnedMesh", { ICON_FA_PERSON, "Rendering" } },
        { "Camera", { ICON_FA_VIDEO, "Rendering" } },
        { "Light", { ICON_FA_LIGHTBULB, "Rendering" } },
        // Physics
        { "Collider", { ICON_FA_VECTOR_SQUARE, "Physics" } },
        { "Rigidbody", { ICON_FA_CIRCLE_DOT, "Physics" } },
        { "ConstantForce", { ICON_FA_BOLT, "Physics" } },
        { "SpringJoint", { ICON_FA_LINK, "Physics" } },
        { "CharacterController", { ICON_FA_PERSON_RUNNING, "Physics" } },
        // Animation
        { "Animator", { ICON_FA_FILM, "Animation" } },
        { "AnimatorController", { ICON_FA_CIRCLE_NODES, "Animation" } },
        // VFX
        { "ParticleEmitter", { ICON_FA_FIRE, "VFX" } },
        { "SpriteRenderer", { ICON_FA_IMAGE, "VFX" } },
        { "TrailRenderer", { ICON_FA_WIND, "VFX" } },
        { "TextMesh", { ICON_FA_FONT, "VFX" } },
        { "Effect", { ICON_FA_WAND_MAGIC_SPARKLES, "VFX" } },
        // Audio
        { "AudioSource", { ICON_FA_VOLUME_HIGH, "Audio" } },
        { "AudioListener", { ICON_FA_HEADPHONES, "Audio" } },
        // Environment
        { "Skybox", { ICON_FA_CLOUD, "Environment" } },
        { "Fog", { ICON_FA_SMOG, "Environment" } },
        { "CameraPostFx", { ICON_FA_SLIDERS, "Environment" } },
        // UI
        { "UIElement", { ICON_FA_WINDOW_MAXIMIZE, "UI" } },
    };
    return t;
}

} // namespace

const ComponentUiInfo& ComponentUiFor(const char* name)
{
    static const ComponentUiInfo kScript = { ICON_FA_CODE, "Scripts" };
    const auto& t = Table();
    const auto it = t.find(name);
    return it != t.end() ? it->second : kScript;
}

const std::vector<const char*>& ComponentUiCategories()
{
    static const std::vector<const char*> cats = { "General",   "Rendering", "Physics",
                                                   "Animation", "VFX",       "Audio",
                                                   "Environment", "UI",      "Scripts" };
    return cats;
}

const char* EntityIconFor(World& world, EntityID e)
{
    const Archetype* arch = world.GetArchetype(e);
    if (!arch) {
        return ICON_FA_CIRCLE;
    }
    const ComponentRegistry& reg = ComponentRegistry::Get();
    // TypeId 昇順 = ビルトインが先。最初に見つかった非 General の登録済みコンポーネントが代表
    for (ComponentTypeId t : arch->Types()) {
        const ComponentDesc& desc = reg.Desc(t);
        if (desc.flags & kComponentHidden) {
            continue;
        }
        const auto& table = Table();
        const auto it = table.find(desc.name);
        if (it == table.end()) {
            return ICON_FA_CODE; // スクリプトコンポーネント
        }
        if (std::strcmp(it->second.category, "General") == 0) {
            continue;
        }
        return it->second.icon;
    }
    return ICON_FA_CIRCLE; // 空のグループノード
}

} // namespace mye
