//====================================================================================
//                          TerminalWindow.cpp
//  MyEngine/ 秋田蓮音                                                      11/19/2026
//                                          ターミナル窓の実装 (セルの描画・キー/IME/貼付/選択・ツールバー・設定)
//====================================================================================
#include "Editor/Windows/Project/TerminalWindow.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "imgui.h"
#include "imgui_internal.h" // IME の入力位置 (PlatformImeData) だけ。ここに閉じる

#include "Editor/Tools/Terminal/TerminalHost.h"
#include "Editor/Tools/Terminal/TerminalInput.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Renderer/ImGui/ImGuiTheme.h"

namespace mye {
namespace {

constexpr const char* kSettingsPopupId = "##terminal_settings";
constexpr const char* kContextPopupId = "##terminal_context";
// ホイール 1 刻みで戻る行数 (Windows Terminal の既定と同じ)
constexpr int kWheelLinesPerNotch = 3;
// カーソルと選択の重ね塗りの不透明度 (0..255)。字を隠さないよう半透明にする
constexpr int kCursorFillAlpha = 150;
constexpr int kCursorOutlineAlpha = 220;
constexpr int kSelectionAlpha = 90;
constexpr float kSettingsInputWidth = 360.0f;
constexpr ImGuiWindowFlags kWindowFlags =
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
    // 矢印・Tab・Enter・Space を ImGui のナビゲーションに取られると子へ届かない (NavEnableKeyboard は全窓で有効)。
    // この窓の中ではナビゲーションを使わない (ボタンはマウスで押す)
    ImGuiWindowFlags_NoNavInputs;

uint32_t PackRgb(const ImVec4& color)
{
    const auto channel = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return (channel(color.x) << 16) | (channel(color.y) << 8) | channel(color.z);
}

ImU32 ToColor(uint32_t rgb, int alpha = 255)
{
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, alpha);
}

std::string FileNameOf(const std::string& path)
{
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

struct KeyBinding
{
    ImGuiKey imgui;
    TerminalKey terminal;
};

constexpr KeyBinding kKeyBindings[] = {
    { ImGuiKey_Enter, TerminalKey::Enter },
    { ImGuiKey_KeypadEnter, TerminalKey::Enter },
    { ImGuiKey_Backspace, TerminalKey::Backspace },
    { ImGuiKey_Tab, TerminalKey::Tab },
    { ImGuiKey_Escape, TerminalKey::Escape },
    { ImGuiKey_UpArrow, TerminalKey::Up },
    { ImGuiKey_DownArrow, TerminalKey::Down },
    { ImGuiKey_LeftArrow, TerminalKey::Left },
    { ImGuiKey_RightArrow, TerminalKey::Right },
    { ImGuiKey_Home, TerminalKey::Home },
    { ImGuiKey_End, TerminalKey::End },
    { ImGuiKey_PageUp, TerminalKey::PageUp },
    { ImGuiKey_PageDown, TerminalKey::PageDown },
    { ImGuiKey_Insert, TerminalKey::Insert },
    { ImGuiKey_Delete, TerminalKey::Delete },
};

} // namespace

void TerminalWindow::ApplySettings(const TerminalWindowSettings& settings)
{
    settings_ = settings;
    std::snprintf(shellBuf_, sizeof(shellBuf_), "%s", settings.shell.c_str());
    settingsDirty_ = false;
}

bool TerminalWindow::TakeSettingsChanged(TerminalWindowSettings& out)
{
    if (!settingsDirty_) {
        return false;
    }
    settingsDirty_ = false;
    settings_.shell = shellBuf_;
    settings_.fontSize = ClampTerminalFontSize(settings_.fontSize);
    settings_.scrollback = ClampTerminalScrollback(settings_.scrollback);
    out = settings_;
    return true;
}

void TerminalWindow::Pump()
{
    if (!session_) {
        return;
    }
    // 新しい出力が来たら最下部へ戻す (spec 4.1)。出力が無いフレームはホイールで戻った位置を保つ
    if (session_->Pump() > 0) {
        viewOffset_ = 0;
    }
}

void TerminalWindow::Shutdown()
{
    session_.reset();
    selection_ = TerminalSelection{};
    dragging_ = false;
}

void TerminalWindow::StartSession(const TerminalWindowHost& host, const TerminalGridSize& grid)
{
    startAttempted_ = true;
    session_.reset();
    shellError_ = TerminalShellError::None;
    shellName_.clear();
    selection_ = TerminalSelection{};
    dragging_ = false;
    viewOffset_ = 0;

    const TerminalShellCommand command = ResolveTerminalShell(shellBuf_, FindTerminalExecutable);
    if (command.error != TerminalShellError::None) {
        shellError_ = command.error;   // 状態行が理由を出す。再起動で再試行できる
        return;
    }
    TerminalLaunchSpec spec;
    spec.exePath = command.path;
    spec.args = command.args;
    spec.workDir = host.projectRoot;
    session_ = std::make_unique<TerminalSession>(CreateConPtyTerminalHost(), grid.cols, grid.rows,
                                                 ClampTerminalScrollback(settings_.scrollback));
    shellName_ = FileNameOf(command.path);
    session_->Start(spec);   // 失敗 (ConPTY 無し・起動失敗) は State() が持ち、状態行が出す
}

void TerminalWindow::RequestRestart()
{
    session_.reset();
    startAttempted_ = false;
    shellError_ = TerminalShellError::None;
    selection_ = TerminalSelection{};
    dragging_ = false;
    viewOffset_ = 0;
}

void TerminalWindow::OnImGui(const TerminalWindowHost& host)
{
    if (!open) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(760.0f, 320.0f), ImGuiCond_FirstUseEver);
    const bool visible = ImGui::Begin(Tr(StrId::Win_Terminal), &open, kWindowFlags);
    if (!visible) {
        ImGui::End();   // タブが隠れている間。セッションは Pump が進め続ける
        return;
    }
    DrawToolbar();
    DrawStatusLine();
    DrawSurface(host);
    DrawSettingsPopup();
    DrawContextMenu();
    popupWasOpen_ = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    ImGui::End();
}

