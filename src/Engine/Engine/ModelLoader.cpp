#include "Engine/Engine/ModelLoader.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/ShaderManager.h"

#include "cgltf/cgltf.h"

using namespace DirectX;

namespace mye::ModelLoader {
namespace {

struct LoadContext {
    Scene* scene = nullptr;
    RenderResources* resources = nullptr;
    std::string pathUtf8;   // AssetID 用のキー
    std::wstring baseDir;   // 外部テクスチャの解決基準
    AssetID shaderId;
};

// 右手系 → 左手系: 位置/法線は z 反転、クォータニオンは (-x, -y, z, w)
XMFLOAT3 FlipZ(const float* v) { return { v[0], v[1], -v[2] }; }

AssetID LoadPrimitiveMesh(LoadContext& lc, const cgltf_primitive* prim, const char* key)
{
    const cgltf_accessor* posAcc = nullptr;
    const cgltf_accessor* nrmAcc = nullptr;
    const cgltf_accessor* uvAcc = nullptr;
    for (cgltf_size a = 0; a < prim->attributes_count; ++a) {
        const cgltf_attribute& attr = prim->attributes[a];
        if (attr.type == cgltf_attribute_type_position) {
            posAcc = attr.data;
        } else if (attr.type == cgltf_attribute_type_normal) {
            nrmAcc = attr.data;
        } else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) {
            uvAcc = attr.data;
        }
    }
    if (!posAcc || prim->type != cgltf_primitive_type_triangles) {
        MYE_LOG_WARN("glTF: unsupported primitive (no POSITION or not triangles): %s", key);
        return {};
    }

    const size_t vertexCount = posAcc->count;
    std::vector<float> pos(vertexCount * 3, 0.0f);
    cgltf_accessor_unpack_floats(posAcc, pos.data(), pos.size());

    std::vector<float> nrm;
    if (nrmAcc && nrmAcc->count == vertexCount) {
        nrm.resize(vertexCount * 3, 0.0f);
        cgltf_accessor_unpack_floats(nrmAcc, nrm.data(), nrm.size());
    }
    std::vector<float> uv;
    if (uvAcc && uvAcc->count == vertexCount) {
        uv.resize(vertexCount * 2, 0.0f);
        cgltf_accessor_unpack_floats(uvAcc, uv.data(), uv.size());
    }

    std::vector<MeshVertex> vertices(vertexCount);
    for (size_t i = 0; i < vertexCount; ++i) {
        vertices[i].position = FlipZ(&pos[i * 3]);
        vertices[i].normal = nrm.empty() ? XMFLOAT3{ 0, 1, 0 } : FlipZ(&nrm[i * 3]);
        vertices[i].uv = uv.empty() ? XMFLOAT2{ 0, 0 } : XMFLOAT2{ uv[i * 2], uv[i * 2 + 1] };
    }

    std::vector<uint32_t> indices;
    if (prim->indices) {
        indices.resize(prim->indices->count);
        for (cgltf_size i = 0; i < prim->indices->count; ++i) {
            indices[i] = static_cast<uint32_t>(cgltf_accessor_read_index(prim->indices, i));
        }
    } else {
        indices.resize(vertexCount);
        for (size_t i = 0; i < vertexCount; ++i) {
            indices[i] = static_cast<uint32_t>(i);
        }
    }
    // Z 反転で裏返るため巻き順も反転 (i1 と i2 を入れ替え)
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        std::swap(indices[i + 1], indices[i + 2]);
    }

    return lc.resources->meshes.Register(key, vertices, indices);
}

AssetID LoadMaterial(LoadContext& lc, const cgltf_material* mat, const char* key)
{
    Material m;
    m.shader = lc.shaderId;
    m.texture = lc.resources->textures.White();
    if (mat) {
        const cgltf_pbr_metallic_roughness& pbr = mat->pbr_metallic_roughness;
        m.baseColor = { pbr.base_color_factor[0], pbr.base_color_factor[1],
                        pbr.base_color_factor[2], pbr.base_color_factor[3] };
        m.transparent = (mat->alpha_mode == cgltf_alpha_mode_blend) ? 1 : 0;

        if (pbr.base_color_texture.texture && pbr.base_color_texture.texture->image) {
            const cgltf_image* img = pbr.base_color_texture.texture->image;
            if (img->buffer_view && img->buffer_view->buffer && img->buffer_view->buffer->data) {
                // GLB 埋め込み
                const auto* bytes =
                    static_cast<const uint8_t*>(img->buffer_view->buffer->data) + img->buffer_view->offset;
                const std::string texKey = lc.pathUtf8 + "#img:" + (img->name ? img->name : key);
                const AssetID tex = lc.resources->textures.CreateFromEncoded(
                    texKey, bytes, img->buffer_view->size);
                if (!tex.IsNull()) {
                    m.texture = tex;
                }
            } else if (img->uri && strncmp(img->uri, "data:", 5) != 0) {
                // 外部ファイル
                const AssetID tex =
                    lc.resources->textures.LoadFile(lc.baseDir + L"\\" + Utf8ToWide(img->uri));
                if (!tex.IsNull()) {
                    m.texture = tex;
                }
            }
        }
    }
    return lc.resources->materials.Register(key, m);
}

