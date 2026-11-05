// InspectorWindow のナビメッシュ関連 (M82b〜)。NavMeshSurface / NavMeshAgent 節の末尾、ベイク結果の確定 (CommitReadyNavBakes)、
// 参照中の .mnav の要約キャッシュ。ベイクの確定ロジックそのものは Editor/Tools/NavBakeCommit.h (ImGui 非依存、SelfTest から呼べる)。
// エンティティ側の描画は InspectorWindow.cpp、アセットは InspectorWindowAssets.cpp
#include "Editor/Windows/Scene/InspectorWindow.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "Editor/Tools/NavBakeCommit.h"      // M82b: ベイク結果の確定 (.mnav 保存・参照の設定・Undo)
#include "Engine/Engine/Navigation/NavMeshAsset.h"
#include "Engine/Core/Asset/AssetGuidResolver.h"
#include "Engine/Engine/Asset/ModelCook.h" // M80j: .mmdl クックキャッシュからボーンウェイト付き頂点を読む
#include "Editor/Project/NavAreaNames.h"
#include "Editor/Project/NavAgentTypes.h"
#include "Editor/Scene/Selection.h"
#include "Editor/Undo/UndoStack.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Renderer/ImGui/ImGuiTheme.h"  // 見出しフォント (テーマ第 3 世代)

#include "imgui.h"

#include "fontawesome/IconsFontAwesome6.h"

