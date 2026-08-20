#pragma once
#include <cstdint>
#include <string>

#include <d3d11.h>
#include <wrl/client.h>

namespace mye {

// D3D11 デバイス / 即時コンテキストの所有者。
// レイヤ規約: 生の ID3D11* を Renderer 層より上のコードから呼び出してはならない
// (Engine 層はこのオブジェクトを所有・受け渡しするだけ)。
class GraphicsDevice {
public:
    // forceWarp=false: HARDWARE を試し、失敗したら WARP (ソフトウェアラスタライザ) へ
    // 自動フォールバックする。true (--warp): 最初から WARP のみを使う。
    // GPU の無い CI runner でも同じ検証一式が回るようにするための構成値分岐であって、
    // 構成 (Debug/Release) による分岐ではない = spec 11.2 規則 1 に非抵触 (M52b)
    bool Init(bool forceWarp = false);
    void Shutdown();

    ID3D11Device* Device() const { return device_.Get(); }
    ID3D11DeviceContext* Context() const { return context_.Get(); }

    // 実際に採用したアダプタ (診断用)。WARP なら IsWarp() が true
    const std::string& AdapterName() const { return adapterName_; }
    bool IsWarp() const { return warp_; }

    // デバッグレイヤの InfoQueue に溜まった警告/エラーをエンジンログへ転送する
    // (デバッガ非接続でも D3D の検証結果を確認できる)。Release では何もしない
    void PumpDebugMessages();

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    std::string adapterName_;
    uint64_t debugMsgCursor_ = 0;
    bool debugLayer_ = false;
    bool warp_ = false;
};

} // namespace mye
