#include "Engine/Engine/Animation/SkinningSystem.h"

#include <algorithm>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/Mesh/Skeleton.h"

namespace mye {

void SkinningSystem::Update(World& world, const RenderResources& resources)
{
    const ComponentTypeId req[] = { SkinnedMeshComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int si = arch.FindTypeIndex(SkinnedMeshComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            auto* sm = static_cast<SkinnedMeshComponent*>(arch.GetPtr(si, row));

            // ---- ポーズプログラムの claim (M89a) ----
            // この tick にコントローラが書いた = 時計はあちらが持つので旧経路は進めない。
            // 駆動中の clip 直書きは無視する: 観測済みにしておかないと、駆動を外した tick に
            // 遅れて「切り替え」として効いてしまう
            if (sm->poseClaim != 0) {
                sm->poseClaim = 0;
                sm->observedClip = sm->clip;
                continue;
            }
            sm->poseLayerCount = 0;

            // ---- clip の書き換え = 切り替え (M18 追補) ----
            // ★playing より先に見る。止めたまま clip を変えて再開したとき、再開の tick に
            //   「前のクリップの時刻のまま新しいクリップを途中から」再生しないため
            // ★初めて見る clip (ロード / 生成直後) は切り替えとして扱わない — シーンに保存
            //   された timeTicks を 0 に潰すと、既存シーンの見た目が変わる
            if (sm->observedClip != sm->clip) {
                if (sm->observedClip != SkinnedMeshComponent::kClipUnobserved) {
                    if (sm->fadeTicks > 0) {
                        // フェード中にさらに切り替えたら、元は「直前の目標クリップ」になる。
                        // 混ざりかけの姿勢そのものは持たない = 状態が int 数本で閉じる
                        // (snapshot に載せられる / 描画側が純関数のまま評価できる)
                        sm->fromClip = sm->observedClip;
                        sm->fromTimeTicks = sm->timeTicks;
                        sm->fadeTotal = sm->fadeTicks;
                        sm->fadeElapsed = 0;
                    } else {
                        sm->fadeTotal = 0;
                        sm->fadeElapsed = 0;
                    }
                    sm->timeTicks = 0;
                }
                sm->observedClip = sm->clip;
            }

            if (!sm->playing) {
                continue;
            }
            if (sm->fadeElapsed < sm->fadeTotal) {
                ++sm->fadeElapsed;
            }
            const SkinnedModel* model = resources.skinnedModels.Get(sm->model);
            if (!model || sm->clip < 0 || sm->clip >= static_cast<int>(model->clips.size())) {
                continue;
            }
            // 60Hz 前提でループ (末尾で 0 に戻す)
            const int durTicks = SkeletalClipTicks(model->clips[static_cast<size_t>(sm->clip)]);
            sm->timeTicks += 1;
            if (durTicks > 0 && sm->timeTicks >= durTicks) {
                // 一度きりは durTicks ちょうどで止める。サンプラは末尾キー以降をクランプするので
                // これが最後のコマ = 「警戒で身構えたまま」「死んだまま」の姿勢になる
                sm->timeTicks = (sm->loop != 0) ? 0 : durTicks;
            }
        }
    });
}

int32_t SkeletalClipTicks(const SkeletalClip& clip)
{
    return (clip.duration > 0.0f) ? static_cast<int32_t>(clip.duration * 60.0f + 0.5f) : 0;
}

bool IsSkinFading(const SkinnedMeshComponent& sm)
{
    return sm.fadeTotal > 0 && sm.fadeElapsed < sm.fadeTotal;
}

namespace {

// 壊れた値 (範囲外の層数) で配列の外を読まない
int32_t ActivePoseLayers(const SkinnedMeshComponent& sm)
{
    return std::clamp(sm.poseLayerCount, 0, SkinnedMeshComponent::kMaxPoseLayers);
}

} // namespace

bool UsesLocalsPath(const SkinnedMeshComponent& sm)
{
    return ActivePoseLayers(sm) > 0 || IsSkinFading(sm);
}

