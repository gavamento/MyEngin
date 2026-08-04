#include "Engine/Engine/SkeletonSelfTest.h"

#include <cmath>
#include <cstring>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Components.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/FbxLoader.h"
#include "Engine/Engine/ModelLoader.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/TransformSystem.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/ShaderManager.h"
#include "Engine/Renderer/Skeleton.h"

using namespace DirectX;

namespace mye {
namespace {

// 全ジョイント × clip {-1,0} × tick {0,30,60} のグローバル行列バイト列の FNV-1a (両モデル連結)。
// Debug/Release 両構成でこの定数に一致すること = 骨ポーズ演算の構成間決定論の先行証明 (M48a)。
// 値は Debug 実行の実測から埋める (アセット CesiumMan.glb / skinned_beam.fbx に依存 —
// アセットを差し替えたら本定数も再採取すること)
constexpr uint64_t kExpectedPoseChecksum = 0x191B01FF512270D0ull;

constexpr int kTicks[] = { 0, 30, 60 };
constexpr int kClips[] = { -1, 0 }; // -1 = バインドポーズ / 0 = 先頭クリップ

// 検証対象 1 体分: SkinnedModel + それを描くエンティティの WorldMatrix (= 部位式の gWorld 項)
struct LoadedSkin {
    const SkinnedModel* model = nullptr;
    XMFLOAT4X4 entityWorld = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
};

LoadedSkin FindSkinned(Scene& scene, RenderResources& resources)
{
    LoadedSkin out;
    World& world = scene.GetWorld();
    const ComponentTypeId req[] = { SkinnedMeshComponent::sTypeId,
                                    WorldMatrixComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            if (out.model) {
                return; // 最初の 1 体で十分 (検証アセットはスキン 1 個)
            }
            const EntityID e = arch.EntityAt(row);
            auto* sm = world.GetComponent<SkinnedMeshComponent>(e);
            auto* wm = world.GetComponent<WorldMatrixComponent>(e);
            const SkinnedModel* model = sm ? resources.skinnedModels.Get(sm->model) : nullptr;
            if (model && wm) {
                out.model = model;
                out.entityWorld = wm->value;
            }
        }
    });
    return out;
}

float MaxAbsDiff(const XMFLOAT4X4& a, const XMFLOAT4X4& b)
{
    float maxDiff = 0.0f;
    const float* pa = &a._11;
    const float* pb = &b._11;
    for (int i = 0; i < 16; ++i) {
        maxDiff = std::max(maxDiff, std::fabs(pa[i] - pb[i]));
    }
    return maxDiff;
}

XMFLOAT4X4 ToF4x4(FXMMATRIX m)
{
    XMFLOAT4X4 out;
    XMStoreFloat4x4(&out, m);
    return out;
}

const XMFLOAT4X4 kIdentity4x4 = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

// 部位 (ソケット) のワールド位置。M48a の結論である
//   socketWorld = jointGlobal * entityWorld   (行ベクトル規約)
// をそのまま実装したもの。withEntityWorld=false は「entityWorld を落とした誤った式」で、
// 検査に識別力があること (正しい式でしか通らないこと) を示すために使う
XMFLOAT3 SocketPos(const LoadedSkin& skin, int32_t joint, bool withEntityWorld)
{
    XMMATRIX m = ComputeJointGlobal(*skin.model, -1, 0.0f, joint);
    if (withEntityWorld) {
        m = XMMatrixMultiply(m, XMLoadFloat4x4(&skin.entityWorld));
    }
    const XMFLOAT4X4 f = ToF4x4(m);
    return { f._41, f._42, f._43 };
}

} // namespace

