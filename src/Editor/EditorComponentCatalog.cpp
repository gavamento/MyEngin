#include "Editor/EditorComponentCatalog.h"

#include <cstring>
#include <string>
#include <unordered_map>

#include "Engine/Core/ComponentRegistry.h"
#include "Engine/Core/Localization.h"
#include "Engine/Core/World.h"

#include "fontawesome/IconsFontAwesome6.h"

namespace mye {

namespace {

const std::unordered_map<std::string, ComponentUiInfo>& Table()
{
    static const std::unordered_map<std::string, ComponentUiInfo> t = {
        // General
        { "Name", { ICON_FA_FONT, "General", "名前" } },
        { "LocalTransform", { ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT, "General", "トランスフォーム" } },
        { "Active", { ICON_FA_EYE, "General", "アクティブ" } },
        // M48f: 部位 (ソケット)。アタッチ先を公開する構造なので General 扱い
        { "Part", { ICON_FA_ANCHOR, "General", "部位" } },
        // M49: 部位の範囲 (箱/球ボリューム)。Part の添え物なので隣に置く
        { "PartBounds", { ICON_FA_BULLSEYE, "General", "部位の範囲" } },
        // Rendering
        { "MeshRenderer", { ICON_FA_CUBE, "Rendering", "メッシュレンダラー" } },
        { "SkinnedMesh", { ICON_FA_PERSON, "Rendering", "スキンメッシュ" } },
        { "Camera", { ICON_FA_VIDEO, "Rendering", "カメラ" } },
        { "Light", { ICON_FA_LIGHTBULB, "Rendering", "ライト" } },
        // Physics
        { "Collider", { ICON_FA_VECTOR_SQUARE, "Physics", "コライダー" } },
        { "Rigidbody", { ICON_FA_CIRCLE_DOT, "Physics", "リジッドボディ" } },
        { "ConstantForce", { ICON_FA_BOLT, "Physics", "定常力" } },
        { "SpringJoint", { ICON_FA_LINK, "Physics", "スプリングジョイント" } },
        { "CharacterController", { ICON_FA_PERSON_RUNNING, "Physics", "キャラクターコントローラー" } },
        // Animation
        { "Animator", { ICON_FA_FILM, "Animation", "アニメーター" } },
        { "AnimatorController", { ICON_FA_CIRCLE_NODES, "Animation", "アニメーターコントローラー" } },
        // VFX
        { "ParticleEmitter", { ICON_FA_FIRE, "VFX", "パーティクルエミッタ" } },
        { "SpriteRenderer", { ICON_FA_IMAGE, "VFX", "スプライトレンダラー" } },
        { "TrailRenderer", { ICON_FA_WIND, "VFX", "トレイルレンダラー" } },
        { "TextMesh", { ICON_FA_FONT, "VFX", "テキストメッシュ" } },
        { "Effect", { ICON_FA_WAND_MAGIC_SPARKLES, "VFX", "エフェクト" } },
        // Audio
        { "AudioSource", { ICON_FA_VOLUME_HIGH, "Audio", "オーディオソース" } },
        { "AudioListener", { ICON_FA_HEADPHONES, "Audio", "オーディオリスナー" } },
        // Environment
        { "Skybox", { ICON_FA_CLOUD, "Environment", "スカイボックス" } },
        { "Fog", { ICON_FA_SMOG, "Environment", "フォグ" } },
        { "CameraPostFx", { ICON_FA_SLIDERS, "Environment", "ポストエフェクト" } },
        // UI
        { "UIElement", { ICON_FA_WINDOW_MAXIMIZE, "UI", "UI 要素" } },
    };
    return t;
}

} // namespace

const ComponentUiInfo& ComponentUiFor(const char* name)
{
    static const ComponentUiInfo kScript = { ICON_FA_CODE, "Scripts", nullptr };
    const auto& t = Table();
    const auto it = t.find(name);
    return it != t.end() ? it->second : kScript;
}

const char* ComponentDisplayName(const char* name)
{
    const ComponentUiInfo& info = ComponentUiFor(name);
    return (info.ja != nullptr && CurrentLanguage() != Lang::En) ? info.ja : name;
}

const std::vector<const char*>& ComponentUiCategories()
{
    static const std::vector<const char*> cats = { "General",   "Rendering", "Physics",
                                                   "Animation", "VFX",       "Audio",
                                                   "Environment", "UI",      "Scripts" };
    return cats;
}

const char* ComponentCategoryLabel(const char* categoryKey)
{
    // キー (英語) は InspectorWindow の strcmp 照合に使うので、表示だけを差し替える
    if (CurrentLanguage() == Lang::En || categoryKey == nullptr) {
        return categoryKey;
    }
    struct Row { const char* key; const char* ja; };
    static const Row kRows[] = {
        { "General", "一般" },      { "Rendering", "レンダリング" }, { "Physics", "物理" },
        { "Animation", "アニメーション" }, { "VFX", "エフェクト" },  { "Audio", "オーディオ" },
        { "Environment", "環境" },  { "UI", "UI" },                  { "Scripts", "スクリプト" },
    };
    for (const Row& r : kRows) {
        if (std::strcmp(r.key, categoryKey) == 0) {
            return r.ja;
        }
    }
    return categoryKey;
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
