#include "Editor/Widgets/EditorWidgets.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string>

#include "Editor/SourceControl/SourceControlState.h" // ChangeState / ChangeStateBadge (M66i)
#include "Engine/Engine/Asset/AssetDatabase.h"       // ClassifyPath (ファイル種別アイコン)
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/ImGui/ImGuiTheme.h"

#include "imgui.h"
#include "fontawesome/IconsFontAwesome6.h"
#include "imgui_internal.h" // SeparatorEx (縦区切り)

namespace mye {

namespace {

ImVec4 Lighten(const ImVec4& c, float k)
{
    return ImVec4((std::min)(c.x * k, 1.0f), (std::min)(c.y * k, 1.0f), (std::min)(c.z * k, 1.0f),
                  c.w);
}

// pos (スクリーン座標) から「色付きアイコン + 通常色ラベル」を描く。直前のアイテム矩形でクリップする
void DrawIconLabelAt(ImVec2 pos, const char* icon, const ImVec4& iconColor, const char* label)
{
    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec4 clip(mn.x, mn.y, mx.x, mx.y);
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    dl->AddText(font, size, pos, ImGui::GetColorU32(iconColor), icon, nullptr, 0.0f, &clip);
    pos.x += ImGui::CalcTextSize(icon).x + ImGui::GetStyle().ItemInnerSpacing.x;
    dl->AddText(font, size, pos, ImGui::GetColorU32(ImGuiCol_Text), label, nullptr, 0.0f, &clip);
}

} // namespace

bool IconMenuItem(const char* icon, const char* label, bool enabled)
{
    const std::string text = std::string(icon) + " " + label;
    return ImGui::MenuItem(text.c_str(), nullptr, false, enabled);
}

bool ToolbarToggle(const char* label, bool on, const char* tooltip, bool mode)
{
    int pushed = 0;
    if (on) {
        // ON の面色。テーマの ButtonHovered/Active は白の半透明オーバーレイなので、
        // Button だけ差し替えるとホバーの瞬間に ON の色が消える — 3 状態とも押す
        const ImVec4 base = mode
            ? ImVec4(themeColor::PlayAccent.x * 0.55f, themeColor::PlayAccent.y * 0.55f,
                     themeColor::PlayAccent.z * 0.55f, 1.0f)
            : themeColor::AccentSoft;
        ImGui::PushStyleColor(ImGuiCol_Button, base);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Lighten(base, 1.25f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Lighten(base, 1.45f));
        pushed = 3;
    }
    const bool pressed = ImGui::Button(label);
    ImGui::PopStyleColor(pushed);
    if (tooltip != nullptr && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }
    return pressed;
}

void ToolbarSeparator()
{
    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical, 1.0f);
    ImGui::SameLine();
}

bool BeginToolbarOverlay(const char* id, float height, const ImVec4* tint)
{
    ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_PopupBg];
    if (tint != nullptr) {
        const float k = 0.30f; // 面へ薄く混ぜるだけ (tint をそのまま塗ると文字が沈む)
        bg = ImVec4(bg.x + (tint->x - bg.x) * k, bg.y + (tint->y - bg.y) * k,
                    bg.z + (tint->z - bg.z) * k, bg.w);
    }
    bg.w = 0.92f; // ビューポートが薄く透ける (完全不透明だとパネルではなく「板」に見える)
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    if (height <= 0.0f) {
        height = ImGui::GetFrameHeight() + 12.0f; // 上下 padding 6px ぶん
    }
    return ImGui::BeginChild(id, ImVec2(0.0f, height),
                             ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_Borders,
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
}