namespace mye {

// M82b: 参照中の .mnav の要約。GUID ごとに 1 回だけ読む (ポリゴン数は dtNavMesh を組まないと数えられない)
const InspectorWindow::NavAssetSummary& InspectorWindow::GetNavAssetSummary(uint64_t guid)
{
    const auto it = navSummaryCache_.find(guid);
    if (it != navSummaryCache_.end()) {
        return it->second;
    }
    NavAssetSummary summary;
    // メモリ上にだけ登録された資産 (--nav-demo) もファイルの資産も、LoadByGuid が同じ順で引く (NavSystem の読み込みと同じ判定)
    NavMeshAsset::Data data;
    if (NavMeshAsset::LoadByGuid(guid, data)) {
        NavTileStore store;
        if (NavMeshAsset::BuildStore(data, store)) {
            const dtNavMesh* nav = store.NavMesh();
            for (int i = 0; i < nav->getMaxTiles(); ++i) {
                const dtMeshTile* tile = nav->getTile(i);
                if (tile != nullptr && tile->header != nullptr) {
                    ++summary.tiles;
                    summary.polygons += tile->header->polyCount;
                }
            }
            summary.layers = static_cast<int>(data.layers.size());
            summary.agentRadius = data.config.agentRadius;
            summary.agentHeight = data.config.agentHeight;
            summary.maxClimb = data.config.agentMaxClimb;
            summary.maxSlopeDeg = data.config.agentMaxSlopeDeg;
            summary.generateLinks = data.config.generateLinks != 0;
            summary.dropHeight = data.config.linkDropHeight;
            summary.jumpDistance = data.config.linkJumpDistance;
            summary.generatedLinks = static_cast<int>(data.links.size());
            const std::wstring path = assetguid::ResolvePath(guid);
            std::error_code ec;
            std::vector<uint8_t> bytes;
            size_t size = 0;
            if (!path.empty() && std::filesystem::exists(path, ec)) {
                size = static_cast<size_t>(std::filesystem::file_size(path, ec));
            } else {
                NavMeshAsset::Serialize(data, bytes);
                size = bytes.size();
            }
            summary.kilobytes = static_cast<int>(size / 1024);
            summary.loadable = true;
        }
    }
    return navSummaryCache_.emplace(guid, summary).first->second;
}

// M82c: NavMeshAgent 節の末尾。状態は NavSystem が毎 tick 書く値 (編集中は前回の Play の値のまま)。
// 警告は「動かない / 食い込む」原因になる組み合わせだけ: CC 無し、Rigidbody で CC 無効、Surface 無し、
// ベイク寸法より大きい (ベイクの寸法は Surface のコンポーネントが持つ = CC と共有できない、M82 spec 2. #6)
void InspectorWindow::DrawNavMeshAgentNotes(EngineContext& ctx, Selection& selection, UndoStack& undo,
                                            const InspectorTargets& tg)
{
    World& world = ctx.scene->GetWorld();
    const auto* agent = world.GetComponent<NavMeshAgentComponent>(tg.e);
    if (agent == nullptr) {
        return;
    }
    ImGui::Separator();
    const char* names[] = { Tr(StrId::Insp_NavSt_Idle),     Tr(StrId::Insp_NavSt_Moving),
                            Tr(StrId::Insp_NavSt_Arrived),  Tr(StrId::Insp_NavSt_NoPath),
                            Tr(StrId::Insp_NavSt_OnLink),   Tr(StrId::Insp_NavSt_Inactive),
                            Tr(StrId::Insp_NavSt_Stuck) };
    const int status = agent->status >= 0 && agent->status < 7 ? agent->status : navagentstatus::kInactive;
    ImGui::TextDisabled(Tr(StrId::Insp_NavAgentStatus), names[status], agent->remainingDistance,
                        agent->pathPartial ? Tr(StrId::Insp_NavAgentPartial) : "");
    if (agent->status == navagentstatus::kOnLink) {
        ImGui::TextDisabled(Tr(StrId::Insp_NavLinkCrossing), agent->linkStart.x, agent->linkStart.y, agent->linkStart.z,
                            agent->linkEnd.x, agent->linkEnd.y, agent->linkEnd.z);
    }

    // 歩けるエリア (areaMask)。エリア 1 (歩行不可) は常に歩けないので出さない。名前は project_settings.json の navAreas
    if (ImGui::TreeNode(Tr(StrId::Insp_NavAreaMask))) {
        NavAreaNames& areaNames = NavAreaNames::Get();
        areaNames.Load(ctx.assetsRoot);
        for (int i = 0; i < kNavAreaCount; ++i) {
            if (i == kNavAreaNotWalkable) {
                continue;
            }
            bool allowed = (agent->areaMask & (1u << i)) != 0;
            ImGui::PushID(i);
            if (ImGui::Checkbox(areaNames.Name(i), &allowed)) {
                const uint32_t bit = 1u << i;
                undo.Record("Edit Agent Area Mask", *ctx.scene, selection, tg.fid, UndoStack::StructuralChanges::None, [&] {
                    if (auto* editable = world.GetComponent<NavMeshAgentComponent>(tg.e)) {
                        editable->areaMask = allowed ? (editable->areaMask | bit) : (editable->areaMask & ~bit);
                    }
                });
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }

    const auto* cc = world.GetComponent<CharacterControllerComponent>(tg.e);
    if (cc == nullptr) {
        ImGui::TextColored(themeColor::Warning, "%s", Tr(StrId::Insp_NavAgentNoCc));
    } else if (world.GetComponent<RigidbodyComponent>(tg.e) != nullptr) {
        ImGui::TextColored(themeColor::Warning, "%s", Tr(StrId::Insp_NavAgentRigidbody));
    }
    if (world.GetComponent<AgentBrainComponent>(tg.e) != nullptr) {
        ImGui::TextColored(themeColor::Warning, "%s", Tr(StrId::Insp_NavAgentBrain));
    }

    // 同じ agentTypeId の Surface のうちキーが最小のもの (NavSystem の割り当てと同じ規則)
    const NavMeshSurfaceComponent* surface = nullptr;
    EntityID surfaceEntity = kNullEntity;
    const ComponentTypeId req[] = { NavMeshSurfaceComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int si = arch.FindTypeIndex(NavMeshSurfaceComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const auto* s = static_cast<const NavMeshSurfaceComponent*>(arch.GetPtr(si, row));
            const EntityID e = arch.EntityAt(row);
            const bool better = surface == nullptr || e.index < surfaceEntity.index
                || (e.index == surfaceEntity.index && e.generation < surfaceEntity.generation);
            if (s->agentTypeId == agent->agentTypeId && better) {
                surface = s;
                surfaceEntity = e;
            }
        }
    });
    if (surface == nullptr) {
        ImGui::TextColored(themeColor::Warning, "%s", Tr(StrId::Insp_NavAgentNoSurface));
        return;
    }
    const float radius = (std::max)(agent->radius, cc != nullptr ? cc->radius : 0.0f);
    const float height = (std::max)(agent->height, cc != nullptr ? cc->height : 0.0f);
    if (radius > surface->agentRadius + 1e-4f || height > surface->agentHeight + 1e-4f) {
        ImGui::TextColored(themeColor::Warning, Tr(StrId::Insp_NavAgentTooBig), surface->agentRadius,
                           surface->agentHeight);
    }
    // 経路はあるのに CC が登れない段差 (M82d)
    const auto* worldMatrix = world.GetComponent<WorldMatrixComponent>(tg.e);
    float worldScaleY = 1.0f; // 実効の段差 = stepOffset x |scale.y| (PhysicsSystem と同じ)
    if (worldMatrix != nullptr) {
        const auto& m = worldMatrix->value;
        worldScaleY = std::sqrt(m._21 * m._21 + m._22 * m._22 + m._23 * m._23);
    }
    if (cc != nullptr && NavAgentStepBelowClimb(*cc, worldScaleY, *surface)) {
        ImGui::PushTextWrapPos(0.0f); // 長い警告がパネル幅で切れないように折り返す
        ImGui::TextColored(themeColor::Warning, Tr(StrId::Insp_NavAgentStepLow), cc->stepOffset * worldScaleY,
                           surface->maxClimb);
        ImGui::PopTextWrapPos();
    }
}

void InspectorWindow::CommitReadyNavBakes(EngineContext& ctx, Selection& selection, UndoStack& undo)
{
    // ドラッグなど複数フレームの Undo 記録中は確定を持ち越す (結果は Ready のまま残り、記録が閉じたフレームで取り込む)
    if (undo.IsRecording()) {
        return;
    }
    World& world = ctx.scene->GetWorld();
    for (const uint64_t fid : navBakeService_.ReadyIds()) {
        NavBakeResult result;
        if (!navBakeService_.TakeResult(fid, result)) {
            continue;
        }
        const GameObject go = ctx.scene->FindByFileId(fid);
        const EntityID e = go ? go.Id() : kNullEntity;
        if (!world.IsAlive(e)) {
            MYE_LOG_WARN("[nav] a bake finished but its NavMeshSurface no longer exists; the result is discarded");
            continue;
        }
        NavBakeOutcome outcome;
        outcome.status = result.output.status;
        outcome.message = result.output.message;
        outcome.triangleCount = result.output.data.inputTriangleCount;
        outcome.elapsedMs = static_cast<int>(result.elapsedMs);
        if (result.output.status == NavBakeStatus::Ok && !CommitNavBake(ctx, selection, undo, e, fid, result.output)) {
            outcome.status = NavBakeStatus::Failed;
            outcome.message = "failed to write the .mnav asset";
        }
        if (outcome.status == NavBakeStatus::Failed || outcome.status == NavBakeStatus::Empty) {
            MYE_LOG_ERROR("[nav] bake failed for '%s': %s", world.GetName(e), outcome.message.c_str());
        }
        navBakeOutcomes_[fid] = std::move(outcome);
        if (const auto* comp = world.GetComponent<NavMeshSurfaceComponent>(e)) {
            navSummaryCache_.erase(comp->navAsset.value); // 同じ名前のファイルを書き直した場合に読み直す
        }
    }
}

// M82b: NavMeshSurface 節の末尾。ベイクの確定 (メインスレッド限定の .mnav 保存・AssetDatabase 登録・Undo) は
// CommitNavBake (NavBakeCommit.h) が持つ (結果の取り込みは CommitReadyNavBakes) — ImGui に触れない純粋なロジックなので SelfTest からも呼べる
void InspectorWindow::DrawNavMeshSurfaceNotes(EngineContext& ctx, Selection& selection, UndoStack& undo,
                                              const InspectorTargets& tg)
{
    World& world = ctx.scene->GetWorld();
    const auto* comp = world.GetComponent<NavMeshSurfaceComponent>(tg.e);
    if (comp == nullptr) {
        return;
    }

    ImGui::Separator();
    // Agent Type との食い違い (M84a)。Project Settings を変えても開いているシーンは自動では変えない
    NavAgentTypes& agentTypes = NavAgentTypes::Get();
    agentTypes.Load(ctx.assetsRoot);
    const NavAgentType* agentType = agentTypes.Find(comp->agentTypeId);
    ImGui::PushTextWrapPos(0.0f);
    if (agentType == nullptr) {
        ImGui::TextColored(themeColor::Warning, Tr(StrId::Insp_NavAgentTypeMissing), comp->agentTypeId);
    } else if (!NavSurfaceMatchesAgentType(*comp, *agentType)) {
        ImGui::TextColored(themeColor::Warning, Tr(StrId::Insp_NavAgentTypeDiffers), agentType->name);
    }
    // 同じ Agent Type の Surface はまとめて 1 つのナビメッシュになる (M84b)。leader の設定と .mnav を使う
    NavSurfaceGroup group;
    if (NavFindSurfaceGroup(world, tg.e, group) && group.members.size() > 1) {
        ImGui::TextDisabled(Tr(StrId::Insp_NavGroupInfo), static_cast<int>(group.members.size()));
        if (group.leader != tg.e) {
            ImGui::TextDisabled(Tr(StrId::Insp_NavGroupLeader), world.GetName(group.leader));
        }
        const uint64_t leaderAsset = world.GetComponent<NavMeshSurfaceComponent>(group.leader)->navAsset.value;
        bool shared = true;
        for (const EntityID member : group.members) {
            shared = shared && world.GetComponent<NavMeshSurfaceComponent>(member)->navAsset.value == leaderAsset;
        }
        if (!shared) {
            ImGui::TextColored(themeColor::Warning, "%s", Tr(StrId::Insp_NavGroupStale));
        }
    }
    ImGui::PopTextWrapPos();
    // 実際に使うセルの大きさと実効の傾斜上限 (M82d)
    {
        const NavCellSize cellSize = NavResolveCellSize(*comp);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled(Tr(StrId::Insp_NavCellInfo), cellSize.cellSize, cellSize.cellHeight,
                            Tr(comp->autoCellSize ? StrId::Insp_NavCellAuto : StrId::Insp_NavCellManual),
                            NavEffectiveSlopeLimitDeg(*comp, cellSize.cellSize));
        if (cellSize.clampedToMinimum) {
            ImGui::TextColored(themeColor::Warning, "%s", Tr(StrId::Insp_NavCellAtMinimum));
        } else if (NavSurfaceSlopeUnreachable(*comp)) {
            ImGui::TextColored(themeColor::Warning, Tr(StrId::Insp_NavSlopeUnreachable), comp->maxSlopeDeg,
                               NavEffectiveSlopeLimitDeg(*comp, cellSize.cellSize));
        }
        ImGui::PopTextWrapPos();
    }
    // エリアのコスト (1 = 既定、大きいほど避ける)。エリア 1 (歩行不可) は通れないので行を出さない
    if (ImGui::TreeNode(Tr(StrId::Insp_NavAreaCosts))) {
        NavAreaNames& areaNames = NavAreaNames::Get();
        areaNames.Load(ctx.assetsRoot);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", Tr(StrId::Insp_NavAreaCostHint));
        ImGui::PopTextWrapPos();
        for (int i = 0; i < kNavAreaCount; ++i) {
            if (i == kNavAreaNotWalkable) {
                continue;
            }
            float cost = comp->areaCosts[i];
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(120.0f);
            const bool changed = ImGui::DragFloat(areaNames.Name(i), &cost, 0.1f, 1.0f, 1000.0f, "%.1f");
            // ドラッグ 1 回を 1 エントリにまとめる (つかんだ瞬間に撮り、離したときに確定)
            if (ImGui::IsItemActivated() && !undo.IsRecording()) {
                undo.BeginRecord("Edit Area Cost", selection);
                undo.CaptureBefore(*ctx.scene, tg.fid);
            }
            if (changed) {
                if (auto* editable = world.GetComponent<NavMeshSurfaceComponent>(tg.e)) {
                    editable->areaCosts[i] = (std::max)(1.0f, cost);
                }
            }
            if (ImGui::IsItemDeactivated() && undo.IsRecording()) {
                undo.CaptureAfter(*ctx.scene, tg.fid);
                undo.EndRecord(selection);
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    const NavBakeJobState state = navBakeService_.GetState(tg.fid);
    if (state == NavBakeJobState::Baking) {
        int done = 0;
        int total = 0;
        navBakeService_.GetProgress(tg.fid, done, total);
        ImGui::TextDisabled(Tr(StrId::Insp_NavBaking), done, total);
        ImGui::ProgressBar(total > 0 ? static_cast<float>(done) / static_cast<float>(total) : 0.0f,
                           ImVec2(-1.0f, 0.0f));
        if (ImGui::Button(Tr(StrId::Insp_NavCancel))) {
            navBakeService_.Cancel(tg.fid);
        }
        return;
    }

    if (comp->navAsset.IsNull()) {
        ImGui::TextUnformatted(Tr(StrId::Insp_NavStateNone));
    } else {
        const NavAssetSummary& summary = GetNavAssetSummary(comp->navAsset.value);
        if (summary.loadable) {
            ImGui::Text(Tr(StrId::Insp_NavStateReady), summary.tiles, summary.layers, summary.polygons,
                        summary.kilobytes);
            // 焼いた寸法 (.mnav の設定) と今の寸法の食い違い。Bake で写す予定の型の寸法とも比べる
            const float wantRadius = agentType != nullptr ? agentType->radius : comp->agentRadius;
            const float wantHeight = agentType != nullptr ? agentType->height : comp->agentHeight;
            const float wantClimb = agentType != nullptr ? agentType->maxClimb : comp->maxClimb;
            const float wantSlope = agentType != nullptr ? agentType->maxSlopeDeg : comp->maxSlopeDeg;
            const float wantDrop = agentType != nullptr ? agentType->dropHeight : comp->dropHeight;
            const float wantJump = agentType != nullptr ? agentType->jumpDistance : comp->jumpDistance;
            if (summary.generateLinks) {
                ImGui::Text(Tr(StrId::Insp_NavGeneratedLinks), summary.generatedLinks);
            }
            // 生成の値は生成を入れた資産でだけ比べる (切った資産は値を焼いていない)
            const bool linksChanged = summary.generateLinks != comp->generateLinks
                || (comp->generateLinks && (summary.dropHeight != wantDrop || summary.jumpDistance != wantJump));
            if (summary.agentRadius != wantRadius || summary.agentHeight != wantHeight
                || summary.maxClimb != wantClimb || summary.maxSlopeDeg != wantSlope || linksChanged) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(themeColor::Warning, "%s", Tr(StrId::Insp_NavNeedsRebake));
                ImGui::PopTextWrapPos();
            }
        } else {
            ImGui::TextColored(themeColor::Error, "%s", Tr(StrId::Insp_NavStateBroken));
        }
    }
    const auto outIt = navBakeOutcomes_.find(tg.fid);
    if (outIt != navBakeOutcomes_.end()) {
        const NavBakeOutcome& o = outIt->second;
        switch (o.status) {
        case NavBakeStatus::Ok:
            ImGui::TextDisabled(Tr(StrId::Insp_NavLastBake), o.triangleCount, o.elapsedMs);
            break;
        case NavBakeStatus::Cancelled:
            ImGui::TextDisabled("%s", Tr(StrId::Insp_NavCancelled));
            break;
        case NavBakeStatus::Empty:
        case NavBakeStatus::Failed:
            ImGui::TextColored(themeColor::Error, Tr(StrId::Insp_NavFailed), o.message.c_str());
            break;
        }
    }

    // Play 中の Bake は .mnav 自体は書けるが、Stop でシーンがスナップショットへ巻き戻り参照が消える
    const char* disableReason = inPlayMode_ ? Tr(StrId::Insp_NavPlayModeDisabled) : nullptr;
    ImGui::BeginDisabled(disableReason != nullptr);
    if (ImGui::Button(Tr(StrId::Insp_NavBake))) {
        // 型の寸法を写してから焼く (M84a)。写しは 1 つの Undo
        if (agentType != nullptr && !NavSurfaceMatchesAgentType(*comp, *agentType) && !undo.IsRecording()) {
            const NavAgentType type = *agentType;
            undo.Record("Apply Agent Type", *ctx.scene, selection, tg.fid, UndoStack::StructuralChanges::None, [&] {
                if (auto* editable = world.GetComponent<NavMeshSurfaceComponent>(tg.e)) {
                    NavApplyAgentType(*editable, type);
                }
            });
        }
        NavBakeInputs inputs;
        if (NavPrepareBakeInputs(world, tg.e, inputs)) {
            navBakeService_.Request(tg.fid, std::move(inputs));
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(comp->navAsset.IsNull());
    if (ImGui::Button(Tr(StrId::Insp_NavClear))) {
        ClearNavBake(ctx, selection, undo, tg.e, tg.fid);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (disableReason != nullptr) {
        ImGui::TextDisabled("%s", disableReason);
    }
}

} // namespace mye
