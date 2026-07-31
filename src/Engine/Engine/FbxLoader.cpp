#include "Engine/Engine/FbxLoader.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <initializer_list>
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
    std::vector<uint32_t> diagnosedMats; // 診断ログを出し終えたマテリアルの element_id
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

// FBX が書き出し元 OS の区切りをそのまま持つことがあるので Windows 側に正規化する
std::string NormalizeSeps(std::string s)
{
    for (char& c : s) {
        if (c == '/') {
            c = '\\';
        }
    }
    return s;
}

std::string BaseNameOf(const std::string& p)
{
    const size_t sep = p.find_last_of("/\\");
    return (sep == std::string::npos) ? p : p.substr(sep + 1);
}

std::string LowerExtOf(const std::string& p)
{
    const size_t dot = p.find_last_of('.');
    if (dot == std::string::npos) {
        return {};
    }
    std::string ext = p.substr(dot);
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext;
}

// stb_image (GpuResources.cpp の stbi_load) が読めない形式。案内を具体的に出すために判定する
bool IsUnsupportedImageExt(const std::string& ext)
{
    return ext == ".tif" || ext == ".tiff" || ext == ".exr";
}

const char* TextureTypeName(ufbx_texture_type t)
{
    switch (t) {
    case UFBX_TEXTURE_FILE:
        return "file";
    case UFBX_TEXTURE_LAYERED:
        return "layered";
    case UFBX_TEXTURE_PROCEDURAL:
        return "procedural";
    case UFBX_TEXTURE_SHADER:
        return "shader";
    default:
        return "unknown";
    }
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

// FBX のテクスチャ参照を AssetID に解決する (P2)。解決順は
// レイヤード/シェーダの展開 → 埋め込みコンテンツ → 外部ファイル → フォールバック探索。
// 解決できなければ空 ID を返し、試したパスを WARN に出す (従来は黙って White に落ちていた)。
AssetID ResolveTexture(LoadContext& lc, const ufbx_texture* tex, bool srgb, const char* slot,
                       const char* matName)
{
    if (!tex) {
        return {};
    }
    // 1. レイヤード / シェーダテクスチャを実ファイルへ展開。file_textures は FILE 型なら
    //    自分自身を含む (ufbx.h:2944) ので、この 1 本で LAYERED/SHADER の両方に対応できる
    const ufbx_texture* file = tex;
    if (file->type != UFBX_TEXTURE_FILE) {
        if (tex->file_textures.count == 0) {
            MYE_LOG_WARN("FBX texture unresolved: material '%s' slot '%s' は %s テクスチャで"
                         "ファイル実体を持たない",
                         matName, slot, TextureTypeName(tex->type));
            return {};
        }
        file = tex->file_textures.data[0];
    }

    // 2. 埋め込みコンテンツ (Embed Media) を優先。FBX では非常に一般的で White 化の最有力原因。
    //    キーは glTF 側 (ModelLoader.cpp) に倣いパス + element_id でモデル内一意にする
    if (file->content.size > 0) {
        char texKey[512];
        snprintf(texKey, sizeof(texKey), "%s#tex:%u", lc.pathUtf8.c_str(), file->element_id);
        const AssetID id =
            lc.resources->textures.CreateFromEncoded(texKey, file->content.data, file->content.size, srgb);
        if (!id.IsNull()) {
            return id;
        }
        // デコード失敗 (stb 非対応の埋め込み形式) は外部ファイル探索へフォールバックする
    }

    // 3-4. 外部ファイル → フォールバック探索。ufbx の filename は既に FBX のディレクトリ基準で
    //      解決済みなので、それが外れたときだけ書き出し元とのフォルダ構成差を救いにいく
    std::vector<std::wstring> tried;
    auto tryPath = [&](const std::wstring& p) -> AssetID {
        if (p.empty()) {
            return {};
        }
        for (const std::wstring& t : tried) {
            if (t == p) {
                return {}; // 同一パスの再試行は無駄なので省く
            }
        }
        tried.push_back(p);
        std::error_code ec;
        if (!std::filesystem::exists(p, ec)) {
            return {};
        }
        return lc.resources->textures.LoadFile(p, srgb);
    };

    const std::string fileName = NormalizeSeps(StrOf(file->filename));
    const std::string relName = NormalizeSeps(StrOf(file->relative_filename));
    const std::string baseName = BaseNameOf(!fileName.empty() ? fileName : relName);

    AssetID id = tryPath(Utf8ToWide(fileName));
    if (id.IsNull() && !fileName.empty() && std::filesystem::path(fileName).is_relative()) {
        id = tryPath(lc.baseDir + L"\\" + Utf8ToWide(fileName));
    }
    if (id.IsNull() && !baseName.empty()) {
        id = tryPath(lc.baseDir + L"\\" + Utf8ToWide(baseName));
    }
    if (id.IsNull() && !baseName.empty()) {
        id = tryPath(lc.baseDir + L"\\textures\\" + Utf8ToWide(baseName));
    }
    if (id.IsNull() && !relName.empty()) {
        id = tryPath(lc.baseDir + L"\\" + Utf8ToWide(relName));
    }
    if (!id.IsNull()) {
        return id;
    }

    // 5-6. 診断。どのマテリアルのどのスロットが、どのパスを試して駄目だったかを残す
    std::string detail;
    for (const std::wstring& t : tried) {
        detail += "\n    " + WideToUtf8(t);
    }
    if (detail.empty()) {
        detail = "\n    (ファイル名が空)";
    }
    if (IsUnsupportedImageExt(LowerExtOf(baseName))) {
        MYE_LOG_WARN("FBX texture unresolved: material '%s' slot '%s' の '%s' は非対応形式です。"
                     "png / tga / dds に変換してください",
                     matName, slot, baseName.c_str());
        return {};
    }
    MYE_LOG_WARN("FBX texture unresolved: material '%s' slot '%s' (埋め込みなし)。試したパス:%s",
                 matName, slot, detail.c_str());
    return {};
}

// テクスチャが接続されていて有効なら返す。has_value はテクスチャの有無と無関係に真に
// なりうる (実測: box.fbx の pbr.normal_map は has_value=1 / texture=null) ので、
// テクスチャスロットの判定に has_value を使ってはいけない。
const ufbx_texture* EnabledTexture(const ufbx_material_map& map)
{
    return (map.texture && map.texture_enabled) ? map.texture : nullptr;
}

// 同一マテリアルは複数のパート / 複数のインスタンスから参照されるため、素直に出すと
// 同じ診断行が何度も並ぶ。マテリアル単位で最初の 1 回だけ出す。
bool ShouldDiagnose(LoadContext& lc, const ufbx_material* mat)
{
    for (const uint32_t id : lc.diagnosedMats) {
        if (id == mat->element_id) {
            return false;
        }
    }
    lc.diagnosedMats.push_back(mat->element_id);
    return true;
}

// マテリアルの不透明度 (0=透明 1=不透明) を求める。
// PBR 系シェーダ (Arnold / glTF / OpenPBR 等) は pbr.opacity を持つのでそれを最優先する。
// FBX 組み込みの Lambert / Phong には opacity マップが存在せず、透明度は
// TransparencyFactor → pbr.transmission_factor / TransparentColor → pbr.transmission_color
// に入る (ufbx.c のシェーダマッピング表)。
//
// ★factor だけで判定してはいけない: Maya 系の書き出しは「factor は倍率、実際の透明度は
// TransparentColor」という規約で、**不透明なマテリアルにも TransparencyFactor=1 を書く**
// (実測: assets\models\cubes_pivot.fbx の Mat_Green は factor=1 / color=(0,0,0) で不透明)。
// factor 単独で判定すると既存アセットが軒並み alpha=0 で消える。色が書かれていれば
// 「最も透過するチャンネル」を倍率として掛け、黒 (=透過なし) を不透明として扱う。
float ComputeOpacity(const ufbx_material* mat)
{
    if (mat->pbr.opacity.has_value) {
        return static_cast<float>(mat->pbr.opacity.value_real);
    }
    if (!mat->pbr.transmission_factor.has_value) {
        return 1.0f;
    }
    float transmission = static_cast<float>(mat->pbr.transmission_factor.value_real);
    if (mat->pbr.transmission_color.has_value) {
        const ufbx_vec3 c = mat->pbr.transmission_color.value_vec3;
        // 赤ガラスのような有色透過を輝度で潰さないよう最大チャンネルを採る
        double maxChannel = c.x > c.y ? c.x : c.y;
        maxChannel = maxChannel > c.z ? maxChannel : c.z;
        transmission *= static_cast<float>(maxChannel);
    }
    return 1.0f - transmission;
}

AssetID LoadMaterial(LoadContext& lc, const ufbx_material* mat, const ufbx_mesh* mesh,
                     const char* key)
{
    Material m;
    m.shader = lc.shaderId;
    m.texture = lc.resources->textures.White();
    if (!mat) {
        return lc.resources->materials.Register(key, m);
    }
    const std::string matName = StrOf(mat->name);
    const bool diag = ShouldDiagnose(lc, mat);

    const ufbx_material_map& bc = mat->pbr.base_color;
    if (bc.has_value) {
        m.baseColor = { static_cast<float>(bc.value_vec4.x), static_cast<float>(bc.value_vec4.y),
                        static_cast<float>(bc.value_vec4.z), static_cast<float>(bc.value_vec4.w) };
    }
    // DiffuseFactor (pbr.base_factor)。無視するとファクタ < 1 のマテリアルが明るすぎる。
    // シェーダはベースカラーをテクスチャに乗算するので rgb に畳み込んでよい
    if (mat->pbr.base_factor.has_value) {
        const float bf = static_cast<float>(mat->pbr.base_factor.value_real);
        m.baseColor.x *= bf;
        m.baseColor.y *= bf;
        m.baseColor.z *= bf;
    }
    if (mat->pbr.metalness.has_value) {
        m.metallic = static_cast<float>(mat->pbr.metalness.value_real);
    } else if (diag) {
        // Lambert / Phong には metalness が無い。specular からの憶測変換はせず既定値のままにする
        MYE_LOG_INFO("FBX material '%s': シェーディングモデル '%s' は metalness を持たないため "
                     "metallic は既定値 (%.2f) です",
                     matName.c_str(), StrOf(mat->shading_model_name).c_str(),
                     static_cast<double>(m.metallic));
    }
    if (mat->pbr.roughness.has_value) {
        m.roughness = static_cast<float>(mat->pbr.roughness.value_real);
    }

    // 半透明。ForwardPath / DeferredPath の半透明キューに乗せる
    const float opacity = ComputeOpacity(mat);
    m.baseColor.w *= opacity;
    if (m.baseColor.w < 0.999f) {
        m.transparent = 1;
        if (diag) {
            MYE_LOG_INFO("FBX material '%s': 半透明 (alpha=%.3f) として読み込みました",
                         matName.c_str(), static_cast<double>(m.baseColor.w));
        }
    }

    // ベースカラー。texture_enabled が false のマップはテクスチャを無視する (ufbx.h:2311)。
    // Phong/Lambert は ufbx が pbr へマップするが、拾えないケースに備えて fbx 側も見る
    const ufbx_texture* tex = EnabledTexture(bc);
    if (!tex) {
        tex = EnabledTexture(mat->fbx.diffuse_color);
    }
    if (tex) {
        // ベースカラーは _SRGB でロード (M38a)
        const AssetID texId = ResolveTexture(lc, tex, /*srgb=*/true, "base_color", matName.c_str());
        if (!texId.IsNull()) {
            m.texture = texId;
        }
    }

    // ノーマルマップ。頂点タンジェントは不要 — common.hlsli の PerturbNormal が
    // posW/uv の導関数から接空間基底を作る (M17.3) ので MeshVertex は変更しなくてよい。
    // bump スロットは最後の手段: 書き出し元によっては接空間ノーマルマップがここに入るが、
    // 本来のハイトマップが入っていることもあり両者を FBX からは区別できない
    const ufbx_texture* nrm = EnabledTexture(mat->pbr.normal_map);
    const char* nrmSlot = "normal_map";
    if (!nrm) {
        nrm = EnabledTexture(mat->fbx.normal_map);
    }
    if (!nrm) {
        nrm = EnabledTexture(mat->fbx.bump);
        if (nrm) {
            nrmSlot = "bump";
            if (diag) {
                MYE_LOG_WARN("FBX material '%s': bump スロットを接空間ノーマルマップとして"
                             "解釈します。ハイトマップの場合は陰影が破綻します",
                             matName.c_str());
            }
        }
    }
    if (nrm) {
        // ノーマルマップは色ではないのでリニア (srgb=false) で読む (M38a)
        const AssetID nrmId = ResolveTexture(lc, nrm, /*srgb=*/false, nrmSlot, matName.c_str());
        if (!nrmId.IsNull()) {
            m.normalTex = nrmId;
        }
    }

    if (diag) {
        // emissive: Material にフィールドが無いので取り込めない (今回は対象外)
        const ufbx_material_map& em = mat->pbr.emission_color;
        const double emFactor =
            mat->pbr.emission_factor.has_value ? mat->pbr.emission_factor.value_real : 1.0;
        if (emFactor * (em.value_vec3.x + em.value_vec3.y + em.value_vec3.z) > 0.0) {
            MYE_LOG_WARN("FBX material '%s': emissive が設定されていますが、エンジンの Material に "
                         "emissive が無いため無視します",
                         matName.c_str());
        }
        // 第 2 UV セットはメッシュ分割が必要なので据え置き (WARN のみ)
        if (mesh && mesh->uv_sets.count > 1) {
            const ufbx_string set0 = mesh->uv_sets.data[0].name;
            for (const ufbx_texture* t : { tex, nrm }) {
                if (!t || t->uv_set.length == 0) {
                    continue;
                }
                if (t->uv_set.length != set0.length ||
                    memcmp(t->uv_set.data, set0.data, set0.length) != 0) {
                    MYE_LOG_WARN("FBX material '%s': テクスチャが UV セット '%s' を参照して"
                                 "いますが、第 1 セット '%s' のみ対応のため UV がずれます",
                                 matName.c_str(), StrOf(t->uv_set).c_str(), StrOf(set0).c_str());
                }
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
        const AssetID matAsset = LoadMaterial(lc, mat, mesh, matKey);

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
