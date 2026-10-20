//====================================================================================
//                          SpriteAnimEditor.cpp
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                  スプライト列クリップの編集 UI と、スプライト 1 枚を描く小物
//====================================================================================
#include "Editor/Windows/Animation/SpriteAnimEditor.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

#include "imgui.h"

#include "Editor/SourceControl/ScmHint.h"
#include "Engine/Core/Asset/AssetGuidResolver.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Engine/Rendering/Sprite2D/SpriteLibrary.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/ImGui/ImGuiRenderer.h" // ImGuiDrawListSetPointSampling

namespace mye {

namespace {

constexpr float kPreviewBox = 128.0f;
constexpr float kRowThumbBox = 32.0f;
constexpr float kTicksPerSecond = 60.0f; // 再生は sim と同じ 60 tick/秒
constexpr ImU32 kThumbBackColor = IM_COL32(30, 32, 38, 255);
constexpr ImU32 kThumbFrameColor = IM_COL32(90, 92, 100, 255);

// 数字の並びを数として比べる ("walk_2" < "walk_10")。スライスを自然な順に並べるため
bool NaturalLess(const std::string& a, const std::string& b)
{
    size_t i = 0;
    size_t j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit(static_cast<unsigned char>(a[i])) && std::isdigit(static_cast<unsigned char>(b[j]))) {
            size_t endA = i;
            size_t endB = j;
            while (endA < a.size() && std::isdigit(static_cast<unsigned char>(a[endA]))) {
                ++endA;
            }
            while (endB < b.size() && std::isdigit(static_cast<unsigned char>(b[endB]))) {
                ++endB;
            }
            size_t startA = i;
            size_t startB = j;
            while (startA + 1 < endA && a[startA] == '0') {
                ++startA;
            }
            while (startB + 1 < endB && b[startB] == '0') {
                ++startB;
            }
            const size_t lenA = endA - startA;
            const size_t lenB = endB - startB;
            if (lenA != lenB) {
                return lenA < lenB;
            }
            const int c = a.compare(startA, lenA, b, startB, lenB);
            if (c != 0) {
                return c < 0;
            }
            i = endA;
            j = endB;
        } else {
            if (a[i] != b[j]) {
                return static_cast<unsigned char>(a[i]) < static_cast<unsigned char>(b[j]);
            }
            ++i;
            ++j;
        }
    }
    return (a.size() - i) < (b.size() - j);
}

// "tex#name" を ("tex", "name") に分ける。'#' が無い (全面) なら name は空
void SplitDisplayName(const std::string& display, std::string& tex, std::string& slice)
{
    const size_t hash = display.find('#');
    tex = display.substr(0, hash);
    slice = hash == std::string::npos ? std::string() : display.substr(hash + 1);
}

std::string SpriteDisplayName(AssetID id)
{
    SpriteLibrary* lib = spritelib::Library();
    if (lib == nullptr || id.IsNull()) {
        return std::string();
    }
    std::vector<SpriteListing> listing;
    lib->Enumerate(listing);
    for (const SpriteListing& l : listing) {
        if (l.id == id) {
            return l.displayName;
        }
    }
    return std::string();
}

} // namespace

