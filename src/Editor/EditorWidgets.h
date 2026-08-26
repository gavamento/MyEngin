#pragma once

struct ImVec4;

namespace mye {

// ツールバー系の共通ウィジェット (テーマ第 3 世代の統一規格)。
// サイズ・ON 色・区切り・オーバーレイ面はここで一元化する — 個別ウィンドウが
// ad-hoc な ImVec4 や固定高さを持つと、パネルごとに挙動と見た目がズレていくため。

// トグルボタン。ON = themeColor::AccentSoft (選択系) / mode=true なら PlayAccent 系
// (地形ブラシ・カメラ操縦のような「別モードに入る」トグル)。tooltip は null 可
bool ToolbarToggle(const char* label, bool on, const char* tooltip = nullptr, bool mode = false);

// フレーム高に揃えた縦の区切り線。テキスト "|" をベースライン描画する旧方式は
// ボタンと縦位置が揃わないので禁止 (これを使う)
void ToolbarSeparator();

// ビューポート上に浮くオーバーレイバー (SceneView ツールバー / 操縦バナー)。
// 面の色はテーマのポップアップ面から導出し、余白は共通値に固定する。
// tint 非 null で面へ薄く色を混ぜる (操縦中の PlayAccent など)。
// 戻り値に関係なく EndToolbarOverlay を必ず呼ぶこと (BeginChild と同じ規約)
bool BeginToolbarOverlay(const char* id, float height = 0.0f, const ImVec4* tint = nullptr);
void EndToolbarOverlay();

// 直前のアイテム (ラベルを空にした TreeNode / CollapsingHeader) の文字位置へ
// 「色付きアイコン + 通常色ラベル」を描く。ImGui は部分着色ができないため、
// ラベルを "###id" で空にして上から描く方式。アイテム矩形でクリップする。
// framed = CollapsingHeader (枠あり TreeNode)。false = Hierarchy の素の TreeNode
void DrawItemIconLabel(const char* icon, const ImVec4& iconColor, const char* label, bool framed);

} // namespace mye
