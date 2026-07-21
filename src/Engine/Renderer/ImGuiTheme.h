#pragma once

struct ImGuiStyle;
struct ImVec4;

namespace mye {

// エディタ共通のアクセントカラー (M27a)。
// StatusBar / Toolbar / Play 視覚化などテーマに追従したい箇所が参照する
namespace themeColor {
extern const ImVec4 Accent;     // 選択・ハイライト (青)
extern const ImVec4 PlayAccent; // Play 中の状態表示 (オレンジ)
} // namespace themeColor

// UE5 風テーマ: ほぼ黒のフラット暗色 + 白文字 + 控えめな丸み + 青アクセント
void ApplyEditorTheme(ImGuiStyle& style);

// エディタフォント構築 (M27a):
//   ベース   = Segoe UI (Windows 標準の欧文)
//   マージ 1 = 日本語 (YuGothM.ttc → meiryo.ttc → msgothic.ttc の順にフォールバック)
//   マージ 2 = Font Awesome 6 Solid (external/fontawesome/ の埋め込み配列、ICON_FA_*)
// 全て失敗しても ImGui 既定フォントで続行する (戻り値 false + WARN ログ)。
// ImGui::CreateContext() 後・初回 BeginFrame 前に 1 回だけ呼ぶこと
bool SetupEditorFonts(float sizePx = 16.0f);

} // namespace mye