bool SamePoseInputs(const SkinnedMeshComponent& a, const SkinnedMeshComponent& b)
{
    const int32_t layers = ActivePoseLayers(a);
    if (layers != ActivePoseLayers(b)) {
        return false;
    }
    if (layers > 0) {
        for (int32_t i = 0; i < layers; ++i) {
            const SkinnedMeshComponent::PoseLayer& la = a.poseLayers[i];
            const SkinnedMeshComponent::PoseLayer& lb = b.poseLayers[i];
            if (la.clip != lb.clip || la.timeQ != lb.timeQ || la.weightQ != lb.weightQ) {
                return false;
            }
        }
        return true;
    }
    if (a.clip != b.clip || a.timeTicks != b.timeTicks) {
        return false;
    }
    const bool fading = IsSkinFading(a);
    if (fading != IsSkinFading(b)) {
        return false;
    }
    return !fading
           || (a.fromClip == b.fromClip && a.fromTimeTicks == b.fromTimeTicks
               && a.fadeElapsed == b.fadeElapsed && a.fadeTotal == b.fadeTotal);
}

void SampleSkinnedLocals(const SkinnedModel& model, const SkinnedMeshComponent& sm,
                         std::vector<DirectX::XMMATRIX>& outLocals)
{
    SampleSkinnedLocalsInterpolated(model, sm, 1.0f, outLocals);
}

void SampleSkinnedLocalsInterpolated(const SkinnedModel& model, const SkinnedMeshComponent& sm,
                                     float interpAlpha, std::vector<DirectX::XMMATRIX>& outLocals)
{
    // ---- ポーズプログラム (M89a) ----
    if (const int32_t layers = ActivePoseLayers(sm); layers > 0) {
        static_assert(SkinnedMeshComponent::kMaxPoseLayers <= kMaxSkeletalLayers,
                      "the sampler must accept every layer a pose program can hold");
        // 秒 = timeQ / (60 * 256)。timeQ が 256 の倍数なら旧経路の timeTicks / 60.0f とビット一致する
        // (分子と分母に同じ 2 の冪を掛けた商は、正しく丸めた結果が変わらない。|timeQ| < 2^24 の範囲)
        constexpr float kTimeQPerSecond =
            60.0f * static_cast<float>(SkinnedMeshComponent::kPoseTimeQPerTick);
        // NaN は補間しない側へ倒す (!(a < 1) は NaN でも真)
        const bool interpolate = interpAlpha < 1.0f;
        const float alpha = interpolate ? std::max(interpAlpha, 0.0f) : 1.0f;
        SkeletalLayer program[SkinnedMeshComponent::kMaxPoseLayers] = {};
        for (int32_t i = 0; i < layers; ++i) {
            const SkinnedMeshComponent::PoseLayer& src = sm.poseLayers[i];
            program[i].clip = src.clip;
            if (interpolate && src.stepQ != 0) {
                // 描画専用なので float の積和でよい (sim の状態には戻らない)
                program[i].timeSec = (static_cast<float>(src.prevTimeQ) + static_cast<float>(src.stepQ) * alpha)
                                     / kTimeQPerSecond;
            } else {
                program[i].timeSec = static_cast<float>(src.timeQ) / kTimeQPerSecond;
            }
            program[i].weight = src.weightQ;
        }
        ComputeJointLocalsLayered(model, program, layers, outLocals);
        return;
    }
    // 時刻式は M18 の RenderSystem と同一 (同じ tick で同じポーズ)
    const float timeSec = static_cast<float>(sm.timeTicks) / 60.0f;
    if (!IsSkinFading(sm)) {
        ComputeJointLocals(model, sm.clip, timeSec, outLocals);
        return;
    }
    const float fromSec = static_cast<float>(sm.fromTimeTicks) / 60.0f;
    // 重みは整数の比から作る (float の累積を持たない = snapshot から戻しても同じ重み)
    const float weight = static_cast<float>(sm.fadeElapsed) / static_cast<float>(sm.fadeTotal);
    ComputeJointLocalsBlended(model, sm.fromClip, fromSec, sm.clip, timeSec, weight, outLocals);
}

} // namespace mye