void TerminalWindow::DrawToolbar()
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s:", Tr(StrId::Term_Shell));
    ImGui::SameLine();
    ImGui::TextUnformatted(shellName_.empty() ? "-" : shellName_.c_str());
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Term_Restart))) {
        RequestRestart();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!session_);
    if (ImGui::Button(Tr(StrId::Term_ClearScrollback))) {
        session_->ClearScrollback();
        viewOffset_ = 0;
        selection_ = TerminalSelection{};
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Term_Settings))) {
        ImGui::OpenPopup(kSettingsPopupId);
    }
}

void TerminalWindow::DrawStatusLine()
{
    if (shellError_ != TerminalShellError::None) {
        StrId message = StrId::Term_NoAutoShell;
        if (shellError_ == TerminalShellError::ConfiguredNotFound) {
            message = StrId::Term_ShellNotFound;
        } else if (shellError_ == TerminalShellError::ConfiguredMalformed) {
            message = StrId::Term_ShellMalformed;
        }
        ImGui::TextColored(themeColor::Error, "%s", Tr(message));
        return;
    }
    if (!session_) {
        return;
    }
    switch (session_->State()) {
    case TerminalSessionState::Exited: {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(themeColor::Warning, "%s %u", Tr(StrId::Term_Exited), static_cast<unsigned>(session_->ExitCode()));
        ImGui::SameLine();
        const std::string label = std::string(Tr(StrId::Term_Restart)) + "##exited";
        if (ImGui::SmallButton(label.c_str())) {
            RequestRestart();   // session_ は破棄済み。以降触らない
        }
        break;
    }
    case TerminalSessionState::Failed:
        ImGui::TextColored(themeColor::Error, "%s", Tr(StrId::Term_StartFailed));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", session_->ErrorMessage().c_str());   // 英語の詳細 (ログと同じ)
        break;
    case TerminalSessionState::Unsupported:
        ImGui::TextColored(themeColor::Error, "%s", Tr(StrId::Term_Unsupported));
        break;
    default:
        break;
    }
}

