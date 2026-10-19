//====================================================================================
//                          SpriteEditorSelfTest.h
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          スプライトエディタの編集モデルとスライス純関数の回帰テスト
//====================================================================================
#pragma once

namespace mye {

// スプライトエディタ (M94e) のヘッドレス回帰テスト。ImGui の描画は対象外 (スクリーンショット検証の範囲)。
//   - グリッド (セルサイズ / セル数) と自動 (4 連結成分) が期待の矩形を返す
//   - Delete Existing / Safe、pivot プリセット、Undo / Redo / ジェスチャ / 128 段、Dirty
//   - Save が .meta の sprites だけを差し替え (filter / ppu を保ち)、SpriteLibrary を登録し直す
//   - .meta の重複 id は後勝ち
bool RunSpriteEditorSelfTest();

} // namespace mye
