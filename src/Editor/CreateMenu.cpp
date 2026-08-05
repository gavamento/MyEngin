#include "Editor/CreateMenu.h"

#include <string>

#include "Editor/Selection.h"
#include "Editor/Undo/UndoStack.h"
#include "Engine/Core/Localization.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/EngineLoop.h"
#include "Engine/Engine/EntityNaming.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Renderer/GpuResources.h"

#include "imgui.h"

namespace mye {

namespace {

// メッシュ + 既定マテリアルを付けた GameObject を生成する共通処理 (CreateCube 相当)
GameObject CreateMeshObject(EngineContext& ctx, const char* name, AssetID mesh)
{
    GameObject obj = ctx.scene->CreateGameObjectTracked(name);
    auto* mr = obj.AddComponent<MeshRendererComponent>();
    mr->mesh = mesh;
    mr->material = ctx.resources->materials.Default(*ctx.shaders, ctx.resources->textures);
    return obj;
}

} // namespace

GameObject CreateEmpty(EngineContext& ctx, const char* name)
{
    return ctx.scene->CreateGameObjectTracked(name);
}
GameObject CreateCube(EngineContext& ctx, const char* name)
{
    return CreateMeshObject(ctx, name, ctx.resources->meshes.Cube());
}
GameObject CreateSphere(EngineContext& ctx, const char* name)
{
    return CreateMeshObject(ctx, name, ctx.resources->meshes.Sphere());
}
GameObject CreatePlane(EngineContext& ctx, const char* name)
{
    return CreateMeshObject(ctx, name, ctx.resources->meshes.Plane());
}
GameObject CreateQuad(EngineContext& ctx, const char* name)
{
    return CreateMeshObject(ctx, name, ctx.resources->meshes.Quad());
}
GameObject CreateCylinder(EngineContext& ctx, const char* name)
{
    return CreateMeshObject(ctx, name, ctx.resources->meshes.Cylinder());
}
GameObject CreateCapsule(EngineContext& ctx, const char* name)
{
    return CreateMeshObject(ctx, name, ctx.resources->meshes.Capsule());
}

GameObject CreateDirectionalLight(EngineContext& ctx, const char* name)
{
    GameObject obj = ctx.scene->CreateGameObjectTracked(name);
    obj.AddComponent<LightComponent>(); // 既定 type=0 (平行光)。向き = エンティティ +Z
    return obj;
}

GameObject CreatePointLight(EngineContext& ctx, const char* name)
{
    GameObject obj = ctx.scene->CreateGameObjectTracked(name);
    auto* l = obj.AddComponent<LightComponent>();
    l->type = 1; // Point。位置はエンティティのワールド座標、range で減衰
    return obj;
}

GameObject CreateSpotLight(EngineContext& ctx, const char* name)
{
    GameObject obj = ctx.scene->CreateGameObjectTracked(name);
    auto* l = obj.AddComponent<LightComponent>();
    l->type = 2; // Spot。向き = エンティティ +Z、spotInner/Outer でコーン
    return obj;
}

GameObject CreateCamera(EngineContext& ctx, const char* name)
{
    GameObject obj = ctx.scene->CreateGameObjectTracked(name);
    auto* cam = obj.AddComponent<CameraComponent>();
    cam->isPrimary = 0; // 既存のメインカメラを奪わない
    return obj;
}

GameObject CreateAudioSource(EngineContext& ctx, const char* name)
{
    GameObject obj = ctx.scene->CreateGameObjectTracked(name);
    obj.AddComponent<AudioSourceComponent>(); // sound は未割当 (Inspector か D&D で割り当てる)
    return obj;
}

GameObject CreateAudioListener(EngineContext& ctx, const char* name)
{
    GameObject obj = ctx.scene->CreateGameObjectTracked(name);
    obj.AddComponent<AudioListenerComponent>();
    return obj;
}

GameObject RecordCreate(EngineContext& ctx, Selection& selection, UndoStack& undo, const char* label,
                        const std::function<GameObject()>& make)
{
    undo.BeginRecord(label, selection);
    GameObject obj = make();
    ctx.scene->GetWorld().ApplyStructuralChanges();
    const uint64_t fid = ctx.scene->EnsureFileId(obj.Id());
    selection.SelectOnly(fid);
    undo.CaptureAfter(*ctx.scene, fid);
    undo.EndRecord(selection);
    return obj;
}

namespace {

using Factory = GameObject (*)(EngineContext&, const char*);

// 生成メニュー項目 1 つ。クリックで factory の結果を (parent があれば子として、
// spawnPos があればその位置に) 生成する。
void CreateItem(EngineContext& ctx, Selection& selection, UndoStack& undo, EntityID parent,
                const DirectX::XMFLOAT3* spawnPos, const char* menuLabel, const char* objName,
                Factory factory)
{
    if (ImGui::MenuItem(menuLabel)) {
        const std::string undoLabel = std::string("Create ") + objName;
        RecordCreate(ctx, selection, undo, undoLabel.c_str(), [&] {
            // 兄弟名の一意化 (M48b)。**生成前**に決めるので exclude は不要 (対象はまだ存在しない)。
            // SetParent は遅延なので world.GetParent(obj) はまだ使えず、引数 parent が権威
            const std::string unique = MakeUniqueSiblingName(ctx.scene->GetWorld(), parent, objName,
                                                             /*exclude=*/kNullEntity);
            GameObject obj = factory(ctx, unique.c_str());
            if (!parent.IsNull()) {
                ctx.scene->GetWorld().SetParent(obj.Id(), parent);
            }
            if (spawnPos) {
                obj.SetLocalPosition(spawnPos->x, spawnPos->y, spawnPos->z);
            }
            return obj;
        });
    }
}

} // namespace

void DrawCreateMenuItems(EngineContext& ctx, Selection& selection, UndoStack& undo, EntityID parent,
                         const DirectX::XMFLOAT3* spawnPos)
{
    CreateItem(ctx, selection, undo, parent, spawnPos, Tr(StrId::Create_Empty), "GameObject", &CreateEmpty);
    if (ImGui::BeginMenu(Tr(StrId::Create_3DObject))) {
        CreateItem(ctx, selection, undo, parent, spawnPos, Tr(StrId::Create_Cube), "Cube", &CreateCube);
        CreateItem(ctx, selection, undo, parent, spawnPos, Tr(StrId::Create_Sphere), "Sphere", &CreateSphere);
        CreateItem(ctx, selection, undo, parent, spawnPos, Tr(StrId::Create_Plane), "Plane", &CreatePlane);
        CreateItem(ctx, selection, undo, parent, spawnPos, Tr(StrId::Create_Quad), "Quad", &CreateQuad);
        CreateItem(ctx, selection, undo, parent, spawnPos, Tr(StrId::Create_Cylinder), "Cylinder", &CreateCylinder);
        CreateItem(ctx, selection, undo, parent, spawnPos, Tr(StrId::Create_Capsule), "Capsule", &CreateCapsule);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(Tr(StrId::Create_Light))) {
        CreateItem(ctx, selection, undo, parent, spawnPos, Tr(StrId::Create_DirLight), "Directional Light",
                   &CreateDirectionalLight);
        CreateItem(ctx, selection, undo, parent, spawnPos, Tr(StrId::Create_PointLight), "Point Light",
                   &CreatePointLight);
        CreateItem(ctx, selection, undo, parent, spawnPos, Tr(StrId::Create_SpotLight), "Spot Light",
                   &CreateSpotLight);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(Tr(StrId::Create_Audio))) {
        CreateItem(ctx, selection, undo, parent, spawnPos, Tr(StrId::Create_AudioSource), "Audio Source",
                   &CreateAudioSource);
        CreateItem(ctx, selection, undo, parent, spawnPos, Tr(StrId::Create_AudioListener), "Audio Listener",
                   &CreateAudioListener);
        ImGui::EndMenu();
    }
    CreateItem(ctx, selection, undo, parent, spawnPos, Tr(StrId::Create_Camera), "Camera", &CreateCamera);

    // 部位 (範囲付き、M49)。汎用 CreateItem を使わないのは、右クリック対象 (parent) の
    // メッシュ AABB へ範囲をフィットさせる親依存の初期化があるため
    if (ImGui::MenuItem(Tr(StrId::Create_Part))) {
        RecordCreate(ctx, selection, undo, "Create Part", [&] {
            const std::string unique =
                MakeUniqueSiblingName(ctx.scene->GetWorld(), parent, "Part", /*exclude=*/kNullEntity);
            GameObject obj = ctx.scene->CreateGameObjectTracked(unique.c_str());
            obj.AddComponent<PartComponent>(); // tag = 0 (未分類)。Inspector の combo で選ぶ
            auto* pb = obj.AddComponent<PartBoundsComponent>();
            if (!parent.IsNull()) {
                ctx.scene->GetWorld().SetParent(obj.Id(), parent);
                // 親のメッシュ AABB に箱をフィット。子の変換は恒等のまま作られるので、
                // 親ローカルの AABB 寸法をそのまま center/halfExtents に写せる
                if (const auto* mr =
                        ctx.scene->GetWorld().GetComponent<MeshRendererComponent>(parent)) {
                    if (const Mesh* mesh = ctx.resources->meshes.Get(mr->mesh)) {
                        const DirectX::XMFLOAT3& lo = mesh->aabbMin;
                        const DirectX::XMFLOAT3& hi = mesh->aabbMax;
                        pb->center = { (lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f,
                                       (lo.z + hi.z) * 0.5f };
                        pb->halfExtents = { (hi.x - lo.x) * 0.5f, (hi.y - lo.y) * 0.5f,
                                            (hi.z - lo.z) * 0.5f };
                    }
                }
            }
            if (spawnPos) {
                obj.SetLocalPosition(spawnPos->x, spawnPos->y, spawnPos->z);
            }
            return obj;
        });
    }
}

} // namespace mye
