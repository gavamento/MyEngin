#include "Engine/Engine/FbxLoader.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "Engine/Core/Components.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/ShaderManager.h"

#include "ufbx/ufbx.h"

using namespace DirectX;

namespace mye::FbxLoader {
namespace {

struct LoadContext {
    Scene* scene = nullptr;
    RenderResources* resources = nullptr;
    std::string pathUtf8; // AssetID 用のキー (正規化パス)
    std::wstring baseDir; // 外部テクスチャの解決基準
    AssetID shaderId;
};

// ufbx_vec3 → XMFLOAT3。左手系への変換は ufbx 側で完了済み (MakeOpts を参照)
XMFLOAT3 ToXm(ufbx_vec3 v)
{
    return { static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z) };
}

std::string StrOf(ufbx_string s)
{
    return std::string(s.data, s.length);
}

// ジオメトリックトランスフォーム用の合成ノードに付ける名前 (Hierarchy 上の識別用)
constexpr char kGeoHelperName[] = "geo";

// ufbx シーンを左手系 (エンジン標準) Y-up + メートル単位に正規化して読み込む共通オプション。
// 右手系 → 左手系の変換 (位置/法線のミラーと巻き順の反転) は ufbx に任せる。
// handedness_conversion_axis は既定が NONE で、その場合 ufbx はジオメトリを鏡像化せず
// ルートスケール -1 に押し込む (= ノードのスケールが負になりエンジン側で破綻する)。
// 明示的に Z を指定してジオメトリ側でミラーさせること。巻き順も ufbx が反転する
// (handedness_conversion_retain_winding は立てない)。
// NOTE: glTF ローダ (ModelLoader.cpp) は cgltf に同等機能が無いため手動 Z 反転のまま。
// 2 ローダで座標変換の規約が分かれるので、どちらかを触るときは両方を確認すること。
//
// ジオメトリックトランスフォーム (FBX の pivot。子には継承されない) は ufbx の
// ヘルパーノードに任せる。頂点にベイクするとメッシュがノード依存になり、
// メッシュ AssetID キー (ノードを含まない) が同一メッシュの複数インスタンス間で
// 衝突して後勝ち上書きされる。ヘルパー化すれば頂点は完全にノード非依存になる。
ufbx_load_opts MakeOpts()
{
    ufbx_load_opts opts = {};
    opts.target_axes = ufbx_axes_left_handed_y_up;
    opts.handedness_conversion_axis = UFBX_MIRROR_AXIS_Z;
    opts.target_unit_meters = 1.0f;
    opts.space_conversion = UFBX_SPACE_CONVERSION_ADJUST_TRANSFORMS;
    opts.generate_missing_normals = true;
    opts.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_HELPER_NODES;
    opts.geometry_transform_helper_name.data = kGeoHelperName;
    opts.geometry_transform_helper_name.length = sizeof(kGeoHelperName) - 1;
    return opts;
}

// メッシュのマテリアルパートを頂点バッファ化する。ジオメトリックトランスフォームは
// ヘルパーノードが持つ (MakeOpts) ので、ここでノード変換を掛けてはいけない
// = 頂点はノード非依存 → 同一メッシュの複数インスタンスでキーが衝突しない。
AssetID LoadMeshPart(LoadContext& lc, const ufbx_mesh* mesh, const ufbx_mesh_part* part,
                     const char* key)
{
    if (part->num_triangles == 0) {
        return {};
    }
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    vertices.reserve(part->num_triangles * 3);
    indices.reserve(part->num_triangles * 3);

    std::vector<uint32_t> triIx(mesh->max_face_triangles * 3);
    for (size_t fi = 0; fi < part->face_indices.count; ++fi) {
        const ufbx_face face = mesh->faces.data[part->face_indices.data[fi]];
        const uint32_t numTri = ufbx_triangulate_face(triIx.data(), triIx.size(), mesh, face);
        for (uint32_t c = 0; c < numTri * 3; ++c) {
            const uint32_t corner = triIx[c];
            const ufbx_vec3 p = ufbx_get_vertex_vec3(&mesh->vertex_position, corner);
            const ufbx_vec3 n = mesh->vertex_normal.exists
                                    ? ufbx_get_vertex_vec3(&mesh->vertex_normal, corner)
                                    : ufbx_vec3{ 0, 1, 0 };
            ufbx_vec2 uv = mesh->vertex_uv.exists ? ufbx_get_vertex_vec2(&mesh->vertex_uv, corner)
                                                  : ufbx_vec2{ 0, 0 };
            MeshVertex mv;
            mv.position = ToXm(p);
            mv.normal = ToXm(n);
            // FBX の UV 原点は左下 → D3D の左上へ V 反転
            mv.uv = { static_cast<float>(uv.x), 1.0f - static_cast<float>(uv.y) };
            vertices.push_back(mv);
            indices.push_back(static_cast<uint32_t>(indices.size()));
        }
    }
    // 巻き順は ufbx が左手系変換時に反転済み (MakeOpts を参照)
    return lc.resources->meshes.Register(key, vertices, indices);
}

AssetID LoadMaterial(LoadContext& lc, const ufbx_material* mat, const char* key)
{
    Material m;
    m.shader = lc.shaderId;
    m.texture = lc.resources->textures.White();
    if (mat) {
        const ufbx_material_map& bc = mat->pbr.base_color;
        if (bc.has_value) {
            m.baseColor = { static_cast<float>(bc.value_vec4.x), static_cast<float>(bc.value_vec4.y),
                            static_cast<float>(bc.value_vec4.z),
                            static_cast<float>(bc.value_vec4.w) };
        }
        if (mat->pbr.metalness.has_value) {
            m.metallic = static_cast<float>(mat->pbr.metalness.value_real);
        }
        if (mat->pbr.roughness.has_value) {
            m.roughness = static_cast<float>(mat->pbr.roughness.value_real);
        }
        const ufbx_texture* tex = bc.texture;
        if (tex) {
            AssetID texId = {};
            if (tex->filename.length > 0) {
                texId = lc.resources->textures.LoadFile(Utf8ToWide(StrOf(tex->filename)),
                                                        /*srgb=*/true); // ベースカラー (M38a)
            }
            if (texId.IsNull() && tex->relative_filename.length > 0) {
                texId = lc.resources->textures.LoadFile(
                    lc.baseDir + L"\\" + Utf8ToWide(StrOf(tex->relative_filename)),
                    /*srgb=*/true);
            }
            if (!texId.IsNull()) {
                m.texture = texId;
            }
        }
    }
    return lc.resources->materials.Register(key, m);
}

void SetNodeTransform(GameObject obj, const ufbx_node* node)
{
    auto* t = obj.GetComponent<LocalTransform>();
    if (!t) {
        return;
    }
    const ufbx_transform& lt = node->local_transform;
    // 左手系への変換は ufbx 側で完了済み (MakeOpts を参照) なのでそのまま写す
    t->position = { static_cast<float>(lt.translation.x), static_cast<float>(lt.translation.y),
                    static_cast<float>(lt.translation.z) };
    t->rotation = { static_cast<float>(lt.rotation.x), static_cast<float>(lt.rotation.y),
                    static_cast<float>(lt.rotation.z), static_cast<float>(lt.rotation.w) };
    t->scale = { static_cast<float>(lt.scale.x), static_cast<float>(lt.scale.y),
                 static_cast<float>(lt.scale.z) };
}

// メッシュ (全マテリアルパート) を対象エンティティ配下に登録する。scene==null なら
// リロード用に AssetID の再登録のみ行う (エンティティは作らない)。
void LoadMeshInto(LoadContext& lc, const ufbx_node* node, GameObject owner)
{
    const ufbx_mesh* mesh = node->mesh;
    const size_t meshId = mesh->element_id;
    // マテリアルはインスタンス毎 (ufbx.h:1323-1326 が mesh->materials を明示的に非推奨としている)。
    // ただしジオメトリヘルパーにはマテリアル接続が移らない (ufbx が remap するのは attrib のみ) ため、
    // ヘルパー側の materials は mesh->materials で埋め戻された値になる。元ノード (親) を見ること。
    const ufbx_node* matNode = node;
    if (matNode->is_geometry_transform_helper && matNode->parent) {
        matNode = matNode->parent;
    }
    const ufbx_material_list& mats =
        (matNode->materials.count > 0) ? matNode->materials : mesh->materials;
    for (size_t pi = 0; pi < mesh->material_parts.count; ++pi) {
        const ufbx_mesh_part* part = &mesh->material_parts.data[pi];
        char key[512];
        snprintf(key, sizeof(key), "%s#mesh%zu#part%zu", lc.pathUtf8.c_str(), meshId, pi);
        const AssetID meshAsset = LoadMeshPart(lc, mesh, part, key);
        if (meshAsset.IsNull()) {
            continue;
        }
        const ufbx_material* mat = (part->index < mats.count) ? mats.data[part->index] : nullptr;
        // マテリアルキーはグローバル (element_id) — メッシュ毎に複製すると
        // 同一マテリアルが AssetID 重複登録され、編集/ホットリロードが 1 箇所で効かない
        char matKey[512];
        if (mat) {
            snprintf(matKey, sizeof(matKey), "%s#mat%u", lc.pathUtf8.c_str(), mat->element_id);
        } else {
            snprintf(matKey, sizeof(matKey), "%s#defaultmat", lc.pathUtf8.c_str());
        }
        const AssetID matAsset = LoadMaterial(lc, mat, matKey);

        if (!lc.scene) {
            continue; // リロード: 登録のみ
        }
        GameObject target = owner;
        if (mesh->material_parts.count > 1) {
            char partName[64];
            snprintf(partName, sizeof(partName), "part%zu", pi);
            target = lc.scene->CreateGameObject(partName);
            target.SetParent(owner);
        }
        auto* mr = target.AddComponent<MeshRendererComponent>();
        mr->mesh = meshAsset;
        mr->material = matAsset;
    }
}

void LoadNode(LoadContext& lc, const ufbx_node* node, GameObject parent)
{
    // ジオメトリヘルパーは合成ノードなので名前が空になりうる (MakeOpts で名前を与えているが保険)
    const char* fallbackName = node->is_geometry_transform_helper ? kGeoHelperName : "node";
    GameObject obj =
        lc.scene->CreateGameObject(node->name.length ? StrOf(node->name).c_str() : fallbackName);
    obj.SetParent(parent);
    SetNodeTransform(obj, node);
    if (node->mesh) {
        LoadMeshInto(lc, node, obj);
    }
    for (size_t c = 0; c < node->children.count; ++c) {
        LoadNode(lc, node->children.data[c], obj);
    }
}

} // namespace

