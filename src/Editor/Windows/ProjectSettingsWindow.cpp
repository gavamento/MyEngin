#include "Editor/Windows/ProjectSettingsWindow.h"

#include <algorithm>
#include <string>

#include "Editor/DiskCompare.h"
#include "Editor/SourceControl/ScmHint.h" // M66i: 保存直後に status を取り直させる
#include "Editor/EditorSettings.h"
#include "Editor/PartTagNames.h"
#include "Editor/PhysicsLayerNames.h"
#include "Editor/ShortcutHub.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Localization.h"
#include "Engine/Engine/Particles/ParticleSystem.h"
#include "Engine/Platform/InputActions.h"
#include "Engine/Renderer/RenderPath.h"

#include "Engine/Renderer/ImGuiTheme.h" // themeColor (意味色)

#include "imgui.h"

namespace mye {

void ProjectSettingsWindow::OnImGui(EngineContext& ctx, EditorSettings& settings,
                                    ShortcutHub& shortcuts)
{
    if (!open) {
        captureKind_ = 0;     // 窓を閉じたら捕捉も取り消す
        particleSaved_ = false; // 保存の確認表示も持ち越さない
        return;
    }
    // 未保存判定に使う参照を控える (M66d)
    inputActions_ = ctx.inputActions;
    assetsRoot_ = ctx.assetsRoot;
    // M51d: キー捕捉はセクションの開閉と無関係に毎フレーム処理する
    // (折り畳み中に lastInput_ が止まると、再展開時に偽の押下エッジを拾う)
    UpdateKeyCapture(ctx);
    if (!ImGui::Begin(Tr(StrId::Win_ProjectSettings), &open)) {
        ImGui::End();
        return;
    }

    // ---- レンダリング ----
    if (ImGui::CollapsingHeader(Tr(StrId::PrjSet_Rendering), ImGuiTreeNodeFlags_DefaultOpen)) {
        const bool isForward = (ctx.renderPath == ctx.renderPathForward);
        ImGui::Text(Tr(StrId::PrjSet_ActivePath), ctx.renderPath ? ctx.renderPath->Name() : "?");
        if (ImGui::RadioButton(Tr(StrId::Menu_Forward), isForward)) {
            ctx.renderPath = ctx.renderPathForward;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton(Tr(StrId::Menu_Deferred), !isForward)) {
            ctx.renderPath = ctx.renderPathDeferred;
        }

        // ---- パーティクルのプロジェクト既定 (M66h、spec §4.2 の決定 8) ----
        // ★`particleBackend` を assets\project_settings.json へ書き戻すのは**ここだけ**。
        //   Particle Settings 窓のラジオはセッション上書き (= `--particle-backend` の
        //   CLI と同じ「書き戻さない」扱い) で、触っただけでは共有ファイルは動かない
        if (ctx.particles != nullptr) {
            ImGui::SeparatorText(Tr(StrId::PrjSet_ParticleBackend));
            int kind = static_cast<int>(ctx.particles->ActiveKind());
            bool changed = false;
            changed |= ImGui::RadioButton(Tr(StrId::Particle_Cpu), &kind, 0);
            ImGui::SameLine();
            changed |= ImGui::RadioButton(Tr(StrId::Particle_Gpu), &kind, 1);
            if (changed) {
                ctx.particles->SetActiveKind(static_cast<ParticleBackendKind>(kind));
                particleSaved_ = false; // 保存済みの表示と食い違う値になった
            }
            if (ImGui::Button(Tr(StrId::PrjSet_SaveParticles))) {
                ctx.particles->SaveSettings();
                particleSaved_ = true;
                scmhint::Changed(ctx.assetsRoot + L"\\project_settings.json"); // M66i
            }
            ImGui::TextDisabled("%s", Tr(StrId::PrjSet_ParticleNote));
            if (particleSaved_) {
                // ★トーストを出す口をこの窓は持たない (ToastCenter は EditorApp)。
                //   押した直後の 1 行だけをここに残す
                ImGui::TextColored(themeColor::Success, "%s", Tr(StrId::PrjSet_ParticleSaved));
            }
        }
    }

    // ---- エディタ設定 (editor_settings.json) ----
    if (ImGui::CollapsingHeader(Tr(StrId::PrjSet_Editor), ImGuiTreeNodeFlags_DefaultOpen)) {
        char cmd[256];
        std::snprintf(cmd, sizeof(cmd), "%s", settings.externalEditorCmd.c_str());
        if (ImGui::InputText(Tr(StrId::PrjSet_ExternalCmd), cmd, sizeof(cmd))) {
            settings.externalEditorCmd = cmd;
        }
        ImGui::TextDisabled("%s", Tr(StrId::PrjSet_CmdHint));
        ImGui::DragFloat(Tr(StrId::PrjSet_SnapTranslate), &settings.snapTranslate, 0.01f, 0.0f, 100.0f);
        ImGui::DragFloat(Tr(StrId::PrjSet_SnapRotate), &settings.snapRotateDeg, 0.5f, 0.0f, 180.0f);
        ImGui::DragFloat(Tr(StrId::PrjSet_SnapScale), &settings.snapScale, 0.01f, 0.0f, 10.0f);
        ImGui::Checkbox(Tr(StrId::PrjSet_GridVisible), &settings.gridVisible);
        if (ImGui::Button(Tr(StrId::PrjSet_SaveSettings))) {
            settings.Save();
        }
    }

    // ---- 物理レイヤー名 (M36a、assets\project_settings.json の physicsLayers) ----
    if (ImGui::CollapsingHeader(Tr(StrId::PrjSet_PhysicsLayers))) {
        PhysicsLayerNames& ln = PhysicsLayerNames::Get();
        ln.Load(ctx.assetsRoot);
        ImGui::TextDisabled("%s", Tr(StrId::PrjSet_LayerHint));
        for (int i = 0; i < PhysicsLayerNames::kCount; ++i) {
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(160.0f);
            char label[16];
            std::snprintf(label, sizeof(label), "%2d", i);
            ImGui::InputText(label, ln.EditBuffer(i), 32);
            ImGui::PopID();
            if ((i % 2) == 0) {
                ImGui::SameLine(240.0f);
            }
        }
        if (ImGui::Button(Tr(StrId::PrjSet_SaveLayers))) {
            if (ln.Save(ctx.assetsRoot)) {
                ln.Load(ctx.assetsRoot, true);
                scmhint::Changed(ctx.assetsRoot + L"\\project_settings.json"); // M66i
            }
        }
    }

    // ---- 部位タグ名 (M48f、assets\project_settings.json の partTags) ----
    // 物理レイヤーと違い **名前のハッシュが ID の実体** なので、行数可変 + 注意書きつき
    if (ImGui::CollapsingHeader(Tr(StrId::PrjSet_PartTags))) {
        PartTagNames& pt = PartTagNames::Get();
        pt.Load(ctx.assetsRoot);
        ImGui::TextWrapped("%s", Tr(StrId::PrjSet_PartTagHint));
        for (int i = 0; i < pt.Count(); ++i) {
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(200.0f);
            char label[16];
            std::snprintf(label, sizeof(label), "%2d", i);
            ImGui::InputText(label, pt.EditBuffer(i), PartTagNames::kNameCapacity);
            ImGui::SameLine();
            ImGui::TextDisabled("0x%016llx", static_cast<unsigned long long>(pt.Id(i)));
            ImGui::PopID();
        }
        if (ImGui::Button(Tr(StrId::PrjSet_AddPartTag))) {
            pt.SetCount(pt.Count() + 1);
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr(StrId::PrjSet_SavePartTags))) {
            if (pt.Save(ctx.assetsRoot)) {
                pt.Load(ctx.assetsRoot, true); // 空欄が落ちた結果を読み直す
                scmhint::Changed(ctx.assetsRoot + L"\\project_settings.json"); // M66i
            }
        }
    }

