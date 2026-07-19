#pragma once

namespace mye {

class GraphicsDevice;
class Win32Window;

// Dear ImGui の Win32/DX11 バックエンドをラップする。
// レイヤ規約: imgui_impl_* (生の D3D 型を触るコード) を Renderer 層に閉じ込めるための壁。
// Editor 層は imgui.h (描画 API 非依存) のみを使う。
class ImGuiRenderer {
public:
    bool Init(Win32Window& window, GraphicsDevice& device);
    void Shutdown();

    void BeginFrame();
    void EndFrame(); // ImGui::Render + RenderDrawData (呼び出し前に RTV をバインドしておくこと)

private:
    bool initialized_ = false;
};

} // namespace mye
