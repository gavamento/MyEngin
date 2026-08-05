#include "Engine/Engine/ModelLoader.h"

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

// glTF 行列 (column-major) を行ベクトル規約の XMFLOAT4X4 に読み、基底変換 F*M*F (F=diag(1,1,-1)) で
// Z 反転する。ModelLoader::SetNodeTransform の has_matrix 経路と同じ扱い。
XMMATRIX FlipZMatrix(const float* colMajor16)
{
    XMFLOAT4X4 m;
    memcpy(&m, colMajor16, sizeof(float) * 16); // 列優先を行優先に流し込む = row-vector 形
    const XMMATRIX flip = XMMatrixScaling(1, 1, -1);
    return flip * XMLoadFloat4x4(&m) * flip;
}

AssetID LoadPrimitiveMesh(LoadContext& lc, const cgltf_primitive* prim, const char* key)
{
    const cgltf_accessor* posAcc = nullptr;
    const cgltf_accessor* nrmAcc = nullptr;
    const cgltf_accessor* uvAcc = nullptr;
    const cgltf_accessor* jointAcc = nullptr;  // JOINTS_0 (VEC4 uint)
    const cgltf_accessor* weightAcc = nullptr; // WEIGHTS_0 (VEC4 float)
    for (cgltf_size a = 0; a < prim->attributes_count; ++a) {
        const cgltf_attribute& attr = prim->attributes[a];
        if (attr.type == cgltf_attribute_type_position) {
            posAcc = attr.data;
        } else if (attr.type == cgltf_attribute_type_normal) {
            nrmAcc = attr.data;
        } else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) {
            uvAcc = attr.data;
        } else if (attr.type == cgltf_attribute_type_joints && attr.index == 0) {
            jointAcc = attr.data;
        } else if (attr.type == cgltf_attribute_type_weights && attr.index == 0) {
            weightAcc = attr.data;
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
        // スキニング (M18): JOINTS_0(uint4)→u8x4、WEIGHTS_0(float4)。無ければ weight 0 (非スキン)
        if (jointAcc && weightAcc) {
            cgltf_uint ji[4] = { 0, 0, 0, 0 };
            float wt[4] = { 0, 0, 0, 0 };
            cgltf_accessor_read_uint(jointAcc, i, ji, 4);
            cgltf_accessor_read_float(weightAcc, i, wt, 4);
            for (int k = 0; k < 4; ++k) {
                vertices[i].boneIndices[k] = static_cast<uint8_t>(ji[k] & 0xFF);
            }
            vertices[i].boneWeights = { wt[0], wt[1], wt[2], wt[3] };
        }
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
        m.metallic = pbr.metallic_factor;   // PBR (M17)
        m.roughness = pbr.roughness_factor;
        m.transparent = (mat->alpha_mode == cgltf_alpha_mode_blend) ? 1 : 0;

        if (pbr.base_color_texture.texture && pbr.base_color_texture.texture->image) {
            const cgltf_image* img = pbr.base_color_texture.texture->image;
            if (img->buffer_view && img->buffer_view->buffer && img->buffer_view->buffer->data) {
                // GLB 埋め込み
                const auto* bytes =
                    static_cast<const uint8_t*>(img->buffer_view->buffer->data) + img->buffer_view->offset;
                const std::string texKey = lc.pathUtf8 + "#img:" + (img->name ? img->name : key);
                const AssetID tex = lc.resources->textures.CreateFromEncoded(
                    texKey, bytes, img->buffer_view->size, /*srgb=*/true); // ベースカラー (M38a)
                if (!tex.IsNull()) {
                    m.texture = tex;
                }
            } else if (img->uri && strncmp(img->uri, "data:", 5) != 0) {
                // 外部ファイル
                const AssetID tex = lc.resources->textures.LoadFile(
                    lc.baseDir + L"\\" + Utf8ToWide(img->uri), /*srgb=*/true); // ベースカラー (M38a)
                if (!tex.IsNull()) {
                    m.texture = tex;
                }
            }
        }
    }
    return lc.resources->materials.Register(key, m);
}