void TerminalWindow::DrawSurface(const TerminalWindowHost& host)
{
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 1.0f || avail.y < 1.0f) {
        return;
    }

    // 等幅フォントが無いときは null のまま (既定フォント + 1 セルずつ置く)。サイズは設定値 (DPI スケール前)
    ImFont* mono = EditorMonospaceFont();
    ImGui::PushFont(mono, static_cast<float>(ClampTerminalFontSize(settings_.fontSize)));
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    const float cellW = ImGui::CalcTextSize("M").x;
    const float cellH = std::ceil(ImGui::GetTextLineHeight());
    if (cellW <= 0.0f || cellH <= 0.0f) {
        ImGui::PopFont();
        return;
    }
    grid_ = TerminalGridFromPixels(avail.x, avail.y, cellW, cellH);
    if (!session_ && !startAttempted_) {
        StartSession(host, grid_);   // 初回表示 (遅延起動)。再起動もここへ戻る
    }
    if (session_) {
        session_->Resize(grid_.cols, grid_.rows);
    }

    const ImGuiIO& io = ImGui::GetIO();
    ImGui::InvisibleButton("##terminal_surface", avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows | ImGuiFocusedFlags_NoPopupHierarchy);

    const TerminalPalette palette = { PackRgb(themeColor::Text), PackRgb(themeColor::BgApp) };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), ToColor(palette.defaultBg));

    if (session_) {
        const TerminalScreen& screen = session_->Screen();
        const int scrollback = screen.ScrollbackCount();
        viewOffset_ = screen.IsAltScreen() ? 0 : ClampTerminalViewOffset(viewOffset_, scrollback);
        // 画面の行 row を、スクロールバックを含む絶対行へ (選択の座標系。TerminalLayout.h)
        const auto absRowOf = [&](int row) { return row - viewOffset_ + scrollback; };

        // ---- マウス: ホイールで履歴、左ドラッグで選択、右クリックで貼付 (選択中はメニュー) ----
        const ImVec2 local(io.MousePos.x - origin.x, io.MousePos.y - origin.y);
        const TerminalCellPos mouseCell = TerminalCellFromPixels(local.x, local.y, cellW, cellH, grid_.cols, grid_.rows);
        if (hovered && io.MouseWheel != 0.0f) {
            viewOffset_ = ScrollTerminalView(viewOffset_, io.MouseWheel, kWheelLinesPerNotch, scrollback, screen.IsAltScreen());
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const int row = absRowOf(mouseCell.row);
            selection_ = TerminalSelection{ false, row, mouseCell.col, row, mouseCell.col };   // クリックだけなら前の選択を消す
            dragging_ = true;
        }
        if (dragging_) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                selection_.headRow = absRowOf(mouseCell.row);
                selection_.headCol = mouseCell.col;
                selection_.active = selection_.headRow != selection_.anchorRow || selection_.headCol != selection_.anchorCol;
            } else {
                dragging_ = false;
            }
        }
        if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
            if (selection_.active) {
                ImGui::OpenPopup(kContextPopupId);
            } else {
                PasteClipboard();
            }
        }

        // ---- キーボード / IME ----
        // ポップアップが開いていた次のフレームは送らない: Esc でポップアップを閉じた同じ押下が、閉じた直後にこの窓へ
        // フォーカスが戻って子への Esc になってしまう (確定の Enter なども同じ)
        if (focused && !popupWasOpen_) {
            HandleKeyboard(grid_.rows);
        }

        // ---- 描画 ----
        dl->PushClipRect(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), true);
        const int cols = screen.Cols();
        rowCells_.resize(static_cast<size_t>(cols));
        const int rows = std::min(grid_.rows, screen.Rows());
        for (int row = 0; row < rows; ++row) {
            const int line = row - viewOffset_;
            for (int col = 0; col < cols; ++col) {
                rowCells_[static_cast<size_t>(col)] = screen.GetCell(line, col);
            }
            BuildTerminalRowRuns(rowCells_.data(), cols, palette, mono != nullptr, rowRuns_);
            const float y = origin.y + static_cast<float>(row) * cellH;
            for (const TerminalBgRun& run : rowRuns_.backgrounds) {
                const float x0 = origin.x + static_cast<float>(run.col) * cellW;
                dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + static_cast<float>(run.cells) * cellW, y + cellH), ToColor(run.rgb));
            }
            const TerminalColSpan span = TerminalSelectionSpanOnRow(selection_, absRowOf(row), cols);
            if (!span.Empty()) {
                const float x0 = origin.x + static_cast<float>(span.first) * cellW;
                const float x1 = origin.x + static_cast<float>(span.last + 1) * cellW;
                const ImVec4& accent = themeColor::Accent;
                dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + cellH), ToColor(PackRgb(accent), kSelectionAlpha));
            }
            for (const TerminalTextRun& run : rowRuns_.texts) {
                const ImVec2 pos(origin.x + static_cast<float>(run.col) * cellW, y);
                const ImU32 color = ToColor(run.rgb);
                const char* begin = run.utf8.data();
                const char* end = begin + run.utf8.size();
                dl->AddText(font, fontSize, pos, color, begin, end);
                if (run.bold) {
                    dl->AddText(font, fontSize, ImVec2(pos.x + 1.0f, pos.y), color, begin, end);   // 太字の書体を持たないので 1px ずらして重ねる
                }
                const float x1 = pos.x + static_cast<float>(run.cells) * cellW;
                if (run.underline) {
                    dl->AddLine(ImVec2(pos.x, y + cellH - 1.0f), ImVec2(x1, y + cellH - 1.0f), color);
                }
                if (run.strike) {
                    dl->AddLine(ImVec2(pos.x, y + cellH * 0.5f), ImVec2(x1, y + cellH * 0.5f), color);
                }
            }
        }

        // カーソル (点滅なし)。フォーカスがあれば塗り、無ければ枠。履歴を見ているとき・子が終わったあとは出さない
        const TerminalCursor cursor = screen.GetCursor();
        const bool cursorInGrid = cursor.row >= 0 && cursor.row < rows && cursor.col >= 0 && cursor.col < cols;
        if (cursorInGrid) {
            const float cx = origin.x + static_cast<float>(cursor.col) * cellW;
            const float cy = origin.y + static_cast<float>(cursor.row) * cellH;
            if (focused) {
                // IME の候補窓をカーソルの位置へ出す。確定した文字だけを HandleKeyboard が送る
                ImGuiPlatformImeData& ime = ImGui::GetCurrentContext()->PlatformImeData;
                ime.WantVisible = true;
                ime.InputPos = ImVec2(cx, cy);
                ime.InputLineHeight = cellH;
                ime.ViewportId = ImGui::GetWindowViewport()->ID;
            }
            const bool showCursor = cursor.visible && viewOffset_ == 0 && session_->State() == TerminalSessionState::Running;
            if (showCursor) {
                const float width = static_cast<float>(std::max(screen.GetCell(cursor.row, cursor.col).width, 1)) * cellW;
                if (focused) {
                    dl->AddRectFilled(ImVec2(cx, cy), ImVec2(cx + width, cy + cellH), ToColor(palette.defaultFg, kCursorFillAlpha));
                } else {
                    dl->AddRect(ImVec2(cx, cy), ImVec2(cx + width, cy + cellH), ToColor(palette.defaultFg, kCursorOutlineAlpha));
                }
            }
        }

        // 履歴を見ているしるし (右上)
        if (viewOffset_ > 0) {
            const std::string tag = std::string(Tr(StrId::Term_ViewingHistory)) + " +" + std::to_string(viewOffset_);
            const ImVec2 size = ImGui::CalcTextSize(tag.c_str());
            const ImVec2 min(origin.x + avail.x - size.x - 12.0f, origin.y + 2.0f);
            dl->AddRectFilled(min, ImVec2(min.x + size.x + 8.0f, min.y + size.y + 2.0f), ToColor(palette.defaultBg, 220));
            dl->AddText(font, fontSize, ImVec2(min.x + 4.0f, min.y + 1.0f), ToColor(PackRgb(themeColor::TextDim)), tag.c_str());
        }
        dl->PopClipRect();
    }

    if (focused) {
        // エディタのショートカット (Ctrl+S / Delete / F など) を抑止する。EditorApp::HandleShortcuts が WantTextInput を見る。
        // このフレームの EndFrame が次フレームの io.WantTextInput へ写す
        ImGui::GetCurrentContext()->PlatformImeData.WantTextInput = true;
    }
    ImGui::PopFont();
}

