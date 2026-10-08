//====================================================================================
//                          AnimatorControllerEditSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/08/2026
//                                  コントローラ窓のステート編集の回帰テスト実装
//====================================================================================
#include "Editor/SelfTest/AnimatorControllerEditSelfTest.h"

#include <string>

#include "Editor/Windows/Animation/ClipEventEdit.h"
#include "Editor/Windows/Animation/ControllerStateEdit.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Renderer/Mesh/Skeleton.h"

namespace mye {

bool RunAnimatorControllerEditSelfTest()
{
    MYE_LOG_INFO("==== AnimatorController edit (M89g / M89o) self test ====");
    int failCount = 0;
    const auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // (1) 種類はアセットの blendType と skelClip から読む
    {
        ControllerState s;
        check(GetStateSkelKind(s) == StateSkelKind::None, "kind: empty state is None");
        SetStateSkelClip(s, "Walk");
        check(GetStateSkelKind(s) == StateSkelKind::Clip, "kind: skelClip makes Clip");
        check(s.skelClipHash == HashStr("Walk"), "SetStateSkelClip keeps the hash in step with the name");
        SetStateSkelClip(s, "");
        check(s.skelClipHash == 0 && GetStateSkelKind(s) == StateSkelKind::None, "an empty bone clip has hash 0 (None)");
    }

    // (2) なし → クリップ 1 本: 候補の先頭で埋める
    {
        ControllerState s;
        SetStateSkelKind(s, StateSkelKind::Clip, "Idle");
        check(s.skelClip == "Idle" && s.skelClipHash == HashStr("Idle") && StateDrivesSkeleton(s),
              "None -> Clip fills the fallback clip");
        ControllerState noModel;
        SetStateSkelKind(noModel, StateSkelKind::Clip, "");
        check(GetStateSkelKind(noModel) == StateSkelKind::None, "None -> Clip without any candidate stays None");
    }

    // (3) クリップ → 1D → クリップ → なし: 行き来しても編集中の値が残り、なしだけが駆動を止める
    {
        ControllerState s;
        SetStateSkelClip(s, "Walk");
        SetStateSkelKind(s, StateSkelKind::Blend1D, "Idle");
        check(s.blendType == ControllerBlendType::Blend1D && s.blendChildren.size() == 1
                  && s.blendChildren[0].clip == "Walk" && s.blendChildren[0].clipHash == HashStr("Walk"),
              "Clip -> Blend1D seeds one child from the bone clip");
        AddBlendChild(s, "Run");
        check(s.blendChildren.size() == 2 && s.blendChildren[1].threshold == 1.0f && s.blendChildren[1].posX == 1.0f
                  && s.blendChildren[1].clipHash == HashStr("Run"),
              "AddBlendChild places the child next to the last one");
        SetStateSkelKind(s, StateSkelKind::Clip, "Idle");
        check(s.blendType == ControllerBlendType::None && s.skelClip == "Walk" && s.blendChildren.size() == 2,
              "Blend1D -> Clip restores the bone clip and keeps the children");
        SetStateSkelKind(s, StateSkelKind::Blend2D, "Idle");
        check(s.blendType == ControllerBlendType::Blend2D && s.blendChildren.size() == 2,
              "Clip -> Blend2D reuses the existing children");
        SetStateSkelKind(s, StateSkelKind::None, "Idle");
        check(GetStateSkelKind(s) == StateSkelKind::None && !StateDrivesSkeleton(s),
              "-> None stops driving the skeleton");
    }

    // (4) ブレンド → クリップで skelClip が空なら、最初の名前付きの子を使う
    {
        ControllerState s;
        SetStateSkelKind(s, StateSkelKind::Blend1D, "");
        check(s.blendChildren.size() == 1 && s.blendChildren[0].clip.empty() && s.blendChildren[0].clipHash == 0,
              "Blend1D without any candidate seeds one unnamed child");
        AddBlendChild(s, "Run");
        SetStateSkelKind(s, StateSkelKind::Clip, "Idle");
        check(s.skelClip == "Run", "Blend -> Clip takes the first named child before the fallback");
    }

    // (5) 編集結果は .controller.json の往復で保たれる (使わない側は保存されない)
    {
        ControllerAsset asset;
        asset.parameters.push_back({ "speed", ControllerParamType::Float });
        asset.parameters.push_back({ "dir", ControllerParamType::Float });
        ControllerState blend;
        blend.name = "Move";
        SetStateSkelClip(blend, "Idle");
        SetStateSkelKind(blend, StateSkelKind::Blend2D, "");
        AddBlendChild(blend, "Walk");
        blend.blendChildren[1].posY = 2.5f;
        blend.blendParam = 0;
        blend.blendParamY = 1;
        ControllerState none;
        none.name = "Off";
        SetStateSkelClip(none, "Walk");
        SetStateSkelKind(none, StateSkelKind::Blend1D, "");
        SetStateSkelKind(none, StateSkelKind::None, "");
        asset.states = { blend, none };

        ControllerAsset back;
        const bool ok = ControllerLibrary::FromJson(ControllerLibrary::ToJson(asset), back);
        check(ok && back.states.size() == 2, "edited controller round-trips through JSON");
        if (ok && back.states.size() == 2) {
            const ControllerState& b = back.states[0];
            check(b.blendType == ControllerBlendType::Blend2D && b.blendParamY == 1 && b.blendChildren.size() == 2
                      && b.blendChildren[0].clipHash == HashStr("Idle") && b.blendChildren[1].clipHash == HashStr("Walk")
                      && b.blendChildren[1].posX == 1.0f && b.blendChildren[1].posY == 2.5f,
                  "Blend2D children, positions and params survive the round trip");
            check(GetStateSkelKind(back.states[1]) == StateSkelKind::None && back.states[1].blendChildren.empty(),
                  "a None state saves no skeleton drive (the kept children are not written)");
        }
    }

    // (6) 候補は名前付きのクリップだけ (無名は名前で引けない)
    {
        SkinnedModel model;
        model.clips.resize(3);
        model.clips[0].name = "Idle";
        model.clips[2].name = "Walk";
        const std::vector<std::string> names = NamedSkeletalClips(&model);
        check(names.size() == 2 && names[0] == "Idle" && names[1] == "Walk", "NamedSkeletalClips skips unnamed clips");
        check(NamedSkeletalClips(nullptr).empty(), "NamedSkeletalClips(null) is empty");
    }

    // (7) アニメイベントの編集 (M89o、プレビュー窓のタイムライン)
    {
        ControllerAsset asset;
        const int32_t first = AddClipEvent(asset, "Walk", 12);
        const int32_t second = AddClipEvent(asset, "Walk", -3);
        check(first == 0 && second == 1 && asset.clipEvents.size() == 1 && asset.clipEvents[0].clip == "Walk"
                  && asset.clipEvents[0].clipHash == HashStr("Walk"),
              "AddClipEvent creates the clip's list once and returns the new index");
        ControllerClipEvents* ce = FindClipEventsMutable(asset, HashStr("Walk"));
        check(ce != nullptr && ce->events[0].name == "Event" && ce->events[0].nameHash == HashStr("Event")
                  && ce->events[0].tick == 12 && ce->events[0].kind == ClipEventKind::Script && ce->events[1].tick == 0,
              "a new event is a Script named Event at the tick (negative ticks clamp to 0)");
        check(FindClipEventsMutable(asset, HashStr("Run")) == nullptr, "FindClipEventsMutable misses an unknown clip");

        ControllerClipEvent& ev = ce->events[0];
        SetClipEventName(ev, "FootL");
        check(ev.nameHash == HashStr("FootL"), "SetClipEventName keeps the hash in step with the name");
        SetClipEventKind(ev, ClipEventKind::Sound);
        SetClipEventAsset(ev, "step_sound");
        check(ev.kind == ClipEventKind::Sound && ev.assetHash == HashStr("step_sound"),
              "SetClipEventAsset keeps the hash in step with the key");
        SetClipEventKind(ev, ClipEventKind::Sound);
        check(ev.asset == "step_sound", "setting the same kind keeps the asset");
        SetClipEventKind(ev, ClipEventKind::Effect);
        check(ev.asset.empty() && ev.assetHash == HashStr(""), "changing the kind clears the asset (sound key != prefab key)");

        // 保存の往復: 種類・アセット・ジョイントが残る
        SetClipEventKind(ev, ClipEventKind::Sound);
        SetClipEventAsset(ev, "step_sound");
        ev.joint = "Foot.L";
        ev.minWeight = 0.5f;
        ControllerAsset back;
        const bool ok = ControllerLibrary::FromJson(ControllerLibrary::ToJson(asset), back);
        const ControllerClipEvents* backCe = ok && back.clipEvents.size() == 1 ? &back.clipEvents[0] : nullptr;
        check(backCe != nullptr && backCe->events.size() == 2 && backCe->events[0].kind == ClipEventKind::Sound
                  && backCe->events[0].nameHash == HashStr("FootL") && backCe->events[0].assetHash == HashStr("step_sound")
                  && backCe->events[0].joint == "Foot.L" && backCe->events[0].minWeight == 0.5f,
              "edited events round-trip through JSON");

        RemoveClipEvent(asset, HashStr("Walk"), 5);
        RemoveClipEvent(asset, HashStr("Run"), 0);
        check(asset.clipEvents.size() == 1 && asset.clipEvents[0].events.size() == 2,
              "RemoveClipEvent ignores an out-of-range index and an unknown clip");
        RemoveClipEvent(asset, HashStr("Walk"), 0);
        check(asset.clipEvents[0].events.size() == 1 && asset.clipEvents[0].events[0].tick == 0,
              "RemoveClipEvent removes the indexed event");
        RemoveClipEvent(asset, HashStr("Walk"), 0);
        check(asset.clipEvents.empty() && !ControllerLibrary::ToJson(asset).contains("clipEvents"),
              "removing the last event drops the clip's list (no empty array is saved)");

        // タイムライン (ステートの長さ) → クリップの tick: 割合の四捨五入、0..clipTicks
        check(TimelineTickToClipTick(30, 60, 120) == 60 && TimelineTickToClipTick(1, 3, 2) == 1
                  && TimelineTickToClipTick(3, 3, 2) == 2 && TimelineTickToClipTick(70, 60, 60) == 60
                  && TimelineTickToClipTick(-5, 60, 60) == 0 && TimelineTickToClipTick(10, 0, 60) == 0
                  && TimelineTickToClipTick(10, 60, 0) == 0,
              "TimelineTickToClipTick rounds by ratio and clamps to the clip");
    }

    MYE_LOG_INFO("==== AnimatorController edit self test: %s (%d failures) ====", failCount == 0 ? "PASS" : "FAIL",
                 failCount);
    return failCount == 0;
}

} // namespace mye
