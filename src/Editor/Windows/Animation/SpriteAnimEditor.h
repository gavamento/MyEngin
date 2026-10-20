//====================================================================================
//                          SpriteAnimEditor.h
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                  スプライト列クリップの編集 UI と、スプライト 1 枚を描く小物
//====================================================================================
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "Engine/Core/Ecs/EntityID.h"
#include "Engine/Engine/Animation/SpriteAnim.h"
#include "Engine/Engine/Rendering/Sprite2D/SpriteLibrary.h"

namespace mye {

struct EngineContext;

// スプライト 1 枚を box x box の枠に、縦横比を保って Point サンプリングで描く。枠はカーソル位置に確保する。
// 解決できない・テクスチャが読み込み中なら枠だけ描いて false (読み込みは頼む)
bool DrawSpriteThumb(EngineContext& ctx, AssetID sprite, float box);

// スプライトのピッカー (現在の名前のボタン + 絞り込み付きポップアップ)。選び直したら true。
// 候補は SpriteLibrary の全面 / スライス (表示名の昇順)。id は ImGui の ID 衝突を避けるための文字列
bool DrawSpritePicker(const char* id, AssetID& sprite, float width);

// スプライトの表示名 ("tex" または "tex#name")。空 / 解決できなければ空文字
std::string SpriteDisplayName(AssetID id);

// スライス済みのテクスチャ名 → そのスライス (自然順: walk_2 < walk_10)。全面だけのテクスチャは含めない
std::map<std::string, std::vector<SpriteListing>> ListSlicedTextures();

// Inspector の .spriteanim.json 編集の作業領域 (選択が変わるたびに LoadSpriteAnimEdit で読み直す)
struct SpriteAnimEditState {
    SpriteAnim anim;
    bool valid = false;
    int32_t previewTick = 0;
    bool playing = false;
    float playAccum = 0.0f; // 再生の端数 (tick)
};

void LoadSpriteAnimEdit(const std::wstring& path, SpriteAnimEditState& state);

// コマの追加・削除・並べ替え・ticks・スプライトの選択、ステップ再生のプレビュー、保存 / 戻す
void DrawSpriteAnimInspector(EngineContext& ctx, const std::wstring& path, SpriteAnimEditState& state);

} // namespace mye
