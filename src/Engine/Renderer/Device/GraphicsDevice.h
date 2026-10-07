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

    // ---- デバイス消失からの復旧 (M88) ----
    // 消失前と同じ種類 (HW / WARP) で作り直す。**WARP へ自動フォールバックしない**
    bool Recreate();
    // 即時コンテキストを手放す (ClearState + Flush の後)。device は残す
    void ReleaseContext();
    // 全所有者が手放した後に、旧デバイスを他者が握っている参照数を返す (0 = 誰も握っていない)。
    // 生の D3D 型を上へ出さないための整数。ReleaseDevice の前に呼ぶこと
    int CountExternalDeviceRefs() const;
    // デバッグレイヤが有効なら生存オブジェクトを詳細にログへ出す (診断のみ。状態は変えない)
    void ReportLiveObjectsDetail();
    void ReleaseDevice();
    // 検証専用 (--simulate-device-lost-stale): 旧デバイスの子を 1 つ握り、参照数ゲートを不合格にする。
    // Shutdown / ReleaseDevice で手放す。成功したら true
    bool HoldChildForTest();

    ID3D11Device* Device() const { return device_.Get(); }
    ID3D11DeviceContext* Context() const { return context_.Get(); }

    // 実際に採用したアダプタ (診断用)。WARP なら IsWarp() が true
    const std::string& AdapterName() const { return adapterName_; }
    bool IsWarp() const { return warp_; }

    // ID3D11Device::GetDeviceRemovedReason の値 (HRESULT)。0 (S_OK) = 消失していない。
    // 生の D3D 型を上へ出さないために整数で返す
    long DeviceRemovedReason() const;

    // デバッグレイヤの InfoQueue に溜まった警告/エラーをエンジンログへ転送する
    // (デバッガ非接続でも D3D の検証結果を確認できる)。Release では何もしない
    void PumpDebugMessages();

private:
    bool CreateDevice(bool tryHardware, bool tryWarp);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> testHeldChild_; // HoldChildForTest の握り分
    std::string adapterName_;
    uint64_t debugMsgCursor_ = 0;
    bool debugLayer_ = false;
    bool warp_ = false;
};

} // namespace mye
