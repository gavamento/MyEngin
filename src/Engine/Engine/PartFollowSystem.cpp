#include "Engine/Engine/PartFollowSystem.h"

#include <cmath>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Parts.h"   // ResolvePartSource (Inspector と共用)
#include "Engine/Engine/Ragdoll.h" // M60g1: 駆動方向の判定 (物理・描画と共用)
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/Skeleton.h"

using namespace DirectX;

namespace mye {
namespace {

// 警告キー (entity index を上位、理由コードを下位に詰める)。ログ抑制専用
enum class WarnKind : uint64_t { NotChild = 1, NoModel = 2, NoJoint = 3, NoSource = 4 };
uint64_t WarnKey(EntityID e, WarnKind k)
{
    return (static_cast<uint64_t>(e.index) << 8) | static_cast<uint64_t>(k);
}

// 行ベクトル規約の行列 (M = S * R * T) を TRS へ分解する。
//
// ★`XMMatrixDecompose` を使わない: 内部が反復 (極分解) で、最適化とレジスタ割り当てに
//   結果が左右されうる。ここは **hash 対象の LocalTransform を作る場所** なので、
//   Debug/Release でビットが違ったら即リプレイ不一致になる。四則と sqrt だけの
//   固定手順で書けば /fp:precise の下でビット再現する。
//
// v1 の制限: 鏡映 (行列式 < 0) は扱わない — ボーンに負のスケールが入っているモデルでは
// スケールの符号が落ちる。骨アニメでは実用上出ないので v1 は割り切る
void DecomposeRowMajorTRS(const XMMATRIX& m, XMFLOAT3& outT, XMFLOAT4& outR, XMFLOAT3& outS)
{
    XMFLOAT4X4 f;
    XMStoreFloat4x4(&f, m);

    outT = { f._41, f._42, f._43 }; // 行ベクトル規約では平行移動は第 4 行

    // 各基底行の長さ = スケール。0 は 1 とみなす (回転行列を作れなくなるため)
    const float sx = std::sqrt(f._11 * f._11 + f._12 * f._12 + f._13 * f._13);
    const float sy = std::sqrt(f._21 * f._21 + f._22 * f._22 + f._23 * f._23);
    const float sz = std::sqrt(f._31 * f._31 + f._32 * f._32 + f._33 * f._33);
    outS = { sx, sy, sz };
    const float ix = (sx > 0.0f) ? 1.0f / sx : 1.0f;
    const float iy = (sy > 0.0f) ? 1.0f / sy : 1.0f;
    const float iz = (sz > 0.0f) ? 1.0f / sz : 1.0f;

    // 正規直交化した回転部分 (r[行][列])
    const float r00 = f._11 * ix, r01 = f._12 * ix, r02 = f._13 * ix;
    const float r10 = f._21 * iy, r11 = f._22 * iy, r12 = f._23 * iy;
    const float r20 = f._31 * iz, r21 = f._32 * iz, r22 = f._33 * iz;

    // 行列 → クォータニオン (Shepperd)。**分岐順は固定** — 最大成分を選ぶことで
    // 0 除算と桁落ちを避ける古典手法で、比較は厳密 (> のみ) なので構成間で分岐が割れない
    const float trace = r00 + r11 + r22;
    float qx, qy, qz, qw;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f; // s = 4*qw
        qw = 0.25f * s;
        qx = (r12 - r21) / s;
        qy = (r20 - r02) / s;
        qz = (r01 - r10) / s;
    } else if (r00 > r11 && r00 > r22) {
        const float s = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f; // s = 4*qx
        qw = (r12 - r21) / s;
        qx = 0.25f * s;
        qy = (r10 + r01) / s;
        qz = (r20 + r02) / s;
    } else if (r11 > r22) {
        const float s = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f; // s = 4*qy
        qw = (r20 - r02) / s;
        qx = (r10 + r01) / s;
        qy = 0.25f * s;
        qz = (r21 + r12) / s;
    } else {
        const float s = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f; // s = 4*qz
        qw = (r01 - r10) / s;
        qx = (r20 + r02) / s;
        qy = (r21 + r12) / s;
        qz = 0.25f * s;
    }
    outR = { qx, qy, qz, qw };
}

} // namespace