GameObject Load(Scene& scene, RenderResources& resources, ShaderManager& shaders,
                const std::wstring& path)
{
    const std::string utf8 = WideToUtf8(path);
    const ufbx_load_opts opts = MakeOpts();
    ufbx_error error;
    ufbx_scene* fbx = ufbx_load_file(utf8.c_str(), &opts, &error);
    if (!fbx) {
        MYE_LOG_ERROR("FBX parse failed: %s (%s)", utf8.c_str(), StrOf(error.description).c_str());
        return {};
    }

    LoadContext lc;
    lc.scene = &scene;
    lc.resources = &resources;
    lc.pathUtf8 = WideToUtf8(NormalizePathKey(path));
    lc.baseDir = std::filesystem::path(path).parent_path().wstring();
    lc.shaderId = shaders.Load("forward_lit");

    const std::string rootName = std::filesystem::path(path).stem().string();
    GameObject root = scene.CreateGameObject(rootName);
    for (size_t c = 0; c < fbx->root_node->children.count; ++c) {
        LoadNode(lc, fbx->root_node->children.data[c], root);
    }

    size_t helpers = 0;
    for (size_t i = 0; i < fbx->nodes.count; ++i) {
        helpers += fbx->nodes.data[i]->is_geometry_transform_helper ? 1 : 0;
    }
    MYE_LOG_INFO("FBX loaded: %s (%zu meshes, %zu materials, %zu geometry-transform helpers)",
                 utf8.c_str(), fbx->meshes.count, fbx->materials.count, helpers);
    ufbx_free_scene(fbx);
    return root;
}

