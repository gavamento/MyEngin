#include "Editor/CreateMenu.h"

#include <string>

#include "Editor/Selection.h"
#include "Editor/Undo/UndoStack.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/EngineLoop.h"
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

// 生成メニュー項目 1 つ。クリックで factory の結果を (parent があれば子として) 生成する。
void CreateItem(EngineContext& ctx, Selection& selection, UndoStack& undo, EntityID parent,
                const char* menuLabel, const char* objName, Factory factory)
{
    if (ImGui::MenuItem(menuLabel)) {
        const std::string undoLabel = std::string("Create ") + objName;
        RecordCreate(ctx, selection, undo, undoLabel.c_str(), [&] {
            GameObject obj = factory(ctx, objName);
            if (!parent.IsNull()) {
                ctx.scene->GetWorld().SetParent(obj.Id(), parent);
            }
            return obj;
        });
    }
}

} // namespace

void DrawCreateMenuItems(EngineContext& ctx, Selection& selection, UndoStack& undo, EntityID parent)
{
    CreateItem(ctx, selection, undo, parent, "Create Empty", "GameObject", &CreateEmpty);
    if (ImGui::BeginMenu("3D Object")) {
        CreateItem(ctx, selection, undo, parent, "Cube", "Cube", &CreateCube);
        CreateItem(ctx, selection, undo, parent, "Sphere", "Sphere", &CreateSphere);
        CreateItem(ctx, selection, undo, parent, "Plane", "Plane", &CreatePlane);
        CreateItem(ctx, selection, undo, parent, "Quad", "Quad", &CreateQuad);
        CreateItem(ctx, selection, undo, parent, "Cylinder", "Cylinder", &CreateCylinder);
        CreateItem(ctx, selection, undo, parent, "Capsule", "Capsule", &CreateCapsule);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Light")) {
        CreateItem(ctx, selection, undo, parent, "Directional Light", "Directional Light",
                   &CreateDirectionalLight);
        CreateItem(ctx, selection, undo, parent, "Point Light", "Point Light", &CreatePointLight);
        CreateItem(ctx, selection, undo, parent, "Spot Light", "Spot Light", &CreateSpotLight);
        ImGui::EndMenu();
    }
    CreateItem(ctx, selection, undo, parent, "Camera", "Camera", &CreateCamera);
}

} // namespace mye
