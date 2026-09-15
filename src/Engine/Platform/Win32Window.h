#pragma once
#include <cstdint>
#include <functional>
#include <vector>

namespace mye {

// 表示モード (ABI v19 の MyeWindowMode と同値)。排他フルスクリーンは持たない —
// SwapChain が Alt+Enter の遷移を切っているのと同じ「ボーダーレスで扱う」方針
enum class WindowMode : int32_t {
    Windowed = 0,   // 枠付き・サイズ変更可 (WS_OVERLAPPEDWINDOW)
    Borderless = 1, // 枠なしでウィンドウのあるモニタ全面を覆う (WS_POPUP)
};

struct WindowDesc {
    const wchar_t* title = L"MyEngine";
    int width = 1600; // ウィンドウ時のクライアント寸法 (ボーダーレスから戻るときの大きさにもなる)
    int height = 900;
    WindowMode mode = WindowMode::Windowed; // 最初に見せる前に当てる (起動時のちらつきを出さない)
};

// Win32 ウィンドウ。生の HWND / windows.h をヘッダから隠蔽する (void* 経由)。
class Win32Window {
public:
    // 戻り値 true = メッセージを消費した (result を LRESULT として返す)
    using MsgHandler =
        std::function<bool(void* hwnd, uint32_t msg, uint64_t wparam, int64_t lparam, int64_t& result)>;

    bool Create(const WindowDesc& desc);
    void Destroy();

    // 登録順に呼ばれる。ImGui → Input の順で登録する想定
    void AddMsgHandler(MsgHandler handler);

    // 保留メッセージを全処理。WM_QUIT を受けたら false
    bool PumpMessages();

    void* Hwnd() const { return hwnd_; }
    int Width() const { return width_; }
    int Height() const { return height_; }
    bool IsMinimized() const { return minimized_; }
    // モニタ DPI 倍率 (1.0 = 96dpi)。UI スケール用 — sim から読んではいけない (機種依存値)
    float DpiScale() const;
    // 自分がフォアグラウンドウィンドウか (M51h: パッド振動を裏で鳴らさないためのゲート。
    // 出力レーン専用 — sim からこの値を読んではいけない。決定論の対象外)
    bool HasFocus() const;

    // 前回呼び出し以降にクライアント領域サイズが変わっていたら true (1 回で消費)
    bool ConsumeResize();

    // 表示モードの切り替え。WM_SIZE が同期で届くので、スワップチェーンは次フレームの
    // ConsumeResize で追従する。ボーダーレスへ入る直前のウィンドウ位置・大きさ・最大化を覚えておき、
    // ウィンドウへ戻すときに復元する
    void SetMode(WindowMode mode);
    WindowMode Mode() const { return mode_; }

    // hwnd は WndProc の引数をそのまま受ける。CreateWindowExW 中 (hwnd_ 代入前) にも
    // WM_NCCALCSIZE 等が届くため、hwnd_ ではなくこの引数を DefWindowProc へ渡す
    intptr_t HandleMsg(void* hwnd, uint32_t msg, uint64_t wparam, int64_t lparam);

private:
    void* hwnd_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    bool minimized_ = false;
    bool resized_ = false;
    WindowMode mode_ = WindowMode::Windowed;
    // ボーダーレスへ入る前のウィンドウ配置 (WINDOWPLACEMENT の rcNormalPosition = ワークスペース座標)
    int32_t windowedRect_[4] = { 0, 0, 0, 0 }; // left, top, right, bottom
    bool windowedMaximized_ = false;
    bool hasWindowedRect_ = false;
    std::vector<MsgHandler> handlers_;
};

} // namespace mye
