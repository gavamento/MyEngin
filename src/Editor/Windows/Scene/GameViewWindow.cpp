#include "Editor/Windows/Scene/GameViewWindow.h"

#include <algorithm>
#include <cstdio>

#include "Editor/Scene/Selection.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Engine/Rendering/RenderSystem.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/UI/UILayout.h"
#include "Engine/Engine/UI/UILayoutGroup.h" // M75e: LayoutScratch
#include "Engine/Engine/UI/UIRenderer.h"
#include "Engine/Renderer/Pipeline/RenderPath.h"

#include "Engine/Renderer/ImGui/ImGuiTheme.h" // themeColor (意味色)

#include "imgui.h"

namespace mye {

void GameViewWindow::OnRenderViews(EngineContext& ctx)
{
    // 見えていない (閉じた / タブの裏) なら描かない。SceneView と同じ規則
    if (!shownLastFrame_ || desiredW_ <= 0 || desiredH_ <= 0) {
        return;
    }
    rt_.Resize(*ctx.device, desiredW_, desiredH_);
    if (!rt_.IsValid()) {
        return;
    }
    FrameTarget target;
    target.rtv = rt_.RTV();
    target.dsv = rt_.DSV();
    target.width = rt_.Width();
    target.height = rt_.Height();
    target.depthSRV = rt_.DepthSRV();       // M42a
    target.dsvReadOnly = rt_.DSVReadOnly();
    target.viewKey = 3; // GameView
    hasCamera_ = ctx.renderSystem->Render(ctx.scene->GetWorld(), *ctx.device, *ctx.renderPath,
                                          *ctx.shaders, *ctx.resources, target, nullptr,
                                          ctx.particles, ctx.vfx);
    // ワールド追従 UI の射影 (直近 Render のカメラ)。OnImGui のアウトラインも同じ値を読む
    uiWcValid_ = ctx.renderSystem->lastCamValid;
    uiWc_.viewProj = ctx.renderSystem->lastViewProjNoJitter;
    uiWc_.prevWorld = ctx.renderSystem->prevWorld;
    uiWc_.alpha = ctx.renderSystem->interpAlpha;
    // M21: ゲーム内 UI を GameView RT に重ねる。
    // ★M70c: ハイライトはエンジンが確定した対話状態そのもの。編集中 (非 Play) は
    //   tick が回らないので状態は既定 (全部 null) = ハイライト無し
    if (ctx.uiRenderer) {
        ctx.uiRenderer->Render(ctx.scene->GetWorld(), *ctx.device, *ctx.shaders, *ctx.resources,
                               target.rtv, target.width, target.height, &ctx.scene->UI(),
                               uiWcValid_ ? &uiWc_ : nullptr);
    }
}

void GameViewWindow::OnImGui(EngineContext& ctx, const Selection& selection)
{
    gameArea_ = {}; // 画像を描けたフレームだけ下で埋める (早期 return = 見えていない)
    shownLastFrame_ = false;
    if (!open) {
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool visible = ImGui::Begin(Tr(StrId::Win_Game), &open);
    ImGui::PopStyleVar();
    if (!visible) {
        ImGui::End();
        return;
    }
    shownLastFrame_ = true;

    // ---- ツールバー (アスペクト比 / 統計オーバーレイ) ----
    static const char* kAspects[] = { "Free", "16:9", "4:3", "1:1" };
    ImGui::SetNextItemWidth(90);
    ImGui::Combo("##aspect", &aspectMode_, kAspects, IM_ARRAYSIZE(kAspects));
    ImGui::SameLine();
    ImGui::Checkbox(Tr(StrId::GameView_Stats), &showStats_);
    ImGui::Separator();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    // 目標アスペクトに合わせてレターボックス (Free は avail 全体)
    float ratio = 0.0f;
    switch (aspectMode_) {
    case 1: ratio = 16.0f / 9.0f; break;
    case 2: ratio = 4.0f / 3.0f; break;
    case 3: ratio = 1.0f; break;
    default: break;
    }
    ImVec2 imgSize = avail;
    if (ratio > 0.0f && avail.x > 1 && avail.y > 1) {
        if (avail.x / avail.y > ratio) {
            imgSize = ImVec2(avail.y * ratio, avail.y); // 横に余白
        } else {
            imgSize = ImVec2(avail.x, avail.x / ratio); // 縦に余白
        }
    }
    desiredW_ = static_cast<int>(std::max(imgSize.x, 16.0f));
    desiredH_ = static_cast<int>(std::max(imgSize.y, 16.0f));

    if (rt_.IsValid()) {
        // レターボックス: 余白分だけカーソルを中央寄せ
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        const ImVec2 off((avail.x - imgSize.x) * 0.5f, (avail.y - imgSize.y) * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(cursor.x + off.x, cursor.y + off.y));
        const ImVec2 imgPos = ImGui::GetCursorScreenPos();
        // ★画像の上のクリックは見えないボタンが受ける。ImGui::Image は操作を受けないアイテムなので、
        //   そのままだと本体クリックが「ウィンドウのドラッグ移動」になり、浮いた Game ウィンドウが
        //   カーソルロック (毎フレーム中央へ SetCursorPos) と噛み合って毎フレーム同じ向きへ
        //   (中央 − クリック点) ずつ動き、画面外へ飛んでいった。ゲームの入力は Win32 の生入力を
        //   gameArea_ で絞って読むので、ImGui がここでクリックを持っても影響しない
        if (imgSize.x > 0.0f && imgSize.y > 0.0f) {
            ImGui::InvisibleButton("##gameImage", imgSize,
                                   ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight
                                       | ImGuiButtonFlags_MouseButtonMiddle);
            ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(rt_.SRV()), imgPos,
                                                 ImVec2(imgPos.x + imgSize.x, imgPos.y + imgSize.y));
        }
        // ゲームがクリックを受け取る範囲 = この画像 (EngineLoop が範囲外のクリックを捨てる)。
        // ★マルチビューポートは無効 (ImGuiRenderer は Docking だけ) なので、ImGui のスクリーン座標が
        //   そのままメインウィンドウのクライアント px = InputSnapshot.mouseX/Y と同じ系
        gameArea_.x = static_cast<int32_t>(imgPos.x);
        gameArea_.y = static_cast<int32_t>(imgPos.y);
        gameArea_.w = static_cast<int32_t>(imgSize.x);
        gameArea_.h = static_cast<int32_t>(imgSize.y);
        // ★画像の外で押したボタンを押し続けている間は「見えていない」(w = h = 0) と報告する。
        //   再生中に Scene タブから Game タブへ切り替えると、タブを押した同じフレームで Game が
        //   見えるようになりカーソルロックが掛かる。ロックは毎フレームカーソルを画像の中央へ
        //   SetCursorPos するので、ImGui には「タブを掴んで右下へ引いた」と見えて Game が
        //   ドックから外れ、外れた窓がマウスを追い、中央へ戻されるたびにまた動いて画面外へ飛んだ。
        //   押した位置はフレーム頭で確定する MouseClickedPos で見る (ActiveId は処理順で後から立つ
        //   ことがある)。0 の範囲は EngineLoop でロック保留とクリック全捨てになるが、押したのは
        //   画像の外なのでゲームが受け取るべきクリックは無い。離した次のフレームからロックが掛かる
        const ImGuiIO& mouseIo = ImGui::GetIO();
        for (int button = 0; button < ImGuiMouseButton_COUNT; ++button) {
            const ImVec2 pressedAt = mouseIo.MouseClickedPos[button];
            const bool pressedInside = pressedAt.x >= imgPos.x && pressedAt.y >= imgPos.y
                                       && pressedAt.x < imgPos.x + imgSize.x && pressedAt.y < imgPos.y + imgSize.y;
            if (mouseIo.MouseDown[button] && !pressedInside) {
                gameArea_.w = 0;
                gameArea_.h = 0;
                break;
            }
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (showStats_) {
            const ImGuiIO& io = ImGui::GetIO();
            char buf[128];
            snprintf(buf, sizeof(buf), "%.0f fps  %.2f ms\n%u entities  tick %llu", io.Framerate,
                     1000.0f / io.Framerate, ctx.scene->GetWorld().AliveCount(),
                     static_cast<unsigned long long>(ctx.tickIndex));
            dl->AddText(ImVec2(imgPos.x + 8, imgPos.y + 6),
                        ImGui::ColorConvertFloat4ToU32(themeColor::Success), buf);
        }
        if (!hasCamera_) {
            dl->AddText(ImVec2(imgPos.x + imgSize.x * 0.5f - 60.0f, imgPos.y + imgSize.y * 0.5f),
                        ImGui::ColorConvertFloat4ToU32(themeColor::Warning),
                        "No CameraComponent in scene");
        }

        // ---- 選択 UI 要素の解決済み矩形アウトライン (M51f) ----
        // UIRenderer と同じ uilayout::ResolveRect を**同じキャンバス**で解く = 描画とズレない
        // (M70b: RT 実寸ではなく RT から求めたキャンバスで解き、結果に canvas.scale を掛ける)。
        // キャンバス px → 表示 px はさらにスケール変換 (RT はリサイズが 1 フレーム遅れるので、
        // 窓リサイズ中だけ僅かに伸縮するが表示専用なので許容)
        {
            World& world = ctx.scene->GetWorld();
            const uilayout::CanvasInfo canvas = uilayout::CanvasSize(rt_.Width(), rt_.Height());
            const float sx = imgSize.x / static_cast<float>(rt_.Width()) * canvas.scale;
            const float sy = imgSize.y / static_cast<float>(rt_.Height()) * canvas.scale;
            uilayout::LayoutScratch layoutScratch; // M75e: 選択ぶんの解決で自動レイアウトのメモを共有
            for (uint64_t fid : selection.ids) {
                GameObject go = ctx.scene->FindByFileId(fid);
                // M75e: RectTransform だけの器 (Layout Group 等) にも枠を出す — 背景を持たないので
                // Game ビューで大きさを確かめる手段が他に無い
                if (!go
                    || (world.GetComponent<UIElementComponent>(go.Id()) == nullptr
                        && world.GetComponent<RectTransformComponent>(go.Id()) == nullptr)) {
                    continue;
                }
                const uilayout::UIRect r =
                    uilayout::ResolveRect(world, go.Id(), canvas.w, canvas.h,
                                          uiWcValid_ ? &uiWc_ : nullptr, &layoutScratch);
                if (r.w <= 0.0f || r.h <= 0.0f) {
                    continue;
                }
                // M75c: r は要素の属するキャンバスの単位。既定キャンバス単位へ直してから表示 px へ
                const float k = uilayout::CanvasOf(world, go.Id(), canvas.w, canvas.h).scale;
                const ImVec2 p0(imgPos.x + r.x * k * sx, imgPos.y + r.y * k * sy);
                const ImVec2 p1(imgPos.x + (r.x + r.w) * k * sx, imgPos.y + (r.y + r.h) * k * sy);
                dl->AddRect(p0, p1, IM_COL32(255, 160, 40, 255), 0.0f,
                            (fid == selection.primary) ? 2.0f : 1.0f);
            }
        }
    }
    ImGui::End();
}

} // namespace mye
