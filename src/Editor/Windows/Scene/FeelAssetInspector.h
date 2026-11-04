//====================================================================================
//                          FeelAssetInspector.h
//  MyEngin/ 秋田蓮音                                                       11/04/2026
//                                          手触りプリセット (.feel.json、M94d、ADR-031) のアセット Inspector
//====================================================================================
#pragma once

#include <string>

#include "Shared/FeelMath.h"

namespace mye {

struct EngineContext;

// .feel.json を 1 件、Inspector で編集する。値は編集用の写しに持ち、Save でファイルへ書いて
// FeelLibrary を読み直す (次に鳴らした分から効く)。Play 中は「プレビュー」で編集中の写しをそのまま鳴らせる
// (シーン状態の編集として扱う — Inspector で Play 中に値を触るのと同じ扱い)
class FeelAssetInspector {
public:
    // 選択されたパスが変わったときに 1 回呼ぶ
    void Load(const std::wstring& path);
    void Draw(EngineContext& ctx, const std::wstring& path);

private:
    void DrawTime();
    void DrawShake();
    void DrawKick();
    void DrawRumble();
    void DrawFlash();

    MyeFeelDesc edit_;  // 編集中の写し
    MyeFeelDesc saved_; // ディスク上の値 (未保存の判定)
    bool valid_ = false;
};

} // namespace mye
