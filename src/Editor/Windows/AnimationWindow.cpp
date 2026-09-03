#include "Editor/Windows/AnimationWindow.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>

#include "Editor/DiskCompare.h"
#include "Editor/Undo/UndoStack.h"
#include "Engine/Core/ComponentRegistry.h"
#include "Engine/Core/Localization.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Platform/PathUtil.h"

#include "imgui.h"

using namespace DirectX;

namespace mye {
namespace {

std::string Sanitize(const char* name)
{
    std::string safe;
    for (const char* p = name; p && *p; ++p) {
        const bool ok = std::isalnum(static_cast<unsigned char>(*p)) || *p == '_' || *p == '-';
        safe += ok ? *p : '_';
    }
    return safe.empty() ? "Clip" : safe;
}

void DfsSubtree(World& w, EntityID root, std::vector<EntityID>& out)
{
    std::function<void(EntityID)> visit = [&](EntityID e) {
        out.push_back(e);
        auto* h = w.GetComponent<HierarchyComponent>(e);
        EntityID c = h ? h->firstChild : kNullEntity;
        while (!c.IsNull()) {
            auto* ch = w.GetComponent<HierarchyComponent>(c);
            const EntityID next = ch ? ch->nextSibling : kNullEntity;
            visit(c);
            c = next;
        }
    };
    visit(root);
}

AnimTrack* FindOrCreateTrack(AnimationClipAsset& clip, uint64_t target, const char* comp,
                             const char* field)
{
    for (AnimTrack& t : clip.tracks) {
        if (t.target == target && t.component == comp && t.field == field) {
            return &t;
        }
    }
    AnimTrack t;
    t.target = target;
    t.component = comp;
    t.field = field;
    t.interp = AnimInterp::Linear;
    const ComponentRegistry& reg = ComponentRegistry::Get();
    t.comp = reg.FindByName(comp);
    if (t.comp != kInvalidComponentType) {
        for (const FieldDesc& f : reg.Desc(t.comp).fields) {
            if (field == std::string(f.name)) {
                t.offset = f.offset;
                t.type = f.type;
                t.compCount = FieldFloatCount(f.type);
                break;
            }
        }
    }
    if (t.compCount == 0) {
        t.compCount = 1;
    }
    clip.tracks.push_back(std::move(t));
    return &clip.tracks.back();
}

void UpsertKey(AnimTrack& tr, int32_t tick, const float* v)
{
    for (AnimKey& k : tr.keys) {
        if (k.tick == tick) {
            for (uint32_t i = 0; i < tr.compCount; ++i) {
                k.value[i] = v[i];
            }
            return;
        }
    }
    AnimKey k;
    k.tick = tick;
    for (uint32_t i = 0; i < tr.compCount; ++i) {
        k.value[i] = v[i];
    }
    tr.keys.push_back(k);
    std::sort(tr.keys.begin(), tr.keys.end(),
              [](const AnimKey& a, const AnimKey& b) { return a.tick < b.tick; });
}

// LocalTransform の 1 チャンネルを現在値で現在 tick にキー
void KeyChannel(AnimationClipAsset& clip, World& w, EntityID e, const char* field, int32_t tick)
{
    auto* lt = w.GetComponent<LocalTransform>(e);
    if (!lt) {
        return;
    }
    float v[4] = { 0, 0, 0, 0 };
    if (std::strcmp(field, "position") == 0) {
        v[0] = lt->position.x;
        v[1] = lt->position.y;
        v[2] = lt->position.z;
    } else if (std::strcmp(field, "scale") == 0) {
        v[0] = lt->scale.x;
        v[1] = lt->scale.y;
        v[2] = lt->scale.z;
    } else { // rotation (quat)
        v[0] = lt->rotation.x;
        v[1] = lt->rotation.y;
        v[2] = lt->rotation.z;
        v[3] = lt->rotation.w;
    }
    UpsertKey(*FindOrCreateTrack(clip, 0, "LocalTransform", field), tick, v);
}

} // namespace

void AnimationWindow::StartPreview(EngineContext& ctx, EntityID animator)
{
    World& w = ctx.scene->GetWorld();
    snapshot_.clear();
    std::vector<EntityID> sub;
    DfsSubtree(w, animator, sub);
    for (EntityID e : sub) {
        if (auto* lt = w.GetComponent<LocalTransform>(e)) {
            snapshot_.emplace_back(e, *lt);
        }
    }
    activeFid_ = ctx.scene->EnsureFileId(animator);
    preview_ = true;
}

void AnimationWindow::StopPreview(EngineContext& ctx)
{
    World& w = ctx.scene->GetWorld();
    for (auto& [e, lt] : snapshot_) {
        if (auto* cur = w.GetComponent<LocalTransform>(e)) {
            *cur = lt;
        }
    }
    snapshot_.clear();
    preview_ = false;
    playing_ = false;
}

void AnimationWindow::HandleRecord(EngineContext& ctx, AnimationClipAsset& clip, EntityID animator)
{
    World& w = ctx.scene->GetWorld();
    auto* lt = w.GetComponent<LocalTransform>(animator);
    if (!lt) {
        return;
    }
    if (!recValid_) {
        recPos_ = lt->position;
        recRot_ = lt->rotation;
        recScl_ = lt->scale;
        recValid_ = true;
        return;
    }
    auto d3 = [](const XMFLOAT3& a, const XMFLOAT3& b) {
        return a.x != b.x || a.y != b.y || a.z != b.z;
    };
    auto d4 = [](const XMFLOAT4& a, const XMFLOAT4& b) {
        return a.x != b.x || a.y != b.y || a.z != b.z || a.w != b.w;
    };
    if (d3(lt->position, recPos_)) {
        KeyChannel(clip, w, animator, "position", previewTick_);
    }
    if (d4(lt->rotation, recRot_)) {
        KeyChannel(clip, w, animator, "rotation", previewTick_);
    }
    if (d3(lt->scale, recScl_)) {
        KeyChannel(clip, w, animator, "scale", previewTick_);
    }
    recPos_ = lt->position;
    recRot_ = lt->rotation;
    recScl_ = lt->scale;
}

void AnimationWindow::OnImGui(EngineContext& ctx, Selection& selection, UndoStack& undo)
{
    if (!open) {
        return;
    }
    if (!ImGui::Begin(Tr(StrId::Win_Animation), &open)) {
        ImGui::End();
        return;
    }
    World& world = ctx.scene->GetWorld();
    GameObject go = ctx.scene->FindByFileId(selection.primary);

    // プレビュー中に選択が変わったら元ポーズへ復元
    if (preview_ && (!go || ctx.scene->EnsureFileId(go.Id()) != activeFid_)) {
        StopPreview(ctx);
    }
    if (!go) {
        ImGui::TextDisabled("%s", Tr(StrId::Clip_SelectEntity));
        ImGui::End();
        return;
    }
    const EntityID e = go.Id();
    auto* anim = world.GetComponent<AnimatorComponent>(e);

    // ---- Animator が無ければ作成 ----
    if (!anim) {
        ImGui::TextDisabled(Tr(StrId::Clip_NoAnimator), world.GetName(e));
        if (ImGui::Button(Tr(StrId::Clip_CreateClip))) {
            const std::string safe = Sanitize(world.GetName(e));
            const std::wstring path =
                ctx.assetsRoot + L"\\animations\\" + Utf8ToWide(safe) + L".anim.json";
            AnimationClipAsset clip;
            clip.name = safe;
            clip.lengthTicks = 60;
            const uint64_t hash = ctx.anims->Register(path, clip);
            ctx.anims->SaveToFile(hash);
            undo.BeginRecord("Create Animator", selection);
            const uint64_t fid = ctx.scene->EnsureFileId(e);
            undo.CaptureBefore(*ctx.scene, fid);
            auto* an = static_cast<AnimatorComponent*>(
                world.AddComponentRaw(e, AnimatorComponent::sTypeId));
            if (an) {
                an->clip = AssetID{ hash };
            }
            world.ApplyStructuralChanges();
            undo.CaptureAfter(*ctx.scene, fid);
            undo.EndRecord(selection);
        }
        ImGui::End();
        return;
    }

    AnimationClipAsset* clip = ctx.anims->GetMutable(anim->clip.value);
    if (!clip) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "Clip missing (hash %llu)",
                           static_cast<unsigned long long>(anim->clip.value));
        ImGui::End();
        return;
    }

    // 未保存判定のために「この窓が触ったクリップ」を控える (M66d)
    MarkTouched(ctx.anims, clip->hash);

    ImGui::Text(Tr(StrId::Clip_Name), clip->name.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(Tr(StrId::Common_Save))) {
        if (ctx.anims->SaveToFile(clip->hash)) {
            MYE_LOG_INFO("anim saved: %s", clip->name.c_str());
        }
    }
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt(Tr(StrId::Clip_Length), &clip->lengthTicks);
    if (clip->lengthTicks < 1) {
        clip->lengthTicks = 1;
    }

    // ---- プレビュー / レコード コントロール ----
    bool prev = preview_;
    if (ImGui::Checkbox(Tr(StrId::Clip_Preview), &prev)) {
        if (prev) {
            recording_ = false;
            StartPreview(ctx, e);
        } else {
            StopPreview(ctx);
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!preview_);
    ImGui::Checkbox(Tr(StrId::Clip_Play), &playing_);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Checkbox(Tr(StrId::Clip_Record), &recording_)) {
        if (recording_) {
            if (preview_) {
                StopPreview(ctx);
            }
            recValid_ = false;
            activeFid_ = ctx.scene->EnsureFileId(e);
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", Tr(StrId::Clip_TipPreview));
    }

    // ---- タイムライン スクラブ ----
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderInt("##time", &previewTick_, 0, clip->lengthTicks, "tick %d");
    previewTick_ = std::clamp(previewTick_, 0, clip->lengthTicks);

    if (preview_ && playing_) {
        previewTick_ += 1;
        if (previewTick_ > clip->lengthTicks) {
            previewTick_ = 0;
        }
    }
    if (preview_) {
        sampler_.Evaluate(world, *ctx.anims, e, previewTick_);
    }
    if (recording_) {
        HandleRecord(ctx, *clip, e);
    }

    // ---- キー追加 (現在 tick、対象 = このエンティティ) ----
    ImGui::Separator();
    ImGui::BeginDisabled(preview_); // プレビュー中はポーズを表示中なのでキー不可
    ImGui::TextUnformatted(Tr(StrId::Clip_KeyAtTick));
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Clip_Position))) {
        KeyChannel(*clip, world, e, "position", previewTick_);
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Clip_Rotation))) {
        KeyChannel(*clip, world, e, "rotation", previewTick_);
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Clip_Scale))) {
        KeyChannel(*clip, world, e, "scale", previewTick_);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Clip_DeleteKeys))) {
        for (AnimTrack& tr : clip->tracks) {
            tr.keys.erase(std::remove_if(tr.keys.begin(), tr.keys.end(),
                                         [&](const AnimKey& k) { return k.tick == previewTick_; }),
                          tr.keys.end());
        }
    }

    // ---- ドープシート (トラック行 + キーマーカー) ----
    ImGui::Separator();
    ImGui::BeginChild("##dope", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rowH = ImGui::GetTextLineHeightWithSpacing();
    const int len = clip->lengthTicks > 0 ? clip->lengthTicks : 1;
    int eraseTrack = -1;
    for (int ti = 0; ti < static_cast<int>(clip->tracks.size()); ++ti) {
        AnimTrack& tr = clip->tracks[ti];
        ImGui::PushID(ti);
        ImGui::Text("%s.%s", tr.component.c_str(), tr.field.c_str());
        ImGui::SameLine(180.0f);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float barW = ImGui::GetContentRegionAvail().x - 30.0f;
        ImGui::InvisibleButton("bar", ImVec2(barW > 10 ? barW : 10, rowH));
        const float midY = p0.y + rowH * 0.5f;
        dl->AddLine(ImVec2(p0.x, midY), ImVec2(p0.x + barW, midY), IM_COL32(100, 100, 100, 255));
        for (const AnimKey& k : tr.keys) {
            const float fx = p0.x + barW * (static_cast<float>(k.tick) / static_cast<float>(len));
            // キーフレーム点。配色ルール (ImGuiTheme.h) の帯内の金 — 原色の黄は使わない
        dl->AddCircleFilled(ImVec2(fx, midY), 4.0f, IM_COL32(212, 181, 110, 255));
        }
        const float cx = p0.x + barW * (static_cast<float>(previewTick_) / static_cast<float>(len));
        dl->AddLine(ImVec2(cx, p0.y), ImVec2(cx, p0.y + rowH), IM_COL32(255, 255, 255, 200), 1.5f);
        if (ImGui::IsItemClicked()) {
            const float mx = ImGui::GetIO().MousePos.x;
            const float f = (mx - p0.x) / (barW > 1 ? barW : 1);
            previewTick_ = std::clamp(static_cast<int>(std::lround(f * len)), 0, clip->lengthTicks);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            eraseTrack = ti;
        }
        ImGui::PopID();
    }
    if (eraseTrack >= 0) {
        clip->tracks.erase(clip->tracks.begin() + eraseTrack);
    }
    ImGui::EndChild();

    ImGui::End();
}

void AnimationWindow::MarkTouched(AnimationLibrary* anims, uint64_t clipHash)
{
    anims_ = anims;
    if (clipHash == 0
        || std::find(touched_.begin(), touched_.end(), clipHash) != touched_.end()) {
        return;
    }
    touched_.push_back(clipHash);
}

bool AnimationWindow::HasUnsavedChanges() const
{
    if (anims_ == nullptr) {
        return false;
    }
    for (const uint64_t hash : touched_) {
        const AnimationClipAsset* clip = anims_->Get(hash);
        if (clip == nullptr) {
            continue; // ライブラリから消えた (= 保存対象でもない)
        }
        if (TextDiffersFromDisk(clip->path, AnimationLibrary::ToJson(*clip).dump(2))) {
            return true;
        }
    }
    return false;
}

} // namespace mye