bool DrawSpriteThumb(EngineContext& ctx, AssetID sprite, float box)
{
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + box, p0.y + box);
    ImGui::Dummy(ImVec2(box, box));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, kThumbBackColor);
    dl->AddRect(p0, p1, kThumbFrameColor);

    SpriteLibrary* lib = spritelib::Library();
    const SpriteDef* def = (lib != nullptr && !sprite.IsNull()) ? lib->Get(sprite) : nullptr;
    if (def == nullptr || ctx.resources == nullptr) {
        return false;
    }
    TextureLibrary& textures = ctx.resources->textures;
    Texture* tex = textures.Get(def->texture);
    if (tex == nullptr) {
        // 未ロード: 読み込みを頼んで、このフレームは枠だけ (描画側 Sprite2DRenderer::Resolve と同じ手順)
        const std::wstring path = assetguid::ResolvePath(def->texture.value);
        if (!path.empty()) {
            textures.RequestLoadFileAsync(path);
        }
        return false;
    }
    if (textures.IsPending(def->texture) || !tex->srv || tex->width <= 0 || tex->height <= 0) {
        return false;
    }
    const float texW = static_cast<float>(tex->width);
    const float texH = static_cast<float>(tex->height);
    float rx = 0.0f;
    float ry = 0.0f;
    float rw = texW;
    float rh = texH;
    if (!def->IsFullTexture()) {
        rx = static_cast<float>(def->rect[0]);
        ry = static_cast<float>(def->rect[1]);
        rw = static_cast<float>(def->rect[2]);
        rh = static_cast<float>(def->rect[3]);
    }
    if (rw <= 0.0f || rh <= 0.0f) {
        return false;
    }
    const float scale = box / (std::max)(rw, rh);
    const float dw = rw * scale;
    const float dh = rh * scale;
    const ImVec2 q0(p0.x + (box - dw) * 0.5f, p0.y + (box - dh) * 0.5f);
    ImGuiDrawListSetPointSampling(dl, true);
    dl->AddImage(reinterpret_cast<ImTextureID>(tex->srv.Get()), q0, ImVec2(q0.x + dw, q0.y + dh),
                 ImVec2(rx / texW, ry / texH), ImVec2((rx + rw) / texW, (ry + rh) / texH));
    ImGuiDrawListSetPointSampling(dl, false);
    return true;
}

bool DrawSpritePicker(const char* id, AssetID& sprite, float width)
{
    bool changed = false;
    std::string label = SpriteDisplayName(sprite);
    if (label.empty()) {
        label = sprite.IsNull() ? Tr(StrId::Insp_NoneItem) : Tr(StrId::SprAnim_Unresolved);
    }
    ImGui::PushID(id);
    if (ImGui::Button(label.c_str(), ImVec2(width, 0.0f))) {
        ImGui::OpenPopup("##spritepick");
    }
    if (ImGui::BeginPopup("##spritepick")) {
        static ImGuiTextFilter filter;
        filter.Draw("##filter", 200.0f);
        ImGui::BeginChild("##list", ImVec2(260.0f, 240.0f), true);
        if (ImGui::Selectable(Tr(StrId::Insp_NoneItem), sprite.IsNull())) {
            sprite = AssetID{};
            changed = true;
        }
        if (SpriteLibrary* lib = spritelib::Library()) {
            std::vector<SpriteListing> listing;
            lib->Enumerate(listing);
            for (const SpriteListing& l : listing) {
                if (!filter.PassFilter(l.displayName.c_str())) {
                    continue;
                }
                if (ImGui::Selectable(l.displayName.c_str(), l.id == sprite)) {
                    sprite = l.id;
                    changed = true;
                }
            }
        }
        ImGui::EndChild();
        if (changed) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

void LoadSpriteAnimEdit(const std::wstring& path, SpriteAnimEditState& state)
{
    state = SpriteAnimEditState{};
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) {
        return;
    }
    nlohmann::json root;
    try {
        f >> root;
    } catch (const nlohmann::json::exception&) {
        return;
    }
    state.valid = SpriteAnimLibrary::FromJson(root, state.anim); // 読み値は Sanitize 済み
    if (state.valid && state.anim.name.empty()) {
        // "X.spriteanim.json" → "X" (stem を 2 回剥がす)
        state.anim.name = WideToUtf8(std::filesystem::path(path).stem().stem().wstring());
        state.anim.RebuildDerived();
    }
}

namespace {

// 追加ポップアップ: スライス済みのテクスチャを選ぶと、そのスライスを自然な順で末尾へ足す
void DrawAddSlices(SpriteAnimEditState& state)
{
    if (ImGui::Button(Tr(StrId::SprAnim_AddSlices))) {
        ImGui::OpenPopup("##addslices");
    }
    if (!ImGui::BeginPopup("##addslices")) {
        return;
    }
    // テクスチャ名 → そのスライス (表示名の昇順で来るが、足すときは自然順に並べ直す)
    std::map<std::string, std::vector<SpriteListing>> byTexture;
    if (SpriteLibrary* lib = spritelib::Library()) {
        std::vector<SpriteListing> listing;
        lib->Enumerate(listing);
        for (const SpriteListing& l : listing) {
            std::string tex;
            std::string slice;
            SplitDisplayName(l.displayName, tex, slice);
            if (!slice.empty()) {
                byTexture[tex].push_back(l);
            }
        }
    }
    if (byTexture.empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::SprAnim_NoSlices));
    }
    for (auto& [tex, slices] : byTexture) {
        if (!ImGui::Selectable(tex.c_str())) {
            continue;
        }
        std::sort(slices.begin(), slices.end(), [](const SpriteListing& a, const SpriteListing& b) {
            return NaturalLess(a.displayName, b.displayName);
        });
        const int32_t lastTicks = state.anim.frames.empty() ? 6 : state.anim.frames.back().ticks;
        for (const SpriteListing& s : slices) {
            if (state.anim.frames.size() >= kMaxSpriteAnimFrames) {
                break;
            }
            state.anim.frames.push_back({ s.id.value, lastTicks });
        }
        state.anim.RebuildDerived();
    }
    ImGui::EndPopup();
}