    // ---- 入力アクション (M51d、assets\input\actions.json) ----
    if (ImGui::CollapsingHeader(Tr(StrId::PrjSet_Input))) {
        DrawInputSection(ctx);
    }

    // ---- ショートカット一覧 (読み取り専用) ----
    if (ImGui::CollapsingHeader(Tr(StrId::PrjSet_Shortcuts))) {
        static const char* kNames[] = {
            "Save", "Undo", "Redo", "Duplicate", "Delete",
            "Focus", "Rename", "Copy", "Cut", "Paste",
        };
        if (ImGui::BeginTable("##sc", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn(Tr(StrId::PrjSet_ColAction));
            ImGui::TableSetupColumn(Tr(StrId::PrjSet_ColKey));
            ImGui::TableHeadersRow();
            for (int i = 0; i < static_cast<int>(Shortcut::Count); ++i) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(kNames[i]);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(shortcuts.Label(static_cast<Shortcut>(i)));
            }
            ImGui::EndTable();
        }
    }

    ImGui::End();
}

void ProjectSettingsWindow::UpdateKeyCapture(EngineContext& ctx)
{
    if (captureKind_ != 0 && ctx.inputActions != nullptr) {
        // 0x00-0x07 はマウスボタン等 — 捕捉ボタンのクリック自体を拾わないため除外
        for (int vk = 0x08; vk < 256; ++vk) {
            const uint8_t v = static_cast<uint8_t>(vk);
            if (!ctx.Input().KeyDown(v) || lastInput_.KeyDown(v)) {
                continue; // 新規押下エッジのみ
            }
            InputActions& ia = *ctx.inputActions;
            if (v == 0x1B) { // Esc = 取消
            } else if (captureKind_ == 1
                       && captureIndex_ < static_cast<int>(ia.Actions().size())) {
                std::vector<uint8_t>& keys = ia.Actions()[captureIndex_].keys;
                if (std::find(keys.begin(), keys.end(), v) == keys.end()) {
                    keys.push_back(v);
                }
            } else if (captureKind_ == 2 && captureIndex_ < static_cast<int>(ia.Axes().size())) {
                ia.Axes()[captureIndex_].posKey = v;
            } else if (captureKind_ == 3 && captureIndex_ < static_cast<int>(ia.Axes().size())) {
                ia.Axes()[captureIndex_].negKey = v;
            }
            captureKind_ = 0;
            break;
        }
    }
    lastInput_ = ctx.Input(); // キー割り当ての取得はレーン 0 (キーボード) 固定
}

