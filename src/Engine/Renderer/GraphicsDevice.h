#pragma once
#include <cstdint>

#include <d3d11.h>
#include <wrl/client.h>

namespace mye {

// D3D11 デバイス / 即時コンテキストの所有者。
// レイヤ規約: 生の ID3D11* を Renderer 層より上のコードから呼び出してはならない
// (Engine 層はこのオブジェクトを所有・受け渡しするだけ)。
class GraphicsDevice {
public:
    bool Init();
    void Shutdown();

    ID3D11Device* Device() const { return device_.Get(); }
    ID3D11DeviceContext* Context() const { return context_.Get(); }

    // デバッグレイヤの InfoQueue に溜まった警告/エラーをエンジンログへ転送する
    // (デバッガ非接続でも D3D の検証結果を確認できる)。Release では何もしない
    void PumpDebugMessages();

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    uint64_t debugMsgCursor_ = 0;
    bool debugLayer_ = false;
};

} // namespace mye