bool ReloadMeshes(RenderResources& resources, ShaderManager& shaders, const std::wstring& path)
{
    const std::string utf8 = WideToUtf8(path);
    const ufbx_load_opts opts = MakeOpts();
    ufbx_error error;
    ufbx_scene* fbx = ufbx_load_file(utf8.c_str(), &opts, &error);
    if (!fbx) {
        MYE_LOG_ERROR("[reload] FBX reload failed: %s (%s)", utf8.c_str(),
                      StrOf(error.description).c_str());
        return false;
    }

    LoadContext lc;
    lc.scene = nullptr; // エンティティは作らない
    lc.resources = &resources;
    lc.pathUtf8 = WideToUtf8(NormalizePathKey(path));
    lc.baseDir = std::filesystem::path(path).parent_path().wstring();
    lc.shaderId = shaders.Load("forward_lit");

    for (size_t ni = 0; ni < fbx->nodes.count; ++ni) {
        const ufbx_node* node = fbx->nodes.data[ni];
        if (node->mesh) {
            LoadMeshInto(lc, node, GameObject{});
        }
    }
    MYE_LOG_INFO("[reload] FBX meshes reloaded: %s", utf8.c_str());
    ufbx_free_scene(fbx);
    return true;
}

} // namespace mye::FbxLoader
