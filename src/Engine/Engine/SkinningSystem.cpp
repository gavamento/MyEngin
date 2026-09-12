#include "Engine/Engine/SkinningSystem.h"

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/Skeleton.h"

namespace mye {

void SkinningSystem::Update(World& world, const RenderResources& resources)
{
    const ComponentTypeId req[] = { SkinnedMeshComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int si = arch.FindTypeIndex(SkinnedMeshComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            auto* sm = static_cast<SkinnedMeshComponent*>(arch.GetPtr(si, row));

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
            // クリップ長 (秒) → tick。60Hz 前提でループ (末尾で 0 に戻す)
            const float durSec = model->clips[static_cast<size_t>(sm->clip)].duration;
            const int durTicks = (durSec > 0.0f) ? static_cast<int>(durSec * 60.0f + 0.5f) : 0;
            sm->timeTicks += 1;
            if (durTicks > 0 && sm->timeTicks >= durTicks) {
                // 一度きりは durTicks ちょうどで止める。サンプラは末尾キー以降をクランプするので
                // これが最後のコマ = 「警戒で身構えたまま」「死んだまま」の姿勢になる
                sm->timeTicks = (sm->loop != 0) ? 0 : durTicks;
            }
        }
    });
}

bool IsSkinFading(const SkinnedMeshComponent& sm)
{
    return sm.fadeTotal > 0 && sm.fadeElapsed < sm.fadeTotal;
}

void SampleSkinnedLocals(const SkinnedModel& model, const SkinnedMeshComponent& sm,
                         std::vector<DirectX::XMMATRIX>& outLocals)
{
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
