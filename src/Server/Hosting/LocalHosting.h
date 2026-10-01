//====================================================================================
//                          LocalHosting.h
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          ローカル実行用のホスティング (即開始・認証なし・Ctrl+C で終了)
//====================================================================================
#pragma once
#include "Server/Hosting/IHostingProvider.h"

namespace mye {

// 開発・CI 用。外部サービスを使わない: NotifyReady の直後に StartSession を 1 回返し、
// 認証は常に通し (ValidatePlayer = true)、Ctrl+C / Ctrl+Break / コンソールを閉じる操作を Terminate として返す。
// コンソールのハンドラはプロセスに 1 つなので、同時に生きる LocalHosting は 1 つだけ
class LocalHosting final : public IHostingProvider {
public:
    ~LocalHosting() override;

    const char* Name() const override { return "local"; }
    bool Init() override;
    bool NotifyReady(uint16_t port, const std::vector<std::wstring>& logPaths) override;
    void Poll(std::vector<HostingEvent>& out) override;
    bool ValidatePlayer(const char*) override { return true; }
    void PlayerLeft(const char*) override {}
    void NotifySessionEnded() override {}
    void Shutdown() override;

private:
    bool handlerInstalled_ = false;
    bool ready_ = false;
    bool startSent_ = false;
    bool terminateSent_ = false;
};

} // namespace mye
