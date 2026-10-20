//====================================================================================
//                          Shape2DEditor.h
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          2D 形状 (.shape2d.json) の保存と Inspector 編集 UI
//====================================================================================
#pragma once
#include <string>

#include "Engine/Engine/Physics/Collider/Shape2DLibrary.h"

namespace mye {

struct EngineContext;

// 形状をファイルへ書き、同じ GUID のまま登録し直す (Collider は GUID で引くので再解決は要らない)。
// 書く前に Sanitize する。失敗時 false
bool SaveShape2D(const std::wstring& path, Shape2D& shape);

// Inspector の .shape2d.json 編集の作業領域 (選択が変わるたびに LoadShape2DEdit で読み直す)
struct Shape2DEditState {
    Shape2D shape;
    bool valid = false;
};

void LoadShape2DEdit(const std::wstring& path, Shape2DEditState& state);

// 点の一覧 (座標の編集・削除・追加)、閉じる / 半厚 / 厚み、保存 / 戻す
void DrawShape2DInspector(EngineContext& ctx, const std::wstring& path, Shape2DEditState& state);

} // namespace mye