void ProjectSettingsWindow::DrawInputSection(EngineContext& ctx)
{
    if (ctx.inputActions == nullptr) {
        return;
    }
    InputActions& ia = *ctx.inputActions;
    ImGui::TextWrapped("%s", Tr(StrId::PrjSet_InputHint));
    ImGui::TextDisabled("%s", Tr(StrId::PrjSet_LiveLegend));
    if (captureKind_ != 0) {
        ImGui::TextColored(themeColor::Warning, "%s", Tr(StrId::PrjSet_CaptureWait));
    }
    const ImVec4 colOn(0.3f, 1.0f, 0.3f, 1.0f);
    const ImVec4 colOff(0.4f, 0.4f, 0.4f, 1.0f);

    // ---- アクション一覧 ----
    ImGui::SeparatorText(Tr(StrId::PrjSet_Actions));
    int removeAction = -1;
    for (int i = 0; i < static_cast<int>(ia.Actions().size()); ++i) {
        InputActionDef& a = ia.Actions()[i];
        ImGui::PushID(i);
        char name[64];
        std::snprintf(name, sizeof(name), "%s", a.name.c_str());
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::InputText("##name", name, sizeof(name))) {
            a.name = name;
            a.nameHash = HashStr(a.name); // 鍵は名前のハッシュ — リネームに追随させる
        }
        // ライブ状態 (直近 tick の評価結果)
        const uint32_t st = ia.ActionStateAt(static_cast<size_t>(i));
        ImGui::SameLine();
        ImGui::TextColored((st & kActionHeld) != 0 ? colOn : colOff, "H");
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::TextColored((st & kActionPressed) != 0 ? colOn : colOff, "P");
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::TextColored((st & kActionReleased) != 0 ? colOn : colOff, "R");
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            removeAction = i;
        }
        // 束縛チップ (クリックで削除) + 追加ボタン
        ImGui::Indent();
        int removeKey = -1;
        for (int k = 0; k < static_cast<int>(a.keys.size()); ++k) {
            ImGui::PushID(k);
            if (ImGui::SmallButton(InputActions::VkNameStr(a.keys[k]).c_str())) {
                removeKey = k;
            }
            ImGui::PopID();
            ImGui::SameLine();
        }
        if (removeKey >= 0) {
            a.keys.erase(a.keys.begin() + removeKey);
        }
        if (ImGui::SmallButton(Tr(StrId::PrjSet_AddKey))) {
            captureKind_ = 1;
            captureIndex_ = i;
        }
        ImGui::SameLine();
        for (int b = 0; b < 16; ++b) {
            const uint16_t mask = static_cast<uint16_t>(1u << b);
            if ((a.padMask & mask) == 0) {
                continue;
            }
            const char* pn = InputActions::PadButtonName(mask);
            ImGui::PushID(100 + b);
            if (pn != nullptr && ImGui::SmallButton(pn)) {
                a.padMask &= static_cast<uint16_t>(~mask);
            }
            ImGui::PopID();
            ImGui::SameLine();
        }
        if (ImGui::SmallButton(Tr(StrId::PrjSet_AddPad))) {
            ImGui::OpenPopup("padadd");
        }
        if (ImGui::BeginPopup("padadd")) {
            // 表示順は UX 優先 (ボタン → ショルダー → スティック押込 → システム → 十字)
            static constexpr uint16_t kOrder[] = { 0x1000, 0x2000, 0x4000, 0x8000, 0x0100,
                                                   0x0200, 0x0040, 0x0080, 0x0010, 0x0020,
                                                   0x0001, 0x0002, 0x0004, 0x0008 };
            for (uint16_t mask : kOrder) {
                if ((a.padMask & mask) == 0
                    && ImGui::Selectable(InputActions::PadButtonName(mask))) {
                    a.padMask |= mask;
                }
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        for (int b = 0; b < 5; ++b) {
            const uint8_t mask = static_cast<uint8_t>(1u << b);
            if ((a.mouseMask & mask) == 0) {
                continue;
            }
            ImGui::PushID(200 + b);
            if (ImGui::SmallButton(InputActions::MouseButtonName(b))) {
                a.mouseMask &= static_cast<uint8_t>(~mask);
            }
            ImGui::PopID();
            ImGui::SameLine();
        }
        if (ImGui::SmallButton(Tr(StrId::PrjSet_AddMouse))) {
            ImGui::OpenPopup("mouseadd");
        }
        if (ImGui::BeginPopup("mouseadd")) {
            for (int b = 0; b < 5; ++b) {
                const uint8_t mask = static_cast<uint8_t>(1u << b);
                if ((a.mouseMask & mask) == 0
                    && ImGui::Selectable(InputActions::MouseButtonName(b))) {
                    a.mouseMask |= mask;
                }
            }
            ImGui::EndPopup();
        }
        ImGui::Unindent();
        ImGui::PopID();
    }
    if (removeAction >= 0) {
        ia.Actions().erase(ia.Actions().begin() + removeAction);
    }
    if (ImGui::Button(Tr(StrId::PrjSet_AddAction))) {
        InputActionDef def;
        def.name = "NewAction" + std::to_string(ia.Actions().size());
        def.nameHash = HashStr(def.name);
        ia.Actions().push_back(std::move(def));
    }

    // ---- 軸一覧 ----
    ImGui::SeparatorText(Tr(StrId::PrjSet_Axes));
    int removeAxis = -1;
    for (int i = 0; i < static_cast<int>(ia.Axes().size()); ++i) {
        InputAxisDef& a = ia.Axes()[i];
        ImGui::PushID(1000 + i);
        char name[64];
        std::snprintf(name, sizeof(name), "%s", a.name.c_str());
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::InputText("##name", name, sizeof(name))) {
            a.name = name;
            a.nameHash = HashStr(a.name);
        }
        // ライブ値 [-1, +1]
        const float v = ia.AxisValueAt(static_cast<size_t>(i));
        char overlay[16];
        std::snprintf(overlay, sizeof(overlay), "%+.2f", v);
        ImGui::SameLine();
        ImGui::ProgressBar((v + 1.0f) * 0.5f, ImVec2(90.0f, 0.0f), overlay);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            removeAxis = i;
        }
        ImGui::Indent();
        // posKey / negKey (クリックで捕捉、右クリックで解除)。
        // 同じキー名が両方に付いても ID が割れないよう "###" で固定 ID にする
        char posLabel[32];
        std::snprintf(posLabel, sizeof(posLabel), "%s###pos",
                      a.posKey != 0 ? InputActions::VkNameStr(a.posKey).c_str() : "---");
        ImGui::TextUnformatted("+");
        ImGui::SameLine();
        if (ImGui::SmallButton(posLabel)) {
            captureKind_ = 2;
            captureIndex_ = i;
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            a.posKey = 0;
            captureKind_ = 0;
        }
        char negLabel[32];
        std::snprintf(negLabel, sizeof(negLabel), "%s###neg",
                      a.negKey != 0 ? InputActions::VkNameStr(a.negKey).c_str() : "---");
        ImGui::SameLine();
        ImGui::TextUnformatted("-");
        ImGui::SameLine();
        if (ImGui::SmallButton(negLabel)) {
            captureKind_ = 3;
            captureIndex_ = i;
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            a.negKey = 0;
            captureKind_ = 0;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        if (ImGui::BeginCombo(Tr(StrId::PrjSet_PadAxis),
                              InputActions::PadAxisName(a.padAxis))) {
            for (int ax = 0; ax < static_cast<int>(PadAxis::Count); ++ax) {
                const PadAxis pa = static_cast<PadAxis>(ax);
                if (ImGui::Selectable(InputActions::PadAxisName(pa), pa == a.padAxis)) {
                    a.padAxis = pa;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::DragFloat(Tr(StrId::PrjSet_Deadzone), &a.deadzone, 0.01f, 0.0f, 0.95f, "%.2f");
        ImGui::Unindent();
        ImGui::PopID();
    }
    if (removeAxis >= 0) {
        ia.Axes().erase(ia.Axes().begin() + removeAxis);
    }
    if (ImGui::Button(Tr(StrId::PrjSet_AddAxis))) {
        InputAxisDef def;
        def.name = "NewAxis" + std::to_string(ia.Axes().size());
        def.nameHash = HashStr(def.name);
        ia.Axes().push_back(std::move(def));
    }

    // 保存ホットリロード: 書き出してから正規形 (重複除去・不明名落ち) を読み直す
    if (ImGui::Button(Tr(StrId::PrjSet_SaveInput))) {
        if (ia.Save(ctx.assetsRoot)) {
            ia.Load(ctx.assetsRoot, true);
            scmhint::Changed(ctx.assetsRoot + L"\\input\\actions.json"); // M66i
        }
    }
}

bool InputActionsDifferFromDisk(const std::wstring& assetsRoot, const InputActions& ia)
{
    if (assetsRoot.empty()) {
        return false;
    }
    // ToJsonText() は Save の実体そのもの = 「保存したらこうなる」文字列。
    // ★不在時の相手は**既定構築した InputActions を同じ直列化器へ通したもの** —
    //   Load は不在なら空マップで返る (InputActions::Load) ので、これが
    //   「ディスクを読み直した状態」と一致する
    const InputActions fresh;
    return TextDiffersFromDisk(assetsRoot + L"\\input\\actions.json", ia.ToJsonText(),
                               fresh.ToJsonText());
}

bool ProjectSettingsWindow::HasUnsavedChanges() const
{
    if (PhysicsLayerNames::Get().DiffersFromDisk() || PartTagNames::Get().DiffersFromDisk()) {
        return true;
    }
    if (inputActions_ != nullptr) {
        return InputActionsDifferFromDisk(assetsRoot_, *inputActions_);
    }
    return false;
}

} // namespace mye