void PartFollowSystem::Update(World& world, const RenderResources& resources)
{
    // 1) 追従する部位を集める。**ForEachArchetype の途中で他アーキタイプを触らない**
    //    (source の SkinnedMesh 読み出しはイテレーション外でやる)
    struct Job {
        EntityID part;
        EntityID source;
        const char* joint;
    };
    std::vector<Job> jobs;
    {
        const ComponentTypeId req[] = { PartComponent::sTypeId, LocalTransform::sTypeId };
        world.ForEachArchetype(req, [&](Archetype& arch) {
            const int pi = arch.FindTypeIndex(PartComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                auto* p = static_cast<PartComponent*>(arch.GetPtr(pi, row));
                if (p->joint[0] == '\0') {
                    continue; // 静的ソケット — 親に対して固定なので何もしない
                }
                jobs.push_back({ arch.EntityAt(row), p->source, p->joint });
            }
        });
    }
    if (jobs.empty()) {
        return; // Part 非使用シーンでは完全 no-op (= 既存シーンのリプレイ不変)
    }

    // 2) (model, clip, timeTicks) 単位でジョイント局所行列をキャッシュする。
    //    ComputeJointGlobal は 1 回ごとに全ジョイントを再評価するので、部位ごとに呼ぶと
    //    O(部位数 × ジョイント数) になる (M48a の申し送り)
    struct PoseCache {
        const SkinnedModel* model;
        int clip;
        int timeTicks;
        std::vector<XMMATRIX> locals;
    };
    std::vector<PoseCache> poses;

    for (const Job& job : jobs) {
        // ---- 供給元の解決 (M48i で Parts:: に 1 本化 — Inspector のジョイント一覧と同じ答え) ----
        const EntityID src = Parts::ResolvePartSource(world, job.part, job.source);
        if (src.IsNull()) {
            if (warned_.insert(WarnKey(job.part, WarnKind::NoSource)).second) {
                MYE_LOG_WARN("[part] '%s': no SkinnedMesh source (set Part.source or put the part "
                             "under a skinned mesh)", world.GetName(job.part));
            }
            continue;
        }
        // ---- M60g1: ラグドールが作動中なら骨を駆動するのは物理のほう ----
        // ★ここで書くと同じ tick に「骨 → 部位」(この関数) と「物理 → 部位」(書き戻し) が
        //   両方 LocalTransform (hash 対象) を書いて二重駆動になる。物理は TickRunner の
        //   この直後に走るので、黙って上書きされて**アニメが勝ったり負けたりする**
        if (ragdoll::IsSourceDriven(world, src)) {
            continue;
        }
        // ---- v1 規約: 部位は source の直子 ----
        // ここを緩めるとワールド行列 (前 tick の値) を読む必要が出て決定論が脆くなる
        if (world.GetParent(job.part) != src) {
            if (warned_.insert(WarnKey(job.part, WarnKind::NotChild)).second) {
                MYE_LOG_WARN("[part] '%s': a bone-following part must be a direct child of its "
                             "skinned mesh (v1 rule) — skipped", world.GetName(job.part));
            }
            continue;
        }

        const auto* sm = world.GetComponent<SkinnedMeshComponent>(src);
        const SkinnedModel* model = resources.skinnedModels.Get(sm->model);
        if (!model) {
            // アセット未登録 — 前回値を保持して skip (勝手に恒等へ潰さない)
            if (warned_.insert(WarnKey(job.part, WarnKind::NoModel)).second) {
                MYE_LOG_WARN("[part] '%s': skinned model is not registered — part stays put",
                             world.GetName(job.part));
            }
            continue;
        }
        const int32_t jointIndex = model->FindJointByName(job.joint);
        if (jointIndex < 0) {
            if (warned_.insert(WarnKey(job.part, WarnKind::NoJoint)).second) {
                MYE_LOG_WARN("[part] '%s': joint '%s' not found in the skinned model",
                             world.GetName(job.part), job.joint);
            }
            continue;
        }

        // **index で持つこと** — push_back で vector が再確保されるとポインタは失効する
        size_t poseIdx = poses.size();
        for (size_t i = 0; i < poses.size(); ++i) {
            if (poses[i].model == model && poses[i].clip == sm->clip
                && poses[i].timeTicks == sm->timeTicks) {
                poseIdx = i;
                break;
            }
        }
        if (poseIdx == poses.size()) {
            PoseCache c;
            c.model = model;
            c.clip = sm->clip;
            c.timeTicks = sm->timeTicks;
            // 時刻式は描画側 (RenderSystem) と同一 — 同じ tick で同じポーズになる
            ComputeJointLocals(*model, c.clip, static_cast<float>(c.timeTicks) / 60.0f, c.locals);
            poses.push_back(std::move(c));
        }

        // 部位が source の直子なので partLocal = jointGlobal でワールドが閉じる (ヘッダ参照)
        auto* lt = world.GetComponent<LocalTransform>(job.part);
        if (!lt) {
            continue;
        }
        DecomposeRowMajorTRS(
            JointGlobalFromLocals(*model, poses[poseIdx].locals, jointIndex),
            lt->position, lt->rotation, lt->scale);
    }
}

} // namespace mye