bool SaveSpriteAnim(const std::wstring& path, SpriteAnimEditState& state)
{
    SpriteAnimLibrary::Sanitize(state.anim); // 手入力の範囲外をファイルへ焼かない
    std::ofstream out(std::filesystem::path(path), std::ios::binary);
    if (!out) {
        MYE_LOG_ERROR("could not write spriteanim: %s", WideToUtf8(path).c_str());
        return false;
    }
    out << SpriteAnimLibrary::ToJson(state.anim).dump(2);
    out.close();
    // 即時反映: 同一 GUID のまま登録し直す (コントローラは GUID で引くので再解決は要らない)
    if (SpriteAnimLibrary* lib = spriteanim::Library()) {
        lib->LoadFromFile(path);
    }
    scmhint::Changed(path); // バッジと Changes の即時反映
    MYE_LOG_INFO("spriteanim saved: %s", WideToUtf8(path).c_str());
    return true;
}

} // namespace

void DrawSpriteAnimInspector(EngineContext& ctx, const std::wstring& path, SpriteAnimEditState& state)
{
    if (!state.valid) {
        ImGui::TextDisabled("%s", Tr(StrId::SprAnim_Failed));
        return;
    }
    SpriteAnim& anim = state.anim;

    char nameBuf[128];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", anim.name.c_str());
    if (ImGui::InputText(Tr(StrId::SprAnim_Name), nameBuf, sizeof(nameBuf))) {
        anim.name = nameBuf;
        anim.RebuildDerived();
    }
    ImGui::Text(Tr(StrId::SprAnim_Length), anim.lengthTicks, static_cast<int>(anim.frames.size()));

    // ---- プレビュー (ステップ / 再生 / スクラブ) ----
    ImGui::Spacing();
    const int32_t lastTick = (std::max)(anim.lengthTicks - 1, 0);
    if (state.playing && anim.lengthTicks > 0) {
        state.playAccum += ImGui::GetIO().DeltaTime * kTicksPerSecond;
        const int32_t steps = static_cast<int32_t>(state.playAccum);
        state.playAccum -= static_cast<float>(steps);
        state.previewTick = (state.previewTick + steps) % anim.lengthTicks;
    }
    state.previewTick = std::clamp(state.previewTick, 0, lastTick);
    const int32_t frame = SpriteAnimFrameAt(anim, state.previewTick);
    const AssetID shown = frame >= 0 ? AssetID{ anim.frames[static_cast<size_t>(frame)].sprite } : AssetID{};
    DrawSpriteThumb(ctx, shown, kPreviewBox);
    if (ImGui::Button(state.playing ? Tr(StrId::AnimPrev_Pause) : Tr(StrId::AnimPrev_Play))) {
        state.playing = !state.playing;
        state.playAccum = 0.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("|<")) {
        state.previewTick = 0;
        state.playing = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("-1")) {
        state.previewTick = state.previewTick == 0 ? lastTick : state.previewTick - 1;
        state.playing = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("+1")) {
        state.previewTick = state.previewTick >= lastTick ? 0 : state.previewTick + 1;
        state.playing = false;
    }
    ImGui::SameLine();
    ImGui::Text(Tr(StrId::SprAnim_PreviewAt), state.previewTick, anim.lengthTicks, frame + 1);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderInt("##sprite_anim_tick", &state.previewTick, 0, lastTick)) {
        state.playing = false;
    }

    // ---- コマの一覧 ----
    ImGui::SeparatorText(Tr(StrId::SprAnim_Frames));
    if (anim.frames.empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::SprAnim_NoFrames));
    }
    bool edited = false;
    int removeAt = -1;
    int swapWith = -1;
    int swapFrom = -1;
    const float pickerWidth = (std::max)(ImGui::GetContentRegionAvail().x - 200.0f, 80.0f);
    for (int i = 0; i < static_cast<int>(anim.frames.size()); ++i) {
        SpriteAnimFrame& f = anim.frames[static_cast<size_t>(i)];
        ImGui::PushID(i);
        if (i == frame) {
            ImGui::TextUnformatted(">");
        } else {
            ImGui::Dummy(ImGui::CalcTextSize(">")); // 印の有無で行の頭がずれないように
        }
        ImGui::SameLine();
        DrawSpriteThumb(ctx, AssetID{ f.sprite }, kRowThumbBox);
        ImGui::SameLine();
        ImGui::BeginGroup();
        AssetID picked{ f.sprite };
        if (DrawSpritePicker("pick", picked, pickerWidth)) {
            f.sprite = picked.value;
            edited = true;
        }
        ImGui::SetNextItemWidth(70.0f);
        if (ImGui::DragInt(Tr(StrId::SprAnim_Ticks), &f.ticks, 0.2f, 1, kMaxSpriteAnimFrameTicks)) {
            f.ticks = std::clamp(f.ticks, 1, kMaxSpriteAnimFrameTicks);
            edited = true;
        }
        ImGui::EndGroup();
        ImGui::SameLine();
        if (ImGui::SmallButton("^") && i > 0) {
            swapFrom = i;
            swapWith = i - 1;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("v") && i + 1 < static_cast<int>(anim.frames.size())) {
            swapFrom = i;
            swapWith = i + 1;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            removeAt = i;
        }
        ImGui::PopID();
    }
    if (swapFrom >= 0) {
        std::swap(anim.frames[static_cast<size_t>(swapFrom)], anim.frames[static_cast<size_t>(swapWith)]);
        edited = true;
    }
    if (removeAt >= 0) {
        anim.frames.erase(anim.frames.begin() + removeAt);
        edited = true;
    }
    if (anim.frames.size() < kMaxSpriteAnimFrames && ImGui::Button(Tr(StrId::SprAnim_AddFrame))) {
        SpriteAnimFrame f = anim.frames.empty() ? SpriteAnimFrame{} : anim.frames.back();
        anim.frames.push_back(f);
        edited = true;
    }
    ImGui::SameLine();
    const size_t before = anim.frames.size();
    DrawAddSlices(state);
    if (anim.frames.size() != before) {
        edited = true;
    }
    if (edited) {
        anim.RebuildDerived();
    }

    ImGui::Separator();
    if (ImGui::Button(Tr(StrId::Common_Save), ImVec2(90, 0))) {
        SaveSpriteAnim(path, state);
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Insp_Revert), ImVec2(90, 0))) {
        LoadSpriteAnimEdit(path, state);
    }
}

} // namespace mye
