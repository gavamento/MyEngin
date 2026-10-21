//====================================================================================
//                          SpriteShapeEditor.h
//  MyEngin/ 秋田蓮音                                                       10/22/2026
//                                  スプライトシェイプのプロファイル (.spriteshape.json) の保存と Inspector 編集 UI
//====================================================================================
#pragma once
#include <string>

#include "Engine/Engine/Rendering/Sprite2D/SpriteShape.h"

namespace mye {

struct EngineContext;

// プロファイルをファイルへ書き、同じ GUID のまま登録し直す。書く前に Sanitize する。失敗時 false
bool SaveSpriteShape(const std::wstring& path, SpriteShapeProfile& profile);

// Inspector の .spriteshape.json 編集の作業領域 (選択が変わるたびに LoadSpriteShapeEdit で読み直す)
struct SpriteShapeEditState {
    SpriteShapeProfile profile;
    bool valid = false;
};

void LoadSpriteShapeEdit(const std::wstring& path, SpriteShapeEditState& state);

// 塗りのスプライト、縁の厚み、縁の一覧 (角度範囲とスプライト、並べ替え・削除・追加)、保存 / 戻す
void DrawSpriteShapeInspector(EngineContext& ctx, const std::wstring& path, SpriteShapeEditState& state);

} // namespace mye