void TerminalWindow::HandleKeyboard(int rows)
{
    const ImGuiIO& io = ImGui::GetIO();
    uint8_t modifiers = kTerminalModNone;
    if (io.KeyShift) {
        modifiers |= kTerminalModShift;
    }
    if (io.KeyAlt) {
        modifiers |= kTerminalModAlt;
    }
    if (io.KeyCtrl) {
        modifiers |= kTerminalModCtrl;
    }
    const bool shiftOnly = io.KeyShift && !io.KeyCtrl && !io.KeyAlt;
    const TerminalScreen& screen = session_->Screen();
    bool sent = false;

    const auto sendKey = [&](ImGuiKey imguiKey, TerminalKey key) {
        if (!ImGui::IsKeyPressed(imguiKey)) {
            return;
        }
        // Shift+PageUp / PageDown は子へ送らず履歴をページ単位で見る (Windows Terminal と同じ)
        if (shiftOnly && (key == TerminalKey::PageUp || key == TerminalKey::PageDown)) {
            viewOffset_ = ScrollTerminalView(viewOffset_, key == TerminalKey::PageUp ? 1.0f : -1.0f, std::max(rows - 1, 1),
                                             screen.ScrollbackCount(), screen.IsAltScreen());
            return;
        }
        session_->SendKey(key, modifiers);
        sent = true;
    };
    for (const KeyBinding& binding : kKeyBindings) {
        sendKey(binding.imgui, binding.terminal);
    }
    for (int i = 0; i < 12; ++i) {
        sendKey(static_cast<ImGuiKey>(ImGuiKey_F1 + i), static_cast<TerminalKey>(static_cast<int>(TerminalKey::F1) + i));
    }

    // Ctrl+文字: 制御文字 (^C など)、コピー、貼付。AltGr (= Ctrl+Alt) は文字として InputQueueCharacters に来るのでここでは扱わない
    if (io.KeyCtrl && !io.KeyAlt) {
        for (int i = 0; i < 26; ++i) {
            if (!ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_A + i))) {
                continue;
            }
            const char letter = static_cast<char>('a' + i);
            switch (ClassifyTerminalCtrlLetter(letter, io.KeyShift, selection_.active)) {
            case TerminalChordAction::Copy:
                CopySelection();
                break;
            case TerminalChordAction::Paste:
                PasteClipboard();
                break;
            case TerminalChordAction::SendToShell:
                session_->SendChar(static_cast<char32_t>(letter), modifiers);
                sent = true;
                break;
            case TerminalChordAction::None:
                break;
            }
        }
    } else if (io.KeyAlt && !io.KeyCtrl) {
        // Alt+文字: Win32 バックエンドは WM_SYSCHAR を文字として渡さないので、キーから作る (ESC 前置)。
        // 大文字小文字は文字そのもので表すので、Shift は修飾から外す
        const uint8_t withoutShift = static_cast<uint8_t>(modifiers & ~kTerminalModShift);
        for (int i = 0; i < 26; ++i) {
            if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_A + i))) {
                session_->SendChar(static_cast<char32_t>((io.KeyShift ? 'A' : 'a') + i), withoutShift);
                sent = true;
            }
        }
        for (int i = 0; i < 10; ++i) {
            if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_0 + i))) {
                session_->SendChar(static_cast<char32_t>('0' + i), withoutShift);
                sent = true;
            }
        }
    }

    // 文字入力 (IME の確定文字を含む)。ImGui は Enter / Tab / BS / Ctrl+文字 の制御文字も文字として積むので、
    // それらはキーの経路 (上) で送り済みとして捨てる。未確定文字は WM_CHAR にならず、ここへ来ない
    if (!io.KeyCtrl || io.KeyAlt) {
        for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
            const ImWchar c = io.InputQueueCharacters[i];
            if (c < 0x20 || c == 0x7F) {
                continue;
            }
            session_->SendChar(static_cast<char32_t>(c), kTerminalModNone);
            sent = true;
        }
    }

    if (sent) {
        viewOffset_ = 0;
        selection_.active = false;
    }
}

