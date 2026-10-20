//====================================================================================
//                          TileAssetEditUi.cpp
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          タイルマップのレイヤーとタイルパレットの編集 UI の実装
//====================================================================================
#include "Editor/Windows/Tilemap/TileAssetEditUi.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"

#include "Editor/Windows/Animation/SpriteAnimEditor.h"
#include "Editor/Windows/Tilemap/TilePaletteEditModel.h"
#include "Editor/Windows/Tilemap/TilemapEditModel.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Engine/Scene/Prefab.h"
#include "Engine/Engine/Scene/SortingLayers.h"
#include "Engine/Engine/Tilemap/TilemapAssets.h"
#include "Engine/Renderer/ImGui/ImGuiTheme.h" // themeColor

namespace mye {
namespace tileui {
namespace {

constexpr size_t kMaxVariants = 64;
constexpr size_t kMaxAnimFrames = 256;
constexpr int kMaxTicksPerFrameUi = 3600;
constexpr float kNeighborButton = 22.0f;
constexpr float kVariantThumb = 28.0f;

// 直前の項目の操作を 1 回の Undo にまとめる記録。項目を作った直後に Track を呼び、全項目を描き終えてから Finish する
class GestureTracker {
public:
    template <class Model>
    void Track(Model& model, bool widgetChanged)
    {
        if (ImGui::IsItemActivated()) {
            model.BeginGesture();
        }
        changed_ = changed_ || widgetChanged;
        endGesture_ = endGesture_ || ImGui::IsItemDeactivated();
    }
    // 項目以外 (ピッカー・ボタン) による変更
    void Mark() { changed_ = true; }
    bool Changed() const { return changed_; }
    template <class Model>
    void FinishGesture(Model& model) const
    {
        if (endGesture_) {
            model.EndGesture();
        }
    }

private:
    bool changed_ = false;
    bool endGesture_ = false;
};

// "tex#name" → "name" (全面スプライトは "tex" のまま)
std::string TileNameFromSprite(const std::string& displayName)
{
    const size_t hash = displayName.find('#');
    return hash == std::string::npos ? displayName : displayName.substr(hash + 1);
}

// 窓の幅で折り返す無効色の文
void WrappedDisabledText(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// ラベル付きの SmallButton を同じ行に続ける
bool SameLineSmallButton(const char* label)
{
    ImGui::SameLine();
    return ImGui::SmallButton(label);
}

} // namespace

// ---------------------------------------------------------------------------
// レイヤー
// ---------------------------------------------------------------------------

void DrawLayerPanel(EngineContext& ctx, TilemapEditModel& model, int& layerIndex)
{
    if (!model.IsLoaded()) {
        return;
    }
    const int count = static_cast<int>(model.Asset().layers.size());
    layerIndex = std::clamp(layerIndex, 0, (std::max)(count - 1, 0));

    // 構造を変える操作の後は layers の参照が変わるので、その場で描画を打ち切る
    ImGui::BeginDisabled(static_cast<size_t>(count) >= kMaxTilemapLayers);
    if (ImGui::Button(Tr(StrId::TilePal_LayerAdd))) {
        const std::string name = std::string(Tr(StrId::TilePal_LayerNewName)) + " " + std::to_string(count + 1);
        if (model.AddLayer(name) == tileedit::EditResult::Changed) {
            layerIndex = count;
        }
        ImGui::EndDisabled();
        return;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(count <= 1);
    if (ImGui::Button(Tr(StrId::TilePal_LayerRemove))) {
        if (model.RemoveLayer(layerIndex) == tileedit::EditResult::Changed) {
            layerIndex = (std::min)(layerIndex, count - 2);
        }
        ImGui::EndDisabled();
        return;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(layerIndex <= 0);
    if (ImGui::Button(Tr(StrId::TilePal_LayerUp))) {
        if (model.MoveLayer(layerIndex, -1) == tileedit::EditResult::Changed) {
            --layerIndex;
        }
        ImGui::EndDisabled();
        return;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(layerIndex + 1 >= count);
    if (ImGui::Button(Tr(StrId::TilePal_LayerDown))) {
        if (model.MoveLayer(layerIndex, 1) == tileedit::EditResult::Changed) {
            ++layerIndex;
        }
        ImGui::EndDisabled();
        return;
    }
    ImGui::EndDisabled();

    const std::vector<TilemapLayer>& layers = model.Asset().layers;
    for (int i = 0; i < count; ++i) {
        const TilemapLayer& layer = layers[static_cast<size_t>(i)];
        const std::string label = std::to_string(i) + ": " + layer.name + "##layer" + std::to_string(i);
        if (ImGui::Selectable(label.c_str(), i == layerIndex)) {
            layerIndex = i;
        }
    }
    WrappedDisabledText(Tr(StrId::TilePal_LayerHelp));
    if (count == 0) {
        return;
    }

    // 選択中のレイヤーの設定。セルは触らないので props だけの一時オブジェクト
    const TilemapLayer& current = layers[static_cast<size_t>(layerIndex)];
    TilemapLayer props;
    props.name = current.name;
    props.sortingLayerId = current.sortingLayerId;
    props.orderInLayer = current.orderInLayer;
    props.collision = current.collision;
    GestureTracker tracker;

    char nameBuf[64];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", props.name.c_str());
    ImGui::PushID("layerprops");
    const bool nameEdited = ImGui::InputText(Tr(StrId::TilePal_LayerName), nameBuf, sizeof(nameBuf));
    tracker.Track(model, nameEdited);
    if (nameEdited) {
        props.name = nameBuf;
    }

    // ソート層は Project Settings の表から名前で選ぶ。表に無い id は黙って潰さず「未定義」を足して選択中にする
    SortingLayers& sorting = SortingLayers::Get();
    sorting.Load(ctx.assetsRoot);
    const char* labels[SortingLayers::kMaxLayers + 1];
    for (int i = 0; i < sorting.Count(); ++i) {
        labels[i] = sorting.At(i).name;
    }
    int labelCount = sorting.Count();
    int sortIndex = sorting.IndexOfId(props.sortingLayerId);
    char undefinedLabel[48];
    if (sortIndex < 0) {
        std::snprintf(undefinedLabel, sizeof(undefinedLabel), "%s %d", Tr(StrId::TilePal_SortingUndefined),
                      props.sortingLayerId);
        labels[labelCount] = undefinedLabel;
        sortIndex = labelCount;
        ++labelCount;
    }
    if (ImGui::Combo(Tr(StrId::TilePal_LayerSorting), &sortIndex, labels, labelCount) && sortIndex < sorting.Count()) {
        props.sortingLayerId = sorting.At(sortIndex).id;
        tracker.Mark();
    }

    const bool orderEdited = ImGui::DragInt(Tr(StrId::TilePal_LayerOrder), &props.orderInLayer, 0.2f);
    tracker.Track(model, orderEdited);
    const bool collisionEdited = ImGui::Checkbox(Tr(StrId::TilePal_LayerCollision), &props.collision);
    tracker.Track(model, collisionEdited);
    ImGui::PopID();

    if (tracker.Changed()) {
        model.SetLayerProps(layerIndex, props);
    }
    tracker.FinishGesture(model);
}

// ---------------------------------------------------------------------------
// パレットのタイル追加 / 削除
// ---------------------------------------------------------------------------

uint32_t DrawPaletteToolbar(TilePaletteEditModel& model, uint32_t selectedTile)
{
    if (!model.IsLoaded()) {
        return 0;
    }
    uint32_t pick = 0;

    if (ImGui::Button(Tr(StrId::TilePal_AddFromSprite))) {
        ImGui::OpenPopup("##tp_addspr");
    }
    if (ImGui::BeginPopup("##tp_addspr")) {
        static ImGuiTextFilter filter;
        filter.Draw("##filter", 200.0f);
        ImGui::BeginChild("##list", ImVec2(260.0f, 240.0f), true);
        if (SpriteLibrary* lib = spritelib::Library()) {
            std::vector<SpriteListing> listing;
            lib->Enumerate(listing);
            for (const SpriteListing& l : listing) {
                if (!filter.PassFilter(l.displayName.c_str())) {
                    continue;
                }
                if (ImGui::Selectable(l.displayName.c_str())) {
                    uint32_t newId = 0;
                    if (model.AddTile(TileNameFromSprite(l.displayName), l.id, &newId)) {
                        pick = newId;
                    }
                    ImGui::CloseCurrentPopup();
                    break; // 追加でパレットが変わった。一覧の描画はここで打ち切る
                }
            }
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }

    if (ImGui::Button(Tr(StrId::TilePal_AddFromTexture))) {
        ImGui::OpenPopup("##tp_addtex");
    }
    if (ImGui::BeginPopup("##tp_addtex")) {
        const std::map<std::string, std::vector<SpriteListing>> byTexture = ListSlicedTextures();
        if (byTexture.empty()) {
            ImGui::TextDisabled("%s", Tr(StrId::TilePal_NoSlices));
        }
        for (const auto& [tex, slices] : byTexture) {
            if (!ImGui::Selectable(tex.c_str())) {
                continue;
            }
            std::vector<std::pair<std::string, AssetID>> entries;
            entries.reserve(slices.size());
            for (const SpriteListing& s : slices) {
                entries.emplace_back(TileNameFromSprite(s.displayName), s.id);
            }
            const uint32_t firstId = model.Palette().NextId();
            if (model.AddTiles(entries) > 0) {
                pick = firstId;
            }
            ImGui::CloseCurrentPopup();
            break;
        }
        ImGui::EndPopup();
    }

    ImGui::BeginDisabled(selectedTile == 0 || model.Palette().Find(selectedTile) == nullptr);
    if (ImGui::Button(Tr(StrId::TilePal_RemoveTile))) {
        model.RemoveTile(selectedTile);
    }
    ImGui::EndDisabled();
    return pick;
}

// ---------------------------------------------------------------------------
// タイル 1 種
// ---------------------------------------------------------------------------

namespace {

// 隣接 8 マスの 3x3 グリッド。クリックで . → x → o → . と回る。中央は「このタイル」
bool DrawNeighborGrid(std::string& neighbors)
{
    if (neighbors.size() != kTileNeighborCount) {
        neighbors.assign(kTileNeighborCount, '.');
    }
    bool changed = false;
    // 行ごとの 8 近傍の添字。-1 は中央
    static constexpr int kCell[3][3] = { { 0, 1, 2 }, { 3, -1, 4 }, { 5, 6, 7 } };
    ImGui::BeginGroup();
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            if (col > 0) {
                ImGui::SameLine(0.0f, 2.0f);
            }
            const int index = kCell[row][col];
            ImGui::PushID(row * 3 + col);
            if (index < 0) {
                ImGui::BeginDisabled();
                ImGui::Button("*", ImVec2(kNeighborButton, kNeighborButton));
                ImGui::EndDisabled();
            } else {
                char& c = neighbors[static_cast<size_t>(index)];
                const bool colored = (c != '.');
                if (colored) {
                    ImGui::PushStyleColor(ImGuiCol_Button, c == 'x' ? themeColor::AccentSoft : themeColor::Warning);
                }
                const char text[2] = { c, '\0' };
                if (ImGui::Button(text, ImVec2(kNeighborButton, kNeighborButton))) {
                    c = (c == '.') ? 'x' : (c == 'x' ? 'o' : '.');
                    changed = true;
                }
                if (colored) {
                    ImGui::PopStyleColor();
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::EndGroup();
    return changed;
}

// 変種のリスト。順序を入れ替えたり消したりした結果は d に直接書く
bool DrawRuleSection(EngineContext& ctx, TileDef& d)
{
    bool changed = false;
    ImGui::TextWrapped("%s", Tr(StrId::TilePal_RuleHelp));
    int swapFrom = -1;
    int swapWith = -1;
    int removeAt = -1;
    for (size_t i = 0; i < d.variants.size(); ++i) {
        TileVariant& v = d.variants[i];
        ImGui::PushID(static_cast<int>(i));
        changed = DrawNeighborGrid(v.neighbors) || changed;
        ImGui::SameLine();
        ImGui::BeginGroup();
        DrawSpriteThumb(ctx, v.sprite, kVariantThumb);
        ImGui::SameLine();
        if (DrawSpritePicker("vsprite", v.sprite, 160.0f)) {
            changed = true;
        }
        if (ImGui::SmallButton("^") && i > 0) {
            swapFrom = static_cast<int>(i);
            swapWith = static_cast<int>(i) - 1;
        }
        if (SameLineSmallButton("v") && i + 1 < d.variants.size()) {
            swapFrom = static_cast<int>(i);
            swapWith = static_cast<int>(i) + 1;
        }
        if (SameLineSmallButton("x")) {
            removeAt = static_cast<int>(i);
        }
        ImGui::EndGroup();
        ImGui::PopID();
        ImGui::Separator();
    }
    if (swapFrom >= 0) {
        std::swap(d.variants[static_cast<size_t>(swapFrom)], d.variants[static_cast<size_t>(swapWith)]);
        changed = true;
    }
    if (removeAt >= 0) {
        d.variants.erase(d.variants.begin() + removeAt);
        changed = true;
    }
    ImGui::BeginDisabled(d.variants.size() >= kMaxVariants);
    if (ImGui::Button(Tr(StrId::TilePal_RuleAdd))) {
        TileVariant v;
        v.sprite = d.sprite;
        d.variants.push_back(std::move(v));
        changed = true;
    }
    ImGui::EndDisabled();
    return changed;
}

bool DrawAnimSection(EngineContext& ctx, TileDef& d, GestureTracker& tracker, TilePaletteEditModel& model)
{
    bool changed = false;
    ImGui::TextWrapped("%s", Tr(StrId::TilePal_AnimHelp));
    const bool ticksEdited = ImGui::DragInt(Tr(StrId::TilePal_AnimTicks), &d.ticksPerFrame, 0.2f, 1, kMaxTicksPerFrameUi);
    tracker.Track(model, ticksEdited);
    int swapFrom = -1;
    int swapWith = -1;
    int removeAt = -1;
    for (size_t i = 0; i < d.animFrames.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        DrawSpriteThumb(ctx, d.animFrames[i], kVariantThumb);
        ImGui::SameLine();
        if (DrawSpritePicker("fsprite", d.animFrames[i], 160.0f)) {
            changed = true;
        }
        if (SameLineSmallButton("^") && i > 0) {
            swapFrom = static_cast<int>(i);
            swapWith = static_cast<int>(i) - 1;
        }
        if (SameLineSmallButton("v") && i + 1 < d.animFrames.size()) {
            swapFrom = static_cast<int>(i);
            swapWith = static_cast<int>(i) + 1;
        }
        if (SameLineSmallButton("x")) {
            removeAt = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    if (swapFrom >= 0) {
        std::swap(d.animFrames[static_cast<size_t>(swapFrom)], d.animFrames[static_cast<size_t>(swapWith)]);
        changed = true;
    }
    if (removeAt >= 0) {
        d.animFrames.erase(d.animFrames.begin() + removeAt);
        changed = true;
    }
    ImGui::BeginDisabled(d.animFrames.size() >= kMaxAnimFrames);
    if (ImGui::Button(Tr(StrId::TilePal_AnimAdd))) {
        d.animFrames.push_back(d.animFrames.empty() ? d.sprite : d.animFrames.back());
        changed = true;
    }
    ImGui::EndDisabled();
    return changed;
}

} // namespace

void DrawTileInspector(EngineContext& ctx, TilePaletteEditModel& model, uint32_t tileId)
{
    const TileDef* current = model.IsLoaded() ? model.Palette().Find(tileId) : nullptr;
    if (current == nullptr) {
        ImGui::TextDisabled("%s", Tr(StrId::TilePal_TileNone));
        return;
    }
    TileDef d = *current;
    GestureTracker tracker;
    ImGui::PushID(static_cast<int>(tileId));

    char nameBuf[64];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", d.name.c_str());
    const bool nameEdited = ImGui::InputText(Tr(StrId::TilePal_TileName), nameBuf, sizeof(nameBuf));
    tracker.Track(model, nameEdited);
    if (nameEdited) {
        d.name = nameBuf;
    }

    DrawSpriteThumb(ctx, d.sprite, kVariantThumb);
    ImGui::SameLine();
    if (DrawSpritePicker("sprite", d.sprite, 200.0f)) {
        tracker.Mark();
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(Tr(StrId::TilePal_TileSprite));

    const char* colliderLabels[2] = { Tr(StrId::TilePal_ColNone), Tr(StrId::TilePal_ColGrid) };
    const bool colliderEdited = ImGui::Combo(Tr(StrId::TilePal_TileCollider), &d.collider, colliderLabels, 2);
    tracker.Track(model, colliderEdited);
    const bool oneWayEdited = ImGui::Checkbox(Tr(StrId::TilePal_TileOneWay), &d.oneWay);
    tracker.Track(model, oneWayEdited);
    const bool hpEdited = ImGui::DragInt(Tr(StrId::TilePal_TileHp), &d.hp, 0.2f, 0, 255);
    tracker.Track(model, hpEdited);

    // 壊れた後のタイル (0 = 空)
    {
        std::string preview = Tr(StrId::Insp_NoneItem);
        if (const TileDef* dest = model.Palette().Find(d.destroyedTile)) {
            preview = "#" + std::to_string(dest->id) + " " + dest->name;
        } else if (d.destroyedTile != 0) {
            preview = "#" + std::to_string(d.destroyedTile) + " ?";
        }
        if (ImGui::BeginCombo(Tr(StrId::TilePal_TileDestroyed), preview.c_str())) {
            if (ImGui::Selectable(Tr(StrId::Insp_NoneItem), d.destroyedTile == 0)) {
                d.destroyedTile = 0;
                tracker.Mark();
            }
            for (const TileDef& t : model.Palette().tiles) {
                const std::string label = "#" + std::to_string(t.id) + " " + t.name;
                if (ImGui::Selectable(label.c_str(), t.id == d.destroyedTile)) {
                    d.destroyedTile = t.id;
                    tracker.Mark();
                }
            }
            ImGui::EndCombo();
        }
    }

    // 破壊エフェクト (プレハブ。0 = 無し)
    if (ctx.prefabs != nullptr) {
        const PrefabAsset* effect = d.destroyEffect.IsNull() ? nullptr : ctx.prefabs->Get(d.destroyEffect.value);
        const std::string preview = d.destroyEffect.IsNull()
                                        ? std::string(Tr(StrId::Insp_NoneItem))
                                        : (effect != nullptr ? effect->name : std::string("?"));
        if (ImGui::BeginCombo(Tr(StrId::TilePal_TileEffect), preview.c_str())) {
            if (ImGui::Selectable(Tr(StrId::Insp_NoneItem), d.destroyEffect.IsNull())) {
                d.destroyEffect = AssetID{};
                tracker.Mark();
            }
            for (const PrefabEntry& p : ctx.prefabs->Enumerate()) {
                if (ImGui::Selectable(p.name.c_str(), p.hash == d.destroyEffect.value)) {
                    d.destroyEffect = AssetID{ p.hash };
                    tracker.Mark();
                }
            }
            ImGui::EndCombo();
        }
    }

    ImGui::Separator();
    const bool ruleToggled = ImGui::Checkbox(Tr(StrId::TilePal_RuleOn), &d.hasRule);
    tracker.Track(model, ruleToggled);
    if (d.hasRule && DrawRuleSection(ctx, d)) {
        tracker.Mark();
    }

    ImGui::Separator();
    const bool animToggled = ImGui::Checkbox(Tr(StrId::TilePal_AnimOn), &d.hasAnim);
    tracker.Track(model, animToggled);
    if (animToggled && d.hasAnim && d.animFrames.empty()) {
        d.animFrames.push_back(d.sprite); // 有効にしたらすぐ 1 コマ見えるように、既定の絵を先頭に置く
    }
    if (d.hasAnim && DrawAnimSection(ctx, d, tracker, model)) {
        tracker.Mark();
    }
    ImGui::PopID();

    if (tracker.Changed()) {
        model.SetTile(d);
    }
    tracker.FinishGesture(model);
}

} // namespace tileui
} // namespace mye
