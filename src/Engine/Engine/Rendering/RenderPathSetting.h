//====================================================================================
//                          RenderPathSetting.h
//  MyEngin/ 秋田蓮音                                                       11/25/2026
//                                          起動時のレンダーパス (project_settings.json) の読み書き
//====================================================================================
#pragma once
#include <string>

namespace mye {

// `assets\project_settings.json` の `"rendering"."renderPath"` ("forward" | "deferred")。
// 描画専用 (sim・ハッシュに触れない)。切り替え口は Project Settings > レンダリングだけ
enum class RenderPathKind { Forward, Deferred };

// ファイルが無い・読めない・キーが無い = Forward。文字列以外・未知の値はログへ 1 行出して Forward
RenderPathKind LoadRenderPathSetting(const std::wstring& assetsRoot);

// renderPath だけを差し替えて書く (他のキー・他の節は残す。読めないファイルは上書きせず false)
bool SaveRenderPathSetting(const std::wstring& assetsRoot, RenderPathKind kind);

} // namespace mye
