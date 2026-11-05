//====================================================================================
//                          FeelAssetInspector.h
//  MyEngin/ 秋田蓮音                                                       11/04/2026
//                                          手触りプリセット (.feel.json、M94d、ADR-035) のアセット Inspector
//====================================================================================
#pragma once

#include <cstdint>
#include <string>

#include "Editor/Undo/UndoStack.h"
#include "Shared/FeelMath.h"

namespace mye {

struct EngineContext;

// .feel.json を 1 件、Inspector で編集する。
// 編集は FeelLibrary に登録済みのプリセットへ**直接**入る (M94e) — スライダを動かした直後に鳴らした分から効くので、
// Play しながら値を詰められる。ディスクへは Save でだけ書き、Revert でディスクの値へ戻す。
// 編集は文書エントリ (UndoDocOp::Kind::Feel) として Undo できる。
// Play 中は「プレビュー」で今の値をそのまま鳴らせる (シーン状態の編集として扱う — Play 中に Inspector で値を触るのと同じ)
class FeelAssetInspector {
public:
    // 選択されたパスが変わったときに 1 回呼ぶ (未登録ならライブラリへ読み込む)
    void Load(const std::wstring& path);
    void Draw(EngineContext& ctx, UndoStack& undo, const std::wstring& path);

private:
    void DrawTime(MyeFeelDesc& d);
    void DrawShake(MyeFeelDesc& d);
    void DrawKick(MyeFeelDesc& d);
    void DrawRumble(MyeFeelDesc& d);
    void DrawFlash(MyeFeelDesc& d);

    uint64_t hash_ = 0;   // 編集中のプリセット (FeelLibrary のキー)
    MyeFeelDesc saved_;   // ディスク上の値 (未保存の判定と Revert)
    bool valid_ = false;
    DocUndoRecorder undoRecorder_;
};

} // namespace mye