void EndToolbarOverlay()
{
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// ---- ToolbarFlow (M47b追補) ----

int ToolbarFlow::BeginFrame(float limitWidth)
{
    int rows = 1;
    float x = 0.0f;
    for (int i = 0; i < groupCount_ && i < kMaxGroups; ++i) {
        const float need = widths_[i] + (i > 0 ? sepWidth_ : 0.0f);
        if (i > 0 && x + need > limitWidth) {
            breakBefore_[i] = true;
            x = widths_[i];
            ++rows;
        } else {
            breakBefore_[i] = false;
            x += need;
        }
    }
    return rows;
}

void ToolbarFlow::FirstGroup()
{
    group_ = 0;
    groupStartX_ = ImGui::GetCursorScreenPos().x;
}

void ToolbarFlow::Separator()
{
    // ★カーソル X ではなく「最後のアイテムの右端 (スクリーン座標)」で幅を測る —
    //   スライダのように SameLine で終わらないグループはカーソルが既に次行へ
    //   落ちていて、カーソル基準だと幅がゼロや負に化けるため
    if (group_ < kMaxGroups) {
        widths_[group_] =
            ImGui::GetItemRectMax().x - groupStartX_ + ImGui::GetStyle().ItemSpacing.x;
    }
    ImGui::SameLine(); // SameLine で終わっていないグループも行内へ戻して起点を揃える
    ++group_;
    if (group_ < kMaxGroups && breakBefore_[group_]) {
        ImGui::NewLine(); // 直前の SameLine を打ち消して次の行頭へ (ImGui の規約)
    } else {
        const float before = ImGui::GetCursorScreenPos().x;
        ToolbarSeparator();
        sepWidth_ = ImGui::GetCursorScreenPos().x - before; // テーマ余白依存なので実測で追随
    }
    groupStartX_ = ImGui::GetCursorScreenPos().x;
}

void ToolbarFlow::EndFrame()
{
    if (group_ < kMaxGroups) {
        widths_[group_] =
            ImGui::GetItemRectMax().x - groupStartX_ + ImGui::GetStyle().ItemSpacing.x;
    }
    groupCount_ = group_ + 1;
}

float ToolbarFlow::OverlayHeight(int rows)
{
    // BeginToolbarOverlay の既定 (フレーム高 + 上下 padding 6px) の行数拡張
    return ImGui::GetFrameHeight() * static_cast<float>(rows)
        + ImGui::GetStyle().ItemSpacing.y * static_cast<float>(rows - 1) + 12.0f;
}

void DrawItemIconLabel(const char* icon, const ImVec4& iconColor, const char* label, bool framed)
{
    // TreeNodeBehavior のラベル開始位置を再現する (imgui 1.92:
    // text_offset_x = FontSize + FramePadding.x * (framed ? 3 : 2)、
    // text_offset_y = framed ? FramePadding.y : 0)。imgui 更新で描画がズレたらここを疑う
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 mn = ImGui::GetItemRectMin();
    DrawIconLabelAt(ImVec2(mn.x + ImGui::GetFontSize() + style.FramePadding.x * (framed ? 3.0f : 2.0f),
                           mn.y + (framed ? style.FramePadding.y : 0.0f)),
                    icon, iconColor, label);
}

void DrawSelectableIconLabel(const char* icon, const ImVec4& iconColor, const char* label)
{
    // Selectable は項目矩形を ItemSpacing の半分 (切り捨て) だけ左上へ広げ、文字は元の位置に置く
    // (imgui 1.92 Selectable の spacing_L / spacing_U)
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 mn = ImGui::GetItemRectMin();
    DrawIconLabelAt(ImVec2(mn.x + IM_TRUNC(style.ItemSpacing.x * 0.5f),
                           mn.y + IM_TRUNC(style.ItemSpacing.y * 0.5f)),
                    icon, iconColor, label);
}

void ReserveItemIconLabel(const char* icon, const char* label)
{
    const float width = ImGui::CalcTextSize(icon).x + ImGui::GetStyle().ItemInnerSpacing.x
        + ImGui::CalcTextSize(label).x;
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::Dummy(ImVec2(width, 0.0f));
}

const char* FolderIcon(bool open)
{
    return open ? ICON_FA_FOLDER_OPEN : ICON_FA_FOLDER;
}

const char* FileTypeIcon(const wchar_t* path)
{
    switch (AssetDatabase::ClassifyPath(path)) {
    case AssetType::Texture: return ICON_FA_FILE_IMAGE;
    case AssetType::Model: return ICON_FA_CUBE;
    case AssetType::Prefab:
    case AssetType::Actor: return ICON_FA_CUBES;
    case AssetType::Scene: return ICON_FA_MAP;
    case AssetType::Anim: return ICON_FA_PERSON_RUNNING;
    case AssetType::Controller: return ICON_FA_DIAGRAM_PROJECT;
    case AssetType::Material: return ICON_FA_PALETTE;
    case AssetType::PhysMat: return ICON_FA_BASKETBALL;
    case AssetType::Audio: return ICON_FA_FILE_AUDIO;
    case AssetType::Sound: return ICON_FA_VOLUME_HIGH;
    case AssetType::Mixer: return ICON_FA_SLIDERS;
    case AssetType::Shader: return ICON_FA_WAND_MAGIC_SPARKLES;
    case AssetType::Script: return ICON_FA_FILE_CODE;
    case AssetType::Schema: return ICON_FA_TABLE;
    case AssetType::Terrain: return ICON_FA_MOUNTAIN;
    case AssetType::FxStack: return ICON_FA_LAYER_GROUP;
    case AssetType::Fracture: return ICON_FA_BURST;
    case AssetType::NavMesh: return ICON_FA_ROUTE;
    case AssetType::NavFilter: return ICON_FA_FILTER;
    case AssetType::BehaviorTree: return ICON_FA_SITEMAP;
    case AssetType::Blackboard: return ICON_FA_TABLE_LIST;
    default: break;
    }
    // エンジンのアセット種別に無いもの (ソースコード・文書・動画など) は末尾の拡張子で引く
    std::wstring ext = std::filesystem::path(path).extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    struct ExtIcon {
        const wchar_t* ext;
        const char* icon;
    };
    static const ExtIcon kExtIcons[] = {
        { L".png", ICON_FA_FILE_IMAGE },  { L".jpg", ICON_FA_FILE_IMAGE },
        { L".jpeg", ICON_FA_FILE_IMAGE }, { L".tga", ICON_FA_FILE_IMAGE },
        { L".bmp", ICON_FA_FILE_IMAGE },  { L".dds", ICON_FA_FILE_IMAGE },
        { L".hdr", ICON_FA_FILE_IMAGE },  { L".psd", ICON_FA_FILE_IMAGE },
        { L".mp3", ICON_FA_FILE_AUDIO },  { L".flac", ICON_FA_FILE_AUDIO },
        { L".cpp", ICON_FA_FILE_CODE },   { L".c", ICON_FA_FILE_CODE },
        { L".cc", ICON_FA_FILE_CODE },    { L".h", ICON_FA_FILE_CODE },
        { L".hpp", ICON_FA_FILE_CODE },   { L".inl", ICON_FA_FILE_CODE },
        { L".cs", ICON_FA_FILE_CODE },    { L".rs", ICON_FA_FILE_CODE },
        { L".py", ICON_FA_FILE_CODE },    { L".ps1", ICON_FA_FILE_CODE },
        { L".bat", ICON_FA_FILE_CODE },   { L".cmd", ICON_FA_FILE_CODE },
        { L".sh", ICON_FA_FILE_CODE },    { L".hlsl", ICON_FA_WAND_MAGIC_SPARKLES },
        { L".hlsli", ICON_FA_WAND_MAGIC_SPARKLES },
        { L".json", ICON_FA_FILE_LINES }, { L".ndjson", ICON_FA_FILE_LINES },
        { L".md", ICON_FA_FILE_LINES },   { L".txt", ICON_FA_FILE_LINES },
        { L".yml", ICON_FA_FILE_LINES },  { L".yaml", ICON_FA_FILE_LINES },
        { L".toml", ICON_FA_FILE_LINES }, { L".ini", ICON_FA_FILE_LINES },
        { L".xml", ICON_FA_FILE_LINES },  { L".csv", ICON_FA_FILE_CSV },
        { L".mp4", ICON_FA_FILE_VIDEO },  { L".webm", ICON_FA_FILE_VIDEO },
        { L".avi", ICON_FA_FILE_VIDEO },  { L".mov", ICON_FA_FILE_VIDEO },
        { L".mkv", ICON_FA_FILE_VIDEO },  { L".zip", ICON_FA_FILE_ZIPPER },
        { L".7z", ICON_FA_FILE_ZIPPER },  { L".glb", ICON_FA_CUBE },
        { L".gltf", ICON_FA_CUBE },       { L".fbx", ICON_FA_CUBE },
        { L".obj", ICON_FA_CUBE },
    };
    for (const ExtIcon& e : kExtIcons) {
        if (ext == e.ext) {
            return e.icon;
        }
    }
    return ICON_FA_FILE;
}

const char* FileTypeIconUtf8(const char* path)
{
    return FileTypeIcon(Utf8ToWide(path).c_str());
}

ImVec4 FolderIconColor(bool hovered)
{
    // 配色ルール (ImGuiTheme.h) の帯に収めた金 — 原色寄りの黄 (232,196,80) は目に刺さる
    return hovered ? ImVec4(219.0f / 255.0f, 194.0f / 255.0f, 134.0f / 255.0f, 1.0f)
                   : ImVec4(199.0f / 255.0f, 173.0f / 255.0f, 112.0f / 255.0f, 1.0f);
}

ImVec4 FileIconColor()
{
    return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
}

const char* ScmStateIcon(ChangeState s)
{
    switch (s) {
    case ChangeState::Modified: return ICON_FA_PEN;
    case ChangeState::Added: return ICON_FA_PLUS;
    case ChangeState::Deleted: return ICON_FA_TRASH;
    case ChangeState::Renamed: return ICON_FA_ARROW_RIGHT;
    case ChangeState::Untracked: return ICON_FA_QUESTION;
    case ChangeState::Conflict: return ICON_FA_TRIANGLE_EXCLAMATION;
    default: return "";
    }
}

ImVec4 ScmBadgeColor(ChangeState s)
{
    switch (s) {
    // 競合と D はどちらも Error。**グリフ ('!' と 'D') が区別を持つ**ので色を
    // 分ける必要がない (spec §4.3)。競合中は一覧そのものが競合モードへ切り替わる
    case ChangeState::Conflict:
    case ChangeState::Deleted:
        return themeColor::Error;
    case ChangeState::Added:
        return themeColor::Success;
    case ChangeState::Renamed:
        return themeColor::Prefab;
    case ChangeState::Modified:
        return themeColor::Warning;
    case ChangeState::Untracked:
    case ChangeState::None:
    default:
        return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    }
}

void DrawScmTileBadge(ChangeState s, const ImVec2& tileMin)
{
    const std::string badge = std::string(ScmStateIcon(s)) + " " + ChangeStateBadge(s);
    const char* text = s == ChangeState::None ? "" : badge.c_str();
    if (text[0] == '\0') {
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 mn(tileMin.x + 2.0f, tileMin.y + 2.0f);
    const ImVec2 mx(mn.x + ts.x + 8.0f, mn.y + ts.y + 2.0f);
    // ★**必ず面を敷く**。バッジはサムネイル (明るい絵のことがある) の上に載るので、
    //   文字だけだと白い画像の上で完全に消える。面色はテーマのポップアップ面から
    //   採る (直値の暗色を書くとテーマ替えで 1 箇所だけ取り残される)
    ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_PopupBg];
    bg.w = 0.85f;
    dl->AddRectFilled(mn, mx, ImGui::GetColorU32(bg), 3.0f);
    dl->AddText(ImVec2(mn.x + 4.0f, mn.y + 1.0f), ImGui::GetColorU32(ScmBadgeColor(s)), text);
}

} // namespace mye