bool RunSkeletonSelfTest()
{
    MYE_LOG_INFO("==== Skeleton self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- ヘッドレスロード (Init しない = GPU バッファ / テクスチャ / シェーダ生成をスキップ) ----
    const std::wstring assetsRoot = FindAssetsRoot();
    RenderResources resources;
    ShaderManager shaders;
    TransformSystem transforms;

    Scene gltfScene;
    const GameObject gltfRoot = ModelLoader::Load(gltfScene, resources, shaders,
                                                  assetsRoot + L"\\models\\CesiumMan.glb");
    check(bool(gltfRoot), "glTF: CesiumMan.glb loads headless");
    gltfScene.GetWorld().ApplyStructuralChanges();
    transforms.Update(gltfScene.GetWorld());
    const LoadedSkin gltf = FindSkinned(gltfScene, resources);
    check(gltf.model != nullptr, "glTF: a skinned entity + SkinnedModel are registered");

    Scene fbxScene;
    const GameObject fbxRoot = FbxLoader::Load(fbxScene, resources, shaders,
                                               assetsRoot + L"\\models\\skinned_beam.fbx");
    check(bool(fbxRoot), "FBX: skinned_beam.fbx loads headless");
    fbxScene.GetWorld().ApplyStructuralChanges();
    transforms.Update(fbxScene.GetWorld());
    const LoadedSkin fbx = FindSkinned(fbxScene, resources);
    check(fbx.model != nullptr, "FBX: a skinned entity + SkinnedModel are registered");

    if (!gltf.model || !fbx.model) {
        MYE_LOG_ERROR("==== Skeleton self test: aborted (assets failed to load) ====");
        return false;
    }

    check(!gltf.model->clips.empty() && !gltf.model->joints.empty(),
          "glTF: model has joints and clips");
    check(!fbx.model->clips.empty() && !fbx.model->joints.empty(),
          "FBX: model has joints and clips");

    // ---- (1) ジョイント名の保持と FindJointByName ----
    {
        size_t named = 0;
        bool roundtrip = true;
        for (size_t j = 0; j < gltf.model->joints.size(); ++j) {
            const std::string& name = gltf.model->joints[j].name;
            if (name.empty()) {
                continue;
            }
            ++named;
            // 先頭一致規約: 返る index の名前が一致していればよい (重複名は最初の 1 件)
            const int32_t found = gltf.model->FindJointByName(name);
            roundtrip &= (found >= 0 && gltf.model->joints[static_cast<size_t>(found)].name == name);
        }
        check(named == gltf.model->joints.size(), "glTF: every joint keeps its node name");
        check(roundtrip, "glTF: FindJointByName round-trips every joint name");

        check(fbx.model->FindJointByName("Bone1") >= 0, "FBX: FindJointByName(Bone1)");
        check(fbx.model->FindJointByName("Bone2") >= 0, "FBX: FindJointByName(Bone2)");
        check(fbx.model->FindJointByName("Armature") >= 0,
              "FBX: ancestor closure joints keep their names (Armature)");
        check(gltf.model->FindJointByName("no_such_joint") == -1
                  && fbx.model->FindJointByName("") == -1,
              "FindJointByName rejects unknown and empty names");
    }

    // ---- (2) 再評価のビット一致 (隠れ状態 / 初期化漏れの検出) ----
    {
        bool stable = true;
        for (const LoadedSkin* skin : { &gltf, &fbx }) {
            for (int clip : kClips) {
                for (int tick : kTicks) {
                    const float timeSec = static_cast<float>(tick) / 60.0f;
                    for (size_t j = 0; j < skin->model->joints.size(); ++j) {
                        const XMFLOAT4X4 a = ToF4x4(ComputeJointGlobal(
                            *skin->model, clip, timeSec, static_cast<int32_t>(j)));
                        const XMFLOAT4X4 b = ToF4x4(ComputeJointGlobal(
                            *skin->model, clip, timeSec, static_cast<int32_t>(j)));
                        stable &= (std::memcmp(&a, &b, sizeof(a)) == 0);
                    }
                }
            }
        }
        check(stable, "ComputeJointGlobal is bit-identical on repeated evaluation");
    }

    // ---- (3) パレットとの合成一致 (リファクタが 2 経路に割れていないことの観測) ----
    {
        bool consistent = true;
        for (const LoadedSkin* skin : { &gltf, &fbx }) {
            std::vector<XMFLOAT4X4> palette;
            ComputeBonePalette(*skin->model, 0, 0.5f, palette);
            for (size_t j = 0; j < skin->model->joints.size(); ++j) {
                const XMMATRIX ib = XMLoadFloat4x4(&skin->model->joints[j].inverseBind);
                const XMMATRIX global =
                    ComputeJointGlobal(*skin->model, 0, 0.5f, static_cast<int32_t>(j));
                const XMFLOAT4X4 composed =
                    ToF4x4(XMMatrixTranspose(XMMatrixMultiply(ib, global)));
                consistent &= (std::memcmp(&composed, &palette[j], sizeof(composed)) == 0);
            }
        }
        check(consistent, "ComputeBonePalette == transpose(IB * ComputeJointGlobal) bit-exact");
    }

    // ---- (4) 構成間チェックサム (Debug/Release で同一定数 = 決定論の先行証明) ----
    {
        uint64_t h = kFnvOffset;
        for (const LoadedSkin* skin : { &gltf, &fbx }) {
            for (int clip : kClips) {
                for (int tick : kTicks) {
                    const float timeSec = static_cast<float>(tick) / 60.0f;
                    for (size_t j = 0; j < skin->model->joints.size(); ++j) {
                        const XMFLOAT4X4 m = ToF4x4(ComputeJointGlobal(
                            *skin->model, clip, timeSec, static_cast<int32_t>(j)));
                        h = HashBytes(&m, sizeof(m), h);
                    }
                }
            }
        }
        MYE_LOG_INFO("  pose checksum = 0x%016llX (expected 0x%016llX)",
                     static_cast<unsigned long long>(h),
                     static_cast<unsigned long long>(kExpectedPoseChecksum));
        check(h == kExpectedPoseChecksum,
              "pose checksum matches the embedded constant (cross-config determinism)");
    }

    // ---- (5) 部位ワールド規約 (M48a の本題。両ローダで同型が成立することの証明) ----
    // 結論 (実測): **両ローダとも `IB_j * jointGlobal_j(bind) == 恒等`**。
    // これは「jointGlobal はメッシュノードの座標系から見たボーンの変換」という意味であり、
    // ここから部位 (ソケット) のワールドは
    //     socketWorld = jointGlobal_j * entityWorld     (行ベクトル規約)
    // となる。スキニングの頂点式 `v * IB * jointGlobal * entityWorld` と同じ座標系に乗るので、
    // 部位に付けた子は必ずボーンが動かす皮膚と一致する。
    //
    // ★当初は `IB * jointGlobal * entityWorld == 恒等` を規約と仮定していたが**これは誤り**。
    //   glTF の inverse-bind は「メッシュノード基準」で書かれており、シーンルート基準ではない
    //   (glTF 仕様の jointMatrix = inverse(meshNodeGlobal) * jointGlobal * IB に対応)。
    //   実測でも entityWorld を掛けた版は max|dev| = 1.000001 = ちょうど entityWorld ぶん外れ、
    //   ズレは全ジョイント共通の固定変換だった (spread = 0.000001) ため、余分な因子と判明した。
    //   FBX は entityWorld が恒等 (P4-5) なので両式が偶然一致し、glTF が差を暴いた。
    {
        float gltfDev = 0.0f;
        for (size_t j = 0; j < gltf.model->joints.size(); ++j) {
            const XMMATRIX ib = XMLoadFloat4x4(&gltf.model->joints[j].inverseBind);
            const XMMATRIX global =
                ComputeJointGlobal(*gltf.model, -1, 0.0f, static_cast<int32_t>(j));
            gltfDev = std::max(gltfDev, MaxAbsDiff(ToF4x4(XMMatrixMultiply(ib, global)), kIdentity4x4));
        }
        // 閾値は実測 (glTF 約 5e-7 = ファイル内 IBM が float32 である以上ほぼ精度の下限、
        // FBX は厳密 0) に対して余裕を 2 桁だけ取った値。緩くすると「規約が壊れた」ではなく
        // 「精度が劣化した」系の退行 (例: バインド行列を低精度で焼き直す) を素通ししてしまう
        MYE_LOG_INFO("  glTF bind-pose regime max deviation = %.8f", gltfDev);
        check(gltfDev < 1e-4f, "glTF: IB * jointGlobal == identity at bind pose");

        // glTF はジョイント外祖先 (Z_UP / Armature) を **entityWorld が担う** 規約なので、
        // entityWorld は恒等ではない = 部位式から落とすと必ず壊れる (下の (7) で実証する)
        check(MaxAbsDiff(gltf.entityWorld, kIdentity4x4) > 0.5f,
              "glTF: entityWorld carries the non-joint ancestors (not identity)");

        // FBX は祖先閉包をジョイント側に入れる規約なのでメッシュ側が恒等になる (P4-5)
        check(MaxAbsDiff(fbx.entityWorld, kIdentity4x4) < 1e-5f,
              "FBX: skinned mesh entity world is identity (P4-5 placement)");
        float fbxDev = 0.0f;
        for (const char* bone : { "Bone1", "Bone2" }) { // 祖先閉包側は IB を持たないので対象外
            const int32_t j = fbx.model->FindJointByName(bone);
            if (j < 0) {
                fbxDev = 1e9f; // 上の (1) で検出済みだがここでも確実に落とす
                continue;
            }
            const XMMATRIX ib =
                XMLoadFloat4x4(&fbx.model->joints[static_cast<size_t>(j)].inverseBind);
            const XMMATRIX global = ComputeJointGlobal(*fbx.model, -1, 0.0f, j);
            fbxDev = std::max(fbxDev, MaxAbsDiff(ToF4x4(XMMatrixMultiply(ib, global)), kIdentity4x4));
        }
        MYE_LOG_INFO("  FBX bind-pose regime max deviation = %.8f", fbxDev);
        check(fbxDev < 1e-5f, "FBX: IB * jointGlobal == identity at bind pose");
    }

    // ---- (6) 既知ポーズ (skinned_beam の設計値。gen_skinned_beam_fbx.ps1 参照) ----
    // バインド: ボーンのワールド位置 = Bone1 (0,0,0) / Bone2 (0,2,0) (Armature T(3,0,0) を
    // Bone1 T(-3,0,0) が相殺)。アニメ: Bone2 の回転 Z が 1 秒で 0 -> 90 度
    {
        const int32_t j1 = fbx.model->FindJointByName("Bone1");
        const int32_t j2 = fbx.model->FindJointByName("Bone2");
        bool bindPos = false;
        if (j1 >= 0 && j2 >= 0) {
            const XMFLOAT4X4 g1 = ToF4x4(ComputeJointGlobal(*fbx.model, -1, 0.0f, j1));
            const XMFLOAT4X4 g2 = ToF4x4(ComputeJointGlobal(*fbx.model, -1, 0.0f, j2));
            bindPos = std::fabs(g1._41) < 1e-4f && std::fabs(g1._42) < 1e-4f
                      && std::fabs(g1._43) < 1e-4f && std::fabs(g2._41) < 1e-4f
                      && std::fabs(g2._42 - 2.0f) < 1e-4f && std::fabs(g2._43) < 1e-4f;
        }
        check(bindPos, "FBX: bind joint world positions are (0,0,0) and (0,2,0)");

        // 回転の進行は「Bone2 の局所 +Y 軸のワールド Y 成分」= cos(角度) で符号規約に依存しない
        bool anim = true;
        const float expected[] = { 1.0f, 0.70711f, 0.0f }; // cos(0/45/90 度)
        for (int i = 0; i < 3; ++i) {
            const XMMATRIX global =
                ComputeJointGlobal(*fbx.model, 0, static_cast<float>(kTicks[i]) / 60.0f, j2);
            const XMVECTOR yAxis =
                XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(0, 1, 0, 0), global));
            anim &= std::fabs(XMVectorGetY(yAxis) - expected[i]) < 2e-3f;
            // 回転は自分の原点まわり: ジョイント位置は動かない
            const XMFLOAT4X4 g = ToF4x4(global);
            anim &= std::fabs(g._41) < 1e-4f && std::fabs(g._42 - 2.0f) < 1e-4f
                    && std::fabs(g._43) < 1e-4f;
        }
        check(anim, "FBX: Bone2 rotates 0/45/90 deg at t=0/0.5/1.0 about its own origin");
    }

    // ---- (7) 部位式が実際に人型を正しく立たせるか (glTF、entityWorld の必要性の実証) ----
    // CesiumMan は glTF が Z-up でエンジンが Y-up。その変換 (Z_UP ノード) は entityWorld 側に
    // 入っているので、**部位式に entityWorld を含めて初めて** 首 > 胴 > 足 の高さ関係が出る。
    // 落とした式では縦軸が +Y ではなくなるため高低差が消える = この検査は識別力を持つ
    {
        const int32_t neck = gltf.model->FindJointByName("Skeleton_neck_joint_2");
        const int32_t torso = gltf.model->FindJointByName("Skeleton_torso_joint_1");
        const int32_t foot = gltf.model->FindJointByName("leg_joint_R_5");
        check(neck >= 0 && torso >= 0 && foot >= 0,
              "glTF: the named joints used by the upright test exist");
        if (neck >= 0 && torso >= 0 && foot >= 0) {
            const XMFLOAT3 n = SocketPos(gltf, neck, true);
            const XMFLOAT3 t = SocketPos(gltf, torso, true);
            const XMFLOAT3 f = SocketPos(gltf, foot, true);
            MYE_LOG_INFO("  socket world Y (correct formula): foot=%.3f torso=%.3f neck=%.3f", f.y,
                         t.y, n.y);
            check(f.y < t.y && t.y < n.y && (n.y - f.y) > 0.5f,
                  "glTF: socket world stands the figure up (foot < torso < neck along +Y)");

            const XMFLOAT3 n2 = SocketPos(gltf, neck, false);
            const XMFLOAT3 t2 = SocketPos(gltf, torso, false);
            const XMFLOAT3 f2 = SocketPos(gltf, foot, false);
            MYE_LOG_INFO("  socket world Y (entityWorld dropped): foot=%.3f torso=%.3f neck=%.3f",
                         f2.y, t2.y, n2.y);
            check(std::fabs(n2.y - f2.y) < 0.5f,
                  "glTF: dropping entityWorld collapses the height spread (test discriminates)");
        }
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Skeleton self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Skeleton self test: %d FAILED ====", failCount);
    return false;
}

} // namespace mye