void TerminalWindow::CopySelection()
{
    if (!session_) {
        return;
    }
    const std::string text = TerminalSelectionText(session_->Screen(), selection_);
    if (!text.empty()) {
        ImGui::SetClipboardText(text.c_str());
    }
    selection_.active = false;   // コピーしたら選択を解く (次の Ctrl+C は子への割り込みになる)
}

void TerminalWindow::PasteClipboard()
{
    if (!session_) {
        return;
    }
    const char* text = ImGui::GetClipboardText();
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    session_->SendPaste(text);
    viewOffset_ = 0;
    selection_.active = false;
}

void TerminalWindow::DrawSettingsPopup()
{
    if (!ImGui::BeginPopup(kSettingsPopupId)) {
        return;
    }
    ImGui::TextUnformatted(Tr(StrId::Term_SettingsShell));
    ImGui::SetNextItemWidth(kSettingsInputWidth);
    ImGui::InputTextWithHint("##terminalShell", Tr(StrId::Term_SettingsShellHint), shellBuf_, sizeof(shellBuf_));
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        MarkSettingsDirty();
    }

    ImGui::TextUnformatted(Tr(StrId::Term_SettingsFontSize));
    ImGui::SetNextItemWidth(kSettingsInputWidth);
    ImGui::InputInt("##terminalFontSize", &settings_.fontSize, 1, 2);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        settings_.fontSize = ClampTerminalFontSize(settings_.fontSize);
        MarkSettingsDirty();
    }

    ImGui::TextUnformatted(Tr(StrId::Term_SettingsScrollback));
    ImGui::SetNextItemWidth(kSettingsInputWidth);
    ImGui::InputInt("##terminalScrollback", &settings_.scrollback, 100, 1000);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        settings_.scrollback = ClampTerminalScrollback(settings_.scrollback);
        MarkSettingsDirty();
    }

    if (ImGui::Checkbox(Tr(StrId::Term_SettingsLoadVsEnv), &settings_.loadVsDevEnv)) {
        MarkSettingsDirty();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, themeColor::TextDim);
    ImGui::TextWrapped("%s", Tr(StrId::Term_SettingsApplyOnRestart));
    ImGui::PopStyleColor();
    ImGui::EndPopup();
}

void TerminalWindow::DrawContextMenu()
{
    if (!ImGui::BeginPopup(kContextPopupId)) {
        return;
    }
    if (ImGui::MenuItem(Tr(StrId::Term_Copy), nullptr, false, selection_.active)) {
        CopySelection();
    }
    if (ImGui::MenuItem(Tr(StrId::Term_Paste))) {
        PasteClipboard();
    }
    ImGui::EndPopup();
}

} // namespace mye