void SetNodeTransform(GameObject obj, const cgltf_node* node)
{
    auto* t = obj.GetComponent<LocalTransform>();
    if (!t) {
        return;
    }
    if (node->has_matrix) {
        // glTF の行列は column-major。XMFLOAT4X4 (row-major) には転置ではなく
        // 「列を行として読む」= そのまま並べると転置になる。メモリ順が一致するのは
        // row-vector 規約の row-major と一致するためそのまま流し込んで分解できる
        XMFLOAT4X4 m;
        memcpy(&m, node->matrix, sizeof(float) * 16);
        // 基底変換 F * M * F (F = diag(1,1,-1)) で Z 反転
        XMMATRIX xm = XMLoadFloat4x4(&m);
        const XMMATRIX flip = XMMatrixScaling(1, 1, -1);
        xm = flip * xm * flip;
        XMVECTOR s, r, p;
        if (XMMatrixDecompose(&s, &r, &p, xm)) {
            XMStoreFloat3(&t->scale, s);
            XMStoreFloat4(&t->rotation, r);
            XMStoreFloat3(&t->position, p);
        }
        return;
    }
    if (node->has_translation) {
        t->position = { node->translation[0], node->translation[1], -node->translation[2] };
    }
    if (node->has_rotation) {
        t->rotation = { -node->rotation[0], -node->rotation[1], node->rotation[2], node->rotation[3] };
    }
    if (node->has_scale) {
        t->scale = { node->scale[0], node->scale[1], node->scale[2] };
    }
}

void LoadNode(LoadContext& lc, const cgltf_data* data, const cgltf_node* node, GameObject parent)
{
    GameObject obj = lc.scene->CreateGameObject(node->name ? node->name : "node");
    obj.SetParent(parent);
    SetNodeTransform(obj, node);

    if (node->mesh) {
        const auto meshIndex = static_cast<size_t>(node->mesh - data->meshes);
        for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p) {
            char key[512];
            snprintf(key, sizeof(key), "%s#mesh%zu#prim%zu", lc.pathUtf8.c_str(), meshIndex,
                     static_cast<size_t>(p));
            const cgltf_primitive* prim = &node->mesh->primitives[p];
            const AssetID meshId = LoadPrimitiveMesh(lc, prim, key);
            if (meshId.IsNull()) {
                continue;
            }
            char matKey[512];
            snprintf(matKey, sizeof(matKey), "%s#mat%zd", lc.pathUtf8.c_str(),
                     prim->material ? (prim->material - data->materials) : -1);
            const AssetID matId = LoadMaterial(lc, prim->material, matKey);

            GameObject target = obj;
            if (node->mesh->primitives_count > 1) {
                char primName[64];
                snprintf(primName, sizeof(primName), "prim%zu", static_cast<size_t>(p));
                target = lc.scene->CreateGameObject(primName);
                target.SetParent(obj);
            }
            auto* mr = target.AddComponent<MeshRendererComponent>();
            mr->mesh = meshId;
            mr->material = matId;
        }
    }

    for (cgltf_size c = 0; c < node->children_count; ++c) {
        LoadNode(lc, data, node->children[c], obj);
    }
}

} // namespace

GameObject Load(Scene& scene, RenderResources& resources, ShaderManager& shaders,
                const std::wstring& path)
{
    const std::string utf8 = WideToUtf8(path);

    cgltf_options options = {};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, utf8.c_str(), &data) != cgltf_result_success) {
        MYE_LOG_ERROR("glTF parse failed: %s", utf8.c_str());
        return {};
    }
    if (cgltf_load_buffers(&options, data, utf8.c_str()) != cgltf_result_success) {
        MYE_LOG_ERROR("glTF buffer load failed: %s", utf8.c_str());
        cgltf_free(data);
        return {};
    }

    LoadContext lc;
    lc.scene = &scene;
    lc.resources = &resources;
    lc.pathUtf8 = utf8;
    lc.baseDir = std::filesystem::path(path).parent_path().wstring();
    lc.shaderId = shaders.Load("forward_lit");

    const std::string rootName = std::filesystem::path(path).stem().string();
    GameObject root = scene.CreateGameObject(rootName);

    const cgltf_scene* gscene = data->scene ? data->scene : (data->scenes_count ? &data->scenes[0] : nullptr);
    if (gscene) {
        for (cgltf_size n = 0; n < gscene->nodes_count; ++n) {
            LoadNode(lc, data, gscene->nodes[n], root);
        }
    }

    MYE_LOG_INFO("glTF loaded: %s (%zu meshes, %zu materials)", utf8.c_str(),
                 static_cast<size_t>(data->meshes_count), static_cast<size_t>(data->materials_count));
    cgltf_free(data);
    return root;
}

} // namespace mye::ModelLoader
