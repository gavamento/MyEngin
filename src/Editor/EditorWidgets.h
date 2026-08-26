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

// ツールバーの折り返し (M47b追補)。器は AutoResizeX で中身に合わせて伸びる一方
// なので、パネルが器より狭いと右端が親にクリップされて**操作不能**になる —
// 区切り (グループ境界) 単位で次の行へ折り返し、全項目を届く位置に保つ。
// ★次グループの幅は描く前に分からないため、**前フレームの実測幅**で改行を決める。
//   リサイズ直後の 1 フレームだけ旧レイアウトで描かれるが目には見えない。
// 使い方: BeginFrame → (器を開く) → FirstGroup → 項目…を ToolbarSeparator の
// 代わりに Separator() で区切る → EndFrame → (器を閉じる)
struct ToolbarFlow {
    static constexpr int kMaxGroups = 16;

    // 描画の前に呼ぶ。前フレームの実測から行数を確定して返す (器の高さ計算用)
    int BeginFrame(float limitWidth);
    // 器内の最初のグループの直前で呼ぶ
    void FirstGroup();
    // ToolbarSeparator の代替: 次グループが収まらないと決めた境界では改行する
    void Separator();
    // 最後のグループの直後 (EndToolbarOverlay の前) に呼ぶ
    void EndFrame();
    // BeginFrame が返した行数 → 器の高さ (BeginToolbarOverlay の既定高の行数拡張)
    static float OverlayHeight(int rows);

private:
    float widths_[kMaxGroups] = {};     // 前フレームの各グループ実測幅 (区切りは含まない)
    bool breakBefore_[kMaxGroups] = {}; // BeginFrame が決めた「このグループの前で改行」
    float sepWidth_ = 16.0f;            // 区切りの実測幅 (初期値は経験値。1 描画で実測へ)
    int groupCount_ = 0;
    int group_ = 0;
    float groupStartX_ = 0.0f;
};

// 直前のアイテム (ラベルを空にした TreeNode / CollapsingHeader) の文字位置へ
// 「色付きアイコン + 通常色ラベル」を描く。ImGui は部分着色ができないため、
// ラベルを "###id" で空にして上から描く方式。アイテム矩形でクリップする。
// framed = CollapsingHeader (枠あり TreeNode)。false = Hierarchy の素の TreeNode
void DrawItemIconLabel(const char* icon, const ImVec4& iconColor, const char* label, bool framed);

} // namespace mye