// glTF skin → SkinnedModel (スケルトン + 全クリップ) を構築・登録し AssetID を返す (M18)。
// ジョイントグローバルはジョイントツリー内だけで計算する規約 — ジョイント以外の祖先
// (Armature/Z_UP 等) の変換はメッシュエンティティの world 行列 (gWorld) が担う。
AssetID LoadSkin(LoadContext& lc, const cgltf_data* data, const cgltf_skin* skin, size_t skinIdx)
{
    const cgltf_size jn = skin->joints_count;
    SkinnedModel model;
    model.joints.resize(jn);

    auto jointIndexOf = [&](const cgltf_node* n) -> int {
        for (cgltf_size j = 0; j < jn; ++j) {
            if (skin->joints[j] == n) {
                return static_cast<int>(j);
            }
        }
        return -1;
    };

    for (cgltf_size j = 0; j < jn; ++j) {
        const cgltf_node* jnode = skin->joints[j];
        SkeletonJoint& out = model.joints[j];
        // 親 = 最も近いジョイント祖先 (途中の非ジョイント祖先は gWorld が担うので飛ばす)
        int parent = -1;
        for (const cgltf_node* p = jnode->parent; p; p = p->parent) {
            const int pj = jointIndexOf(p);
            if (pj >= 0) {
                parent = pj;
                break;
            }
        }
        out.parent = parent;
        out.name = jnode->name ? jnode->name : ""; // 部位ソケットの joint 名参照 (M48a)
        if (skin->inverse_bind_matrices) {
            float ib[16];
            cgltf_accessor_read_float(skin->inverse_bind_matrices, j, ib, 16);
            XMStoreFloat4x4(&out.inverseBind, FlipZMatrix(ib)); // 列優先→行ベクトル + Z 反転
        }
        float localMat[16];
        cgltf_node_transform_local(jnode, localMat);
        XMVECTOR s, r, t;
        if (XMMatrixDecompose(&s, &r, &t, FlipZMatrix(localMat))) {
            XMStoreFloat3(&out.bindT, t);
            XMStoreFloat4(&out.bindR, r);
            XMStoreFloat3(&out.bindS, s);
        }
    }

    // アニメーション: 全 animation の channel をこの skin のジョイントへ振り分ける
    for (cgltf_size ai = 0; ai < data->animations_count; ++ai) {
        const cgltf_animation& anim = data->animations[ai];
        SkeletalClip clip;
        clip.name = anim.name ? anim.name : "";
        clip.tracks.resize(jn);
        float dur = 0.0f;
        for (cgltf_size ci = 0; ci < anim.channels_count; ++ci) {
            const cgltf_animation_channel& ch = anim.channels[ci];
            const int jidx = jointIndexOf(ch.target_node);
            if (jidx < 0 || !ch.sampler) {
                continue;
            }
            const cgltf_accessor* in = ch.sampler->input;   // 時刻 (scalar, 秒)
            const cgltf_accessor* val = ch.sampler->output; // 値
            const cgltf_size kc = in->count;
            std::vector<float> times(kc);
            for (cgltf_size k = 0; k < kc; ++k) {
                cgltf_accessor_read_float(in, k, &times[k], 1);
                dur = (times[k] > dur) ? times[k] : dur;
            }
            JointTrack& tr = clip.tracks[static_cast<size_t>(jidx)];
            if (ch.target_path == cgltf_animation_path_type_translation) {
                tr.tTimes = times;
                tr.tVals.resize(kc);
                for (cgltf_size k = 0; k < kc; ++k) {
                    float v[3];
                    cgltf_accessor_read_float(val, k, v, 3);
                    tr.tVals[k] = { v[0], v[1], -v[2] }; // Z 反転
                }
            } else if (ch.target_path == cgltf_animation_path_type_rotation) {
                tr.rTimes = times;
                tr.rVals.resize(kc);
                for (cgltf_size k = 0; k < kc; ++k) {
                    float v[4];
                    cgltf_accessor_read_float(val, k, v, 4);
                    tr.rVals[k] = { -v[0], -v[1], v[2], v[3] }; // クォータニオン Z 反転
                }
            } else if (ch.target_path == cgltf_animation_path_type_scale) {
                tr.sTimes = times;
                tr.sVals.resize(kc);
                for (cgltf_size k = 0; k < kc; ++k) {
                    float v[3];
                    cgltf_accessor_read_float(val, k, v, 3);
                    tr.sVals[k] = { v[0], v[1], v[2] };
                }
            }
        }
        clip.duration = dur;
        model.clips.push_back(std::move(clip));
    }

    char key[512];
    snprintf(key, sizeof(key), "%s#skin%zu", lc.pathUtf8.c_str(), skinIdx);
    return lc.resources->skinnedModels.Register(key, std::move(model));
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
        // スキン付きメッシュ (M18): この node の skin から SkinnedModel を一度だけ登録
        AssetID skinModelId = {};
        if (node->skin) {
            const auto skinIdx = static_cast<size_t>(node->skin - data->skins);
            skinModelId = LoadSkin(lc, data, node->skin, skinIdx);
        }
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
            // スキン付きなら SkinnedMeshComponent を付与 (ポーズは描画専用・非ハッシュ)。
            // clip 0 = 最初のアニメ (CesiumMan は歩行)。RenderSystem がフレーム毎に評価する
            if (!skinModelId.IsNull()) {
                auto* sm = target.AddComponent<SkinnedMeshComponent>();
                sm->model = skinModelId;
                sm->clip = 0;
                sm->timeTicks = 0;
                sm->playing = 1;
            }
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
    // AssetID キーは正規化パスから生成 (ホットリロード時の照合と一致させる)
    lc.pathUtf8 = WideToUtf8(NormalizePathKey(path));
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

bool RegisterAssets(RenderResources& resources, ShaderManager& shaders, const std::wstring& path,
                    bool logErrors)
{
    const std::string utf8 = WideToUtf8(path);

    cgltf_options options = {};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, utf8.c_str(), &data) != cgltf_result_success
        || cgltf_load_buffers(&options, data, utf8.c_str()) != cgltf_result_success) {
        if (logErrors) {
            MYE_LOG_ERROR("[reload] glTF reload parse failed: %s", utf8.c_str());
        }
        if (data) {
            cgltf_free(data);
        }
        return false;
    }

    LoadContext lc;
    lc.scene = nullptr; // エンティティは作らない
    lc.resources = &resources;
    lc.pathUtf8 = WideToUtf8(NormalizePathKey(path));
    lc.baseDir = std::filesystem::path(path).parent_path().wstring();
    lc.shaderId = shaders.Load("forward_lit");

    // 全メッシュ / プリミティブを同じキーで再登録 (= GPU バッファ差し替え)
    for (cgltf_size m = 0; m < data->meshes_count; ++m) {
        for (cgltf_size p = 0; p < data->meshes[m].primitives_count; ++p) {
            char key[512];
            snprintf(key, sizeof(key), "%s#mesh%zu#prim%zu", lc.pathUtf8.c_str(),
                     static_cast<size_t>(m), static_cast<size_t>(p));
            const cgltf_primitive* prim = &data->meshes[m].primitives[p];
            LoadPrimitiveMesh(lc, prim, key);

            char matKey[512];
            snprintf(matKey, sizeof(matKey), "%s#mat%zd", lc.pathUtf8.c_str(),
                     prim->material ? (prim->material - data->materials) : -1);
            LoadMaterial(lc, prim->material, matKey);
        }
    }
    // M50a: スキンも同じキーで登録 (RegisterSkinnedModels と同一ループ) — これで
    // Load が作る AssetID 全種 (mesh / material / skin) がヘッドレスで揃う
    for (cgltf_size i = 0; i < data->skins_count; ++i) {
        LoadSkin(lc, data, &data->skins[i], static_cast<size_t>(i));
    }
    cgltf_free(data);
    return true;
}

