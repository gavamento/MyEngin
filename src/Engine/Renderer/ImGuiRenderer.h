#pragma once
#include <string>

namespace mye {

class GraphicsDevice;
class Win32Window;

// ImGui 初期化オプション (M26)
struct ImGuiInitOptions {
    std::wstring iniPath;    // 空 = ImGui 既定 (CWD 相対の imgui.ini)。非空 = 絶対パスに固定
    bool disableIni = false; // true = レイアウト永続化なし (プロジェクトマネージャ画面用)
    // モニタ DPI 倍率 (テーマ第 3 世代)。1.0 超でフォントとスタイル寸法を実寸へ拡大する。
    // ★--screenshot 中は呼び出し側が 1.0 に固定すること — 撮影サイズは論理ピクセルで
    //   固定されており、撮った機械の DPI で絵が変わると決定的スクショにならない
    float dpiScale = 1.0f;
};

// Dear ImGui の Win32/DX11 バックエンドをラップする。
// レイヤ規約: imgui_impl_* (生の D3D 型を触るコード) を Renderer 層に閉じ込めるための壁。
// Editor 層は imgui.h (描画 API 非依存) のみを使う。
class ImGuiRenderer {
public:
    bool Init(Win32Window& window, GraphicsDevice& device, const ImGuiInitOptions& opts = {});
    void Shutdown();

    void BeginFrame();
    void EndFrame(); // ImGui::Render + RenderDrawData (呼び出し前に RTV をバインドしておくこと)

private:
    bool initialized_ = false;
    // io.IniFilename は文字列をコピーせずポインタを保持するだけなので、寿命をここで保証する
    std::string iniPathUtf8_;
};

} // namespace mye
