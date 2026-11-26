//====================================================================================
//                          LocalHosting.h
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          ローカル実行用のホスティング (即開始・認証なし)
//====================================================================================
#pragma once
#include "Server/Hosting/IHostingProvider.h"

namespace mye {

// 開発・CI 用。外部サービスを使わない: NotifyReady の直後に StartSession を 1 回返し、認証は常に通す。
// 停止要求 (Ctrl+C / --stop-event) はホスティングの種類によらず StopRequest (ServerLoop) が受ける
class LocalHosting final : public IHostingProvider {
public:
    const char* Name() const override { return "local"; }
    bool Init() override;
    bool NotifyReady(uint16_t port, const std::vector<std::wstring>& logPaths) override;
    void Poll(std::vector<HostingEvent>& out) override;
    bool ValidatePlayer(const char*) override { return true; }
    void PlayerLeft(const char*) override {}
    void NotifySessionEnded() override {}
    void Shutdown() override {}

private:
    bool ready_ = false;
    bool startSent_ = false;
};

} // namespace mye