bool ReloadMeshes(RenderResources& resources, ShaderManager& shaders, const std::wstring& path)
{
    if (!RegisterAssets(resources, shaders, path, /*logErrors=*/true)) {
        return false;
    }
    MYE_LOG_INFO("[reload] glTF meshes reloaded: %s", WideToUtf8(path).c_str());
    return true;
}

size_t RegisterSkinnedModels(RenderResources& resources, const std::wstring& path)
{
    const std::string utf8 = WideToUtf8(path);
    cgltf_options options = {};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, utf8.c_str(), &data) != cgltf_result_success
        || cgltf_load_buffers(&options, data, utf8.c_str()) != cgltf_result_success) {
        if (data) {
            cgltf_free(data);
        }
        return 0; // 起動時の全走査から呼ばれるので、非モデルの .glb でも黙って諦める
    }
    LoadContext lc;
    lc.scene = nullptr; // エンティティは作らない
    lc.resources = &resources;
    lc.pathUtf8 = WideToUtf8(NormalizePathKey(path));
    lc.baseDir = std::filesystem::path(path).parent_path().wstring();
    // shaderId は使わない (LoadSkin はマテリアルに触れない)

    // **skinIdx は data->skins の index** — Load 側も `node->skin - data->skins` で
    // 同じ値を作っているので、全 skin を順に登録すればキーが一致する
    for (cgltf_size i = 0; i < data->skins_count; ++i) {
        LoadSkin(lc, data, &data->skins[i], static_cast<size_t>(i));
    }
    const size_t count = static_cast<size_t>(data->skins_count);
    cgltf_free(data);
    return count;
}

} // namespace mye::ModelLoader
