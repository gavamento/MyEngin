#include "Engine/Engine/Animation/SkeletonSelfTest.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/Asset/FbxLoader.h"
#include "Engine/Engine/Asset/ModelLoader.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Animation/SkinningSystem.h"
#include "Engine/Engine/Scene/TransformSystem.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/Shader/ShaderManager.h"
#include "Engine/Renderer/Mesh/Skeleton.h"

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

// シーン内で最初に見つかった SkinnedMesh (検証アセットはスキン 1 個)。
// 以降に構造変更を起こさないので、ポインタのまま持ってよい
SkinnedMeshComponent* FirstSkinnedComponent(Scene& scene)
{
    SkinnedMeshComponent* out = nullptr;
    const ComponentTypeId req[] = { SkinnedMeshComponent::sTypeId };
    scene.GetWorld().ForEachArchetype(req, [&](Archetype& arch) {
        if (out == nullptr && arch.Count() > 0) {
            const int si = arch.FindTypeIndex(SkinnedMeshComponent::sTypeId);
            out = static_cast<SkinnedMeshComponent*>(arch.GetPtr(si, 0));
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
    // ★`IB * jointGlobal * entityWorld == 恒等` は規約**ではない**。
    //   glTF の inverse-bind は「メッシュノード基準」で書かれており、シーンルート基準ではない
    //   (glTF 仕様の jointMatrix = inverse(meshNodeGlobal) * jointGlobal * IB に対応)。
    //   entityWorld を掛けた版は max|dev| = 1.000001 = ちょうど entityWorld ぶん外れ、
    //   ズレは全ジョイント共通の固定変換 (spread = 0.000001) = 余分な因子。
    //   FBX は entityWorld が恒等 (P4-5) なので両式が偶然一致し、glTF でしか差が出ない。
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

    // ---- (8) クロスフェードの評価 (M18 追補) ----
    // skinned_beam の Bone2 はクリップ 0 で 1 秒かけて Z 回転 0 -> 90 度 ((6) の設計値)。
    // バインド (clip -1 = 0 度) と clip 0 の末尾 (90 度) を半分ずつ混ぜれば 45 度になる。
    // 行列を線形に混ぜる誤った実装だと、角度は合っても +Y 軸の長さが縮む (cos45 に届かない)
    {
        SkinnedMeshComponent sm;
        sm.clip = 0;
        sm.timeTicks = 30;
        std::vector<XMMATRIX> sampled;
        std::vector<XMMATRIX> direct;
        SampleSkinnedLocals(*fbx.model, sm, sampled);
        ComputeJointLocals(*fbx.model, 0, 30.0f / 60.0f, direct);
        bool same = sampled.size() == direct.size();
        for (size_t j = 0; same && j < direct.size(); ++j) {
            same = std::memcmp(&sampled[j], &direct[j], sizeof(XMMATRIX)) == 0;
        }
        check(!IsSkinFading(sm) && same,
              "fade: without a fade SampleSkinnedLocals is bit-identical to ComputeJointLocals");

        sm.timeTicks = 60;
        sm.fromClip = -1;
        sm.fromTimeTicks = 0;
        sm.fadeTotal = 2;
        sm.fadeElapsed = 1;
        check(IsSkinFading(sm), "fade: fadeElapsed < fadeTotal is a fade");
        SampleSkinnedLocals(*fbx.model, sm, sampled);
        const int32_t j2 = fbx.model->FindJointByName("Bone2");
        bool half = false;
        if (j2 >= 0) {
            const XMMATRIX g = JointGlobalFromLocals(*fbx.model, sampled, j2);
            const XMVECTOR yAxis = XMVector3TransformNormal(XMVectorSet(0, 1, 0, 0), g);
            MYE_LOG_INFO("  fade 50%% Bone2 +Y = (y %.5f, length %.5f) expected (0.70711, 1)",
                         XMVectorGetY(yAxis), XMVectorGetX(XMVector3Length(yAxis)));
            half = std::fabs(XMVectorGetY(yAxis) - 0.70711f) < 2e-3f
                   && std::fabs(XMVectorGetX(XMVector3Length(yAxis)) - 1.0f) < 2e-3f;
        }
        check(half, "fade: halfway between bind (0 deg) and clip 0 end (90 deg) is 45 deg "
                    "without shrinking (rotation slerps, not a matrix lerp)");

        // 端点: 重み 0 = 元、1 = 先。slerp の端点は丸めで 1e-7 程度ずれうるので許容差で見る
        std::vector<XMMATRIX> w0, w1, a, b;
        ComputeJointLocalsBlended(*fbx.model, -1, 0.0f, 0, 1.0f, 0.0f, w0);
        ComputeJointLocalsBlended(*fbx.model, -1, 0.0f, 0, 1.0f, 1.0f, w1);
        ComputeJointLocals(*fbx.model, -1, 0.0f, a);
        ComputeJointLocals(*fbx.model, 0, 1.0f, b);
        float dev = 0.0f;
        for (size_t j = 0; j < a.size(); ++j) {
            dev = std::max(dev, MaxAbsDiff(ToF4x4(w0[j]), ToF4x4(a[j])));
            dev = std::max(dev, MaxAbsDiff(ToF4x4(w1[j]), ToF4x4(b[j])));
        }
        MYE_LOG_INFO("  fade endpoint max deviation = %.8f", dev);
        check(dev < 1e-5f, "fade: weight 0 reproduces the source pose and weight 1 the target");
    }

    // ---- (9) SkinningSystem の再生規則 (ループ / 一度きり / 切り替え / フェードの進行) ----
    {
        SkinnedMeshComponent* sm = FirstSkinnedComponent(fbxScene);
        check(sm != nullptr, "skinning: the FBX skinned entity has a SkinnedMesh");
        if (sm != nullptr && !fbx.model->clips.empty()) {
            World& w = fbxScene.GetWorld();
            SkinningSystem skinning;
            const int durTicks = static_cast<int>(fbx.model->clips[0].duration * 60.0f + 0.5f);

            // 初めて見る clip は切り替えではない (シーンに保存された timeTicks を潰さない)
            sm->clip = 0;
            sm->timeTicks = 5;
            sm->playing = true;
            sm->loop = 1;
            sm->fadeTicks = 8;
            skinning.Update(w, resources);
            check(sm->observedClip == 0 && sm->timeTicks == 6 && !IsSkinFading(*sm),
                  "skinning: the first observed clip keeps its time and starts no fade");

            // loop = 1 (M18 の既定) はクリップ末尾で 0 へ戻る
            sm->timeTicks = durTicks - 2;
            skinning.Update(w, resources);
            skinning.Update(w, resources);
            check(sm->timeTicks == 0, "skinning: loop = 1 wraps at the clip end (M18 behaviour)");

            // loop = 0 は最後のコマ (= durTicks) に止まり続ける
            sm->loop = 0;
            sm->timeTicks = durTicks - 2;
            for (int i = 0; i < 5; ++i) {
                skinning.Update(w, resources);
            }
            check(sm->timeTicks == durTicks, "skinning: loop = 0 holds the last frame");

            // 切り替え: 新しいクリップは頭から、元は切り替えた瞬間のクリップと時刻で凍る
            sm->loop = 1;
            sm->clip = -1;
            skinning.Update(w, resources);
            check(sm->fromClip == 0 && sm->fromTimeTicks == durTicks && sm->fadeTotal == 8
                      && sm->fadeElapsed == 1 && sm->timeTicks == 0 && IsSkinFading(*sm),
                  "skinning: a clip change restarts the new clip and freezes the old pose to fade from");
            for (int i = 0; i < 7; ++i) {
                skinning.Update(w, resources);
            }
            check(sm->fadeElapsed == 8 && !IsSkinFading(*sm),
                  "skinning: the fade ends after fadeTicks ticks");

            // fadeTicks = 0 は即時切り替え (頭から再生し直すことだけは同じ)
            sm->fadeTicks = 0;
            sm->clip = 0;
            skinning.Update(w, resources);
            check(sm->timeTicks == 1 && sm->fadeTotal == 0 && !IsSkinFading(*sm),
                  "skinning: fadeTicks = 0 switches instantly from the head of the clip");

            // 止めたまま切り替えても、再開の tick に古い時刻を持ち越さない
            sm->playing = false;
            sm->timeTicks = 20;
            sm->clip = -1;
            skinning.Update(w, resources);
            check(sm->observedClip == -1 && sm->timeTicks == 0,
                  "skinning: a clip change while paused still rewinds to the head");
        }
    }

    // ---- (10) ポーズプログラムの多層サンプラ (M89a) ----
    auto bitEqual = [](const std::vector<XMMATRIX>& a, const std::vector<XMMATRIX>& b) {
        bool same = a.size() == b.size();
        for (size_t j = 0; same && j < a.size(); ++j) {
            same = std::memcmp(&a[j], &b[j], sizeof(XMMATRIX)) == 0;
        }
        return same;
    };
    auto maxDev = [](const std::vector<XMMATRIX>& a, const std::vector<XMMATRIX>& b) {
        if (a.size() != b.size()) {
            return 1e9f;
        }
        float dev = 0.0f;
        for (size_t j = 0; j < a.size(); ++j) {
            dev = std::max(dev, MaxAbsDiff(ToF4x4(a[j]), ToF4x4(b[j])));
        }
        return dev;
    };
    {
        // 重みが正の層が 1 枚 = ComputeJointLocals そのもの (単一クリップのステートが旧経路と同じ絵)。
        // 重み 0 の層が前後に居ても落とされること、満杯でない重みでも同じことを一緒に見る
        bool single = true;
        bool program = true;
        for (const LoadedSkin* skin : { &gltf, &fbx }) {
            for (int clip : kClips) {
                for (int tick : kTicks) {
                    const float timeSec = static_cast<float>(tick) / 60.0f;
                    std::vector<XMMATRIX> direct, layered;
                    ComputeJointLocals(*skin->model, clip, timeSec, direct);
                    const SkeletalLayer layers[] = { { 0, 0.3f, 0 },
                                                     { clip, timeSec, 65536 },
                                                     { 0, 0.7f, 0 } };
                    ComputeJointLocalsLayered(*skin->model, layers, 3, layered);
                    single &= bitEqual(direct, layered);

                    // timeQ (1/256 tick) からの秒換算が timeTicks / 60.0f とビット一致すること
                    SkinnedMeshComponent sm;
                    sm.poseLayerCount = 1;
                    sm.poseLayers[0] = { clip, tick * SkinnedMeshComponent::kPoseTimeQPerTick,
                                         SkinnedMeshComponent::kPoseWeightOne };
                    std::vector<XMMATRIX> sampled;
                    SampleSkinnedLocals(*skin->model, sm, sampled);
                    program &= bitEqual(direct, sampled);
                }
            }
        }
        check(single, "layered: one live layer is bit-identical to ComputeJointLocals "
                      "(zero-weight layers are dropped)");
        check(program, "layered: a 1-layer pose program samples bit-identically to the old "
                       "timeTicks / 60 path");

        // ---- (M89f) 描画補間: alpha = 1 は補間なしとビット一致 (決定的撮影の golden を動かさない)、
        // alpha = 0 は前 tick の時刻、途中は prevTimeQ + stepQ * alpha の時刻で引いたのと同じ
        {
            constexpr int32_t kQ = SkinnedMeshComponent::kPoseTimeQPerTick;
            const auto programAt = [&](int32_t timeQ) {
                SkinnedMeshComponent at;
                at.poseLayerCount = 2;
                at.poseLayers[0] = { 0, timeQ, 40000 };
                at.poseLayers[1] = { -1, 0, 25536 };
                std::vector<XMMATRIX> out;
                SampleSkinnedLocals(*gltf.model, at, out);
                return out;
            };
            SkinnedMeshComponent sm;
            sm.poseLayerCount = 2;
            sm.poseLayers[0] = { 0, 11 * kQ, 40000, 10 * kQ, kQ };
            sm.poseLayers[1] = { -1, 0, 25536 };
            std::vector<XMMATRIX> exact, interp;
            SampleSkinnedLocals(*gltf.model, sm, exact);
            SampleSkinnedLocalsInterpolated(*gltf.model, sm, 1.0f, interp);
            bool ok = bitEqual(exact, interp) && bitEqual(exact, programAt(11 * kQ));
            SampleSkinnedLocalsInterpolated(*gltf.model, sm, std::numeric_limits<float>::quiet_NaN(), interp);
            ok = ok && bitEqual(exact, interp);
            check(ok, "interp: alpha = 1 (and NaN) samples exactly timeQ, bit-identical to SampleSkinnedLocals");

            SampleSkinnedLocalsInterpolated(*gltf.model, sm, 0.0f, interp);
            ok = bitEqual(interp, programAt(10 * kQ));
            SampleSkinnedLocalsInterpolated(*gltf.model, sm, 0.5f, interp);
            ok = ok && bitEqual(interp, programAt(10 * kQ + kQ / 2));
            SampleSkinnedLocalsInterpolated(*gltf.model, sm, -3.0f, interp);
            ok = ok && bitEqual(interp, programAt(10 * kQ));
            check(ok, "interp: alpha 0 / 0.5 sample the previous tick / the midpoint; negative alpha clamps to 0");

            sm.poseLayers[0].stepQ = 0;
            SampleSkinnedLocalsInterpolated(*gltf.model, sm, 0.5f, interp);
            check(bitEqual(exact, interp), "interp: a layer with stepQ = 0 is not interpolated (frozen / new state)");

            // 旧経路は補間しない
            SkinnedMeshComponent legacy;
            legacy.clip = 0;
            legacy.timeTicks = 7;
            std::vector<XMMATRIX> legacyExact;
            SampleSkinnedLocals(*gltf.model, legacy, legacyExact);
            SampleSkinnedLocalsInterpolated(*gltf.model, legacy, 0.25f, interp);
            check(bitEqual(legacyExact, interp), "interp: the legacy path (poseLayerCount = 0) ignores alpha");
        }

        // 重みが正の層が無ければバインドポーズ
        std::vector<XMMATRIX> bind, none;
        ComputeJointLocals(*fbx.model, -1, 0.0f, bind);
        const SkeletalLayer zero[] = { { 0, 0.5f, 0 } };
        ComputeJointLocalsLayered(*fbx.model, zero, 1, none);
        check(bitEqual(bind, none), "layered: no live layer is the bind pose");
    }
    {
        // 2 層 50/50: バインド (0 度) と clip 0 の末尾 (90 度) の間 = 45 度、縮まない ((8) と同じ設計値)
        const SkeletalLayer half[] = { { -1, 0.0f, 32768 }, { 0, 1.0f, 32768 } };
        std::vector<XMMATRIX> layered, blended;
        ComputeJointLocalsLayered(*fbx.model, half, 2, layered);
        const int32_t j2 = fbx.model->FindJointByName("Bone2");
        bool mid = false;
        if (j2 >= 0) {
            const XMMATRIX g = JointGlobalFromLocals(*fbx.model, layered, j2);
            const XMVECTOR yAxis = XMVector3TransformNormal(XMVectorSet(0, 1, 0, 0), g);
            mid = std::fabs(XMVectorGetY(yAxis) - 0.70711f) < 2e-3f
                  && std::fabs(XMVectorGetX(XMVector3Length(yAxis)) - 1.0f) < 2e-3f;
        }
        check(mid, "layered: two layers at 50/50 slerp to 45 deg without shrinking");

        // 2 層は ComputeJointLocalsBlended と同じ混ぜ方 (比 w1 / (w0 + w1) = 0.25)
        const SkeletalLayer quarter[] = { { -1, 0.0f, 49152 }, { 0, 1.0f, 16384 } };
        ComputeJointLocalsLayered(*fbx.model, quarter, 2, layered);
        ComputeJointLocalsBlended(*fbx.model, -1, 0.0f, 0, 1.0f, 0.25f, blended);
        const float dev2 = maxDev(layered, blended);
        MYE_LOG_INFO("  layered 2-layer vs Blended max deviation = %.8f", dev2);
        check(dev2 < 1e-6f, "layered: two layers fold the same way as ComputeJointLocalsBlended");

        // 同じポーズを 3 枚に割っても同じポーズ (比の作り方の検算。重みの合計 65536)
        std::vector<XMMATRIX> direct;
        ComputeJointLocals(*gltf.model, 0, 0.5f, direct);
        const SkeletalLayer split[] = { { 0, 0.5f, 20000 }, { 0, 0.5f, 20000 }, { 0, 0.5f, 25536 } };
        ComputeJointLocalsLayered(*gltf.model, split, 3, layered);
        const float dev3 = maxDev(layered, direct);
        MYE_LOG_INFO("  layered 3-way split of one pose max deviation = %.8f", dev3);
        check(dev3 < 1e-5f, "layered: splitting one pose over three layers reproduces it");

        // 層の並びとプログラムの評価が決定的 (同じ入力で 2 回評価してビット一致)
        const SkeletalLayer three[] = { { 0, 0.2f, 30000 }, { -1, 0.0f, 10000 }, { 0, 0.9f, 25536 } };
        std::vector<XMMATRIX> again;
        ComputeJointLocalsLayered(*gltf.model, three, 3, layered);
        ComputeJointLocalsLayered(*gltf.model, three, 3, again);
        check(bitEqual(layered, again), "layered: repeated evaluation is bit-identical");
    }
    {
        // 入口の判定と部位追従のキャッシュキー
        SkinnedMeshComponent a, b;
        check(!UsesLocalsPath(a), "entry: the old path without a fade uses the palette path");
        a.fadeTotal = 4;
        a.fadeElapsed = 1;
        check(UsesLocalsPath(a), "entry: a fade uses the locals path");
        a.fadeTotal = 0;
        a.fadeElapsed = 0;
        a.poseLayerCount = 1;
        check(UsesLocalsPath(a), "entry: a pose program uses the locals path");
        a.poseLayerCount = 0;

        // 旧経路: 終わったフェードの残骸だけが違うなら同じポーズ
        b.fromClip = 3;
        b.fromTimeTicks = 17;
        b.fadeTotal = 5;
        b.fadeElapsed = 5;
        check(SamePoseInputs(a, b), "key: leftovers of a finished fade do not split the key");
        b = a;
        b.timeTicks = 1;
        check(!SamePoseInputs(a, b), "key: a different time is a different pose");

        // プログラム: 層の数・中身で割れる。使っていない層の中身では割れない
        a.poseLayerCount = 1;
        a.poseLayers[0] = { 0, 256, 65536 };
        b = a;
        b.clip = 5;          // 駆動中は旧経路の欄を見ない
        b.poseLayers[3] = { 2, 9, 9 };
        check(SamePoseInputs(a, b), "key: a program ignores old-path fields and unused layers");
        b.poseLayers[0].prevTimeQ = 100;
        b.poseLayers[0].stepQ = 156;
        check(SamePoseInputs(a, b), "key: the render-only interpolation fields (M89f) do not split the key");
        b.poseLayers[0].timeQ = 257;
        check(!SamePoseInputs(a, b), "key: a different layer time is a different pose");
        b = a;
        b.poseLayerCount = 0;
        check(!SamePoseInputs(a, b), "key: a program and the old path never share a key");
    }
    {
        // claim 手順: 書かれた tick は旧経路の時計を止め、印を下ろす。書かれなくなった tick に旧経路へ戻る。
        // 駆動中の clip 直書きは無視される (駆動を外した後にフェードとして遅れて効かない)
        SkinnedMeshComponent* sm = FirstSkinnedComponent(fbxScene);
        if (sm != nullptr && !fbx.model->clips.empty()) {
            World& w = fbxScene.GetWorld();
            SkinningSystem skinning;
            sm->clip = -1; // 範囲外 = 旧経路の時計は進まない
            sm->playing = true;
            sm->loop = 1;
            sm->fadeTicks = 8;
            sm->fadeTotal = 0;
            sm->fadeElapsed = 0;
            sm->timeTicks = 10;
            skinning.Update(w, resources);

            sm->poseLayerCount = 1;
            sm->poseLayers[0] = { 0, 0, SkinnedMeshComponent::kPoseWeightOne };
            sm->poseClaim = 1;
            sm->clip = 0; // 駆動中の直書き
            skinning.Update(w, resources);
            check(sm->poseClaim == 0 && sm->poseLayerCount == 1 && sm->timeTicks == 10
                      && sm->observedClip == 0 && sm->fadeTotal == 0,
                  "claim: a claimed tick keeps the program, lowers the flag and freezes the old clock");

            skinning.Update(w, resources);
            check(sm->poseLayerCount == 0 && sm->timeTicks == 11 && !IsSkinFading(*sm),
                  "claim: an unclaimed tick drops the program and resumes the old path without a "
                  "late fade");
        }
    }

    // ---- (M89b) 生成素材 anim_test.glb / anim_test_zup.glb (tools\gen_anim_test_gltf.ps1) ----
    // 名前付きの 4 クリップが読めること、Y-up 版と Z-up 版がワールドで同じ姿勢になること、
    // ルートの動きが「ルートの親空間 × エンティティの WorldMatrix」で水平に出ること (ルートモーションの下調べ)
    {
        Scene yScene;
        Scene zScene;
        const GameObject yRoot =
            ModelLoader::Load(yScene, resources, shaders, assetsRoot + L"\\models\\anim_test.glb");
        const GameObject zRoot =
            ModelLoader::Load(zScene, resources, shaders, assetsRoot + L"\\models\\anim_test_zup.glb");
        check(bool(yRoot) && bool(zRoot), "anim_test: both generated glTF files load headless");
        yScene.GetWorld().ApplyStructuralChanges();
        zScene.GetWorld().ApplyStructuralChanges();
        transforms.Update(yScene.GetWorld());
        transforms.Update(zScene.GetWorld());
        const LoadedSkin ySkin = FindSkinned(yScene, resources);
        const LoadedSkin zSkin = FindSkinned(zScene, resources);
        check(ySkin.model != nullptr && zSkin.model != nullptr, "anim_test: both register a SkinnedModel");
        if (ySkin.model != nullptr && zSkin.model != nullptr) {
            const SkinnedModel& ym = *ySkin.model;
            struct ClipSpec {
                const char* name;
                int32_t ticks;
            };
            const ClipSpec specs[] = { { "Idle", 120 }, { "Walk", 60 }, { "Run", 36 }, { "Attack", 48 } };
            bool clipsOk = ym.joints.size() == 17 && zSkin.model->joints.size() == 17 && ym.clips.size() == 4;
            for (const ClipSpec& c : specs) {
                const int32_t yi = ym.FindClipByHash(HashStr(c.name));
                const int32_t zi = zSkin.model->FindClipByHash(HashStr(c.name));
                clipsOk = clipsOk && yi >= 0 && zi >= 0
                          && SkeletalClipTicks(ym.clips[static_cast<size_t>(yi)]) == c.ticks
                          && SkeletalClipTicks(zSkin.model->clips[static_cast<size_t>(zi)]) == c.ticks;
            }
            check(clipsOk && ym.joints[0].name == "Root" && ym.joints[0].parent == -1
                      && ym.FindJointByName("Foot.L") >= 0,
                  "anim_test: 17 joints (Root first), clips Idle/Walk/Run/Attack = 120/60/36/48 ticks by name");

            // 全ジョイントのワールド位置 = JointGlobal * entityWorld が Y-up と Z-up で一致する
            const auto worldJoint = [](const LoadedSkin& skin, int clip, float t, int32_t joint) {
                const XMMATRIX m = XMMatrixMultiply(ComputeJointGlobal(*skin.model, clip, t, joint),
                                                    XMLoadFloat4x4(&skin.entityWorld));
                const XMFLOAT4X4 f = ToF4x4(m);
                return XMFLOAT3{ f._41, f._42, f._43 };
            };
            float maxDiff = 0.0f;
            for (const ClipSpec& c : specs) {
                const int yi = ym.FindClipByHash(HashStr(c.name));
                const int zi = zSkin.model->FindClipByHash(HashStr(c.name));
                for (int tick : { 0, 7, 20 }) {
                    const float t = static_cast<float>(tick) / 60.0f;
                    for (int32_t j = 0; j < static_cast<int32_t>(ym.joints.size()); ++j) {
                        const XMFLOAT3 a = worldJoint(ySkin, yi, t, j);
                        const XMFLOAT3 b = worldJoint(zSkin, zi, t, j);
                        maxDiff = (std::max)(maxDiff, (std::max)({ std::fabs(a.x - b.x), std::fabs(a.y - b.y),
                                                                   std::fabs(a.z - b.z) }));
                    }
                }
            }
            MYE_LOG_INFO("  [anim_test] max world joint difference Y-up vs Z-up = %g", maxDiff);
            check(maxDiff < 1e-4f, "anim_test: the Y-up and Z-up files pose identically in world space");

            // ルートの 1 周の移動: ワールドで水平・正面 (エンジンの -Z、ローダの Z 反転) へ 1.4 m。
            // Z-up 版ではルートの局所移動そのものは縦軸 (Y) に出る = 上向きはルートの親空間ではなく
            // エンティティ側 (非ジョイント祖先の変換) から求める必要がある
            const int yWalk = ym.FindClipByHash(HashStr("Walk"));
            const int zWalk = zSkin.model->FindClipByHash(HashStr("Walk"));
            const XMFLOAT3 y0 = worldJoint(ySkin, yWalk, 0.0f, 0);
            const XMFLOAT3 y1 = worldJoint(ySkin, yWalk, 1.0f, 0);
            const XMFLOAT3 z1 = worldJoint(zSkin, zWalk, 1.0f, 0);
            const XMFLOAT4X4 zLocalEnd = ToF4x4(ComputeJointGlobal(*zSkin.model, zWalk, 1.0f, 0));
            check(std::fabs(y1.x - y0.x) < 1e-5f && std::fabs(y1.y - y0.y) < 1e-5f
                      && std::fabs((y1.z - y0.z) + 1.4f) < 1e-4f && std::fabs(z1.z + 1.4f) < 1e-4f
                      && std::fabs(z1.y) < 1e-4f,
                  "anim_test: one Walk cycle moves the root 1.4 m forward (-Z) and horizontally in world space");
            check(std::fabs(zLocalEnd._42 + 1.4f) < 1e-4f && std::fabs(zLocalEnd._43) < 1e-4f,
                  "anim_test (Z-up): the root's own translation runs along its parent's Y axis (up comes from the entity side)");
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
