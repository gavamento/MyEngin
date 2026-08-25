#pragma once

struct ImFont;
struct ImGuiStyle;
struct ImVec4;

namespace mye {

// エディタ共通の役割カラー (M27a、テーマ第 3 世代で体系化)。
//
// ---- 配色ルール ----
// 1. 原色を使わない: UI に置く有彩色は彩度 0.35〜0.60・明度 0.65〜0.85 の帯から取る
//    (暗背景で刺さらず、かつ区別は付く帯。純色 RGB / S=1 / V=1 は禁止)
// 2. ImVec4 リテラルでの着色は禁止 — 必ずこの表を経由する。同じ「意味」は同じトークン
//    (かつて Warn 系だけで 4 通りの黄色が散っていた再発防止)
// 3. Accent (青) は「選択・フォーカス・トグル ON」専用。状態の意味色
//    (Success/Warning/Error/PlayAccent) と混用しない
// 4. コンポーネントのカテゴリ色は EditorComponentCatalog::ComponentCategoryColor が持つ
//    (カテゴリキーはエディタ層の概念のため)。色はルール 1 と同じ帯から選ぶ
// 5. 例外: 3D ビューポート内の描画 (ギズモの軸 RGB・選択アウトラインのオレンジ・
//    物理デバッグ線) は「3D コンテンツの上での視認性」が優先で、このルールの対象外
namespace themeColor {
extern const ImVec4 Accent;     // 選択・ハイライト・トグル ON (青)
extern const ImVec4 AccentSoft; // トグル ON のボタン面 (Accent より暗い面色。文字が読める)
extern const ImVec4 PlayAccent; // Play 中 + 「別モード」系ツール ON (地形ブラシ/カメラ操縦)
extern const ImVec4 Success;    // 成功・PASS
extern const ImVec4 Warning;    // 警告・注意 (ログ Warn / バージョン不一致 等)
extern const ImVec4 Error;      // エラー・失敗 (ログ Error / NG / desync 等)
extern const ImVec4 Prefab;     // プレハブ由来の印 (Unity 風の淡青。Accent とは別物)
} // namespace themeColor

// テーマ第 3 世代: 3 段の暗色面 (最奥 < パネル < ポップアップ) + 沈み込む入力欄 +
// 白半透明のホバー + 青アクセント。M27a の UE5 風 (灰色の浮き上がる入力欄) を置き換えた
void ApplyEditorTheme(ImGuiStyle& style);

// エディタフォント構築 (M27a):
//   ベース   = Segoe UI (Windows 標準の欧文)
//   マージ 1 = 日本語 (YuGothM.ttc → meiryo.ttc → msgothic.ttc の順にフォールバック)
//   マージ 2 = Font Awesome 6 Solid (external/fontawesome/ の埋め込み配列、ICON_FA_*)
// 全て失敗しても ImGui 既定フォントで続行する (WARN ログ)。
// ImGui::CreateContext() 後・初回 BeginFrame 前に 1 回だけ呼ぶこと。
//
// **戻り値 = 日本語グリフが描けるか** (M47a)。false のまま UI 言語を日本語にすると
// 画面が豆腐だらけになるため、呼び出し側は SetLanguage(Lang::En) へ落とすこと。
// アイコンフォントの成否は戻り値に含めない (欠けても文字は読める)
bool SetupEditorFonts(float sizePx = 16.0f);

// 見出しフォント (テーマ第 3 世代): Segoe UI Semibold + 日本語 Bold + アイコンのマージ。
// null = Semibold が無い環境 (そのまま PushFont(nullptr, size) に渡せばサイズ差だけになる)。
// 使い方: ImGui::PushFont(EditorHeadingFont(), ImGui::GetStyle().FontSizeBase * 1.1f);
// ★GetFontSize() を渡さないこと — あれは DPI スケール適用後の値で二重スケールになる
ImFont* EditorHeadingFont();

} // namespace mye
