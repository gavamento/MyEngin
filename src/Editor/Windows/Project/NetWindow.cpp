#include "Editor/Windows/Project/NetWindow.h"

#include "Engine/Core/Localization/Localization.h"
#include "Engine/Engine/Net/NetRuntime.h"

#include "Engine/Renderer/ImGui/ImGuiTheme.h" // themeColor (意味色)

#include "imgui.h"

#include <Windows.h>
#include <shellapi.h> // ShellExecuteW (自 exe の起動)

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {

namespace {

// NetRole の生値 (NetRuntimeInfo.role)
constexpr int kRoleHost = 1;
constexpr int kRoleServerClient = 4;

// LaneState の生値 (NetRuntimeInfo.laneState)
const char* LaneStateText(uint32_t state)
{
    switch (state) {
    case 1: return Tr(StrId::Net_LaneConnected);
    case 2: return Tr(StrId::Net_LaneReserved);
    default: return Tr(StrId::Net_LaneEmpty);
    }
}

// 専用サーバ構成のクライアントの表示 (役割 / playerId / レーン 4 本 / 確定 tick / 先行量 / 到着余裕 / 再同期 / desync)
void ShowServerClient(const NetRuntimeInfo& n)
{
    ImGui::Text(Tr(StrId::Net_Role), Tr(StrId::Net_RoleServerClient), n.localPlayer, n.playerCount, n.inputDelay);
    ImGui::Text(Tr(StrId::Net_PlayerId), static_cast<unsigned long long>(n.playerId));
    ImGui::Separator();
    for (uint32_t i = 0; i < 4; ++i) {
        ImGui::Text(Tr(StrId::Net_LaneRow), i, i == n.localPlayer ? Tr(StrId::Net_LaneYou) : "",
                    LaneStateText(n.laneState[i]), static_cast<unsigned long long>(n.lanePlayerId[i]));
    }
    ImGui::Separator();
    ImGui::Text(Tr(StrId::Net_ServerTicks), static_cast<unsigned long long>(n.confirmedTick), n.speculation,
                static_cast<double>(n.arrivalMarginMs), static_cast<double>(n.pingMs));
    ImGui::Text(Tr(StrId::Net_Rollback), static_cast<unsigned long long>(n.rollbacks),
                static_cast<unsigned long long>(n.rollbackTicks),
                static_cast<unsigned long long>(n.maxRollbackDepth));
    ImGui::Text(Tr(StrId::Net_ServerSync), static_cast<unsigned long long>(n.resyncs),
                static_cast<unsigned long long>(n.desyncs));
    if (n.desyncs > 0) {
        // サーバ構成は止まらず再同期する。窓は事後の報告先 (黙って飲み込まないための表示)
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Error);
        ImGui::TextWrapped("%s", Tr(StrId::Net_ServerDesyncNote));
        ImGui::PopStyleColor();
    }
    ImGui::Separator();
    ImGui::Text(Tr(StrId::Net_Packets), static_cast<unsigned long long>(n.packetsSent),
                static_cast<unsigned long long>(n.packetsRecv),
                static_cast<unsigned long long>(n.packetsDropped),
                static_cast<unsigned long long>(n.stalls), n.stallMs);
}

} // namespace

std::wstring NetWindow::BuildConnectArgs(const std::wstring& projectRoot, const std::string& hostPort,
                                         const std::string& playerSessionId)
{
    // コマンドラインへそのまま埋めるので、引用符と制御文字・空白は拒否する (引数の分断・注入を防ぐ)
    const auto safe = [](const std::string& s) {
        for (const unsigned char c : s) {
            if (c <= 0x20 || c == '"' || c == 0x7F) {
                return false;
            }
        }
        return true;
    };
    if (hostPort.empty() || !safe(hostPort) || !safe(playerSessionId)) {
        return {};
    }
    // ★--autoplay: Editor は Play 中しか sim を進めないので、接続と同時に Play も始める
    std::wstring args;
    if (!projectRoot.empty()) {
        args += L"--project \"" + projectRoot + L"\" ";
    }
    args += L"--net-connect " + Utf8ToWide(hostPort) + L" --autoplay";
    if (!playerSessionId.empty()) {
        args += L" --player-session-id " + Utf8ToWide(playerSessionId);
    }
    return args;
}

bool NetWindow::LaunchClient(const EngineContext& ctx) const
{
    const std::wstring args = BuildConnectArgs(ctx.projectRoot, hostPort_, playerSessionId_);
    if (args.empty()) {
        MYE_LOG_ERROR("[net] connect: invalid HOST:PORT or player session ID");
        return false;
    }
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const wchar_t* workDir = ctx.projectRoot.empty() ? nullptr : ctx.projectRoot.c_str();
    const HINSTANCE r = ShellExecuteW(nullptr, L"open", exePath, args.c_str(), workDir, SW_SHOWNORMAL);
    const bool ok = reinterpret_cast<intptr_t>(r) > 32;
    if (ok) {
        MYE_LOG_INFO("[net] connect: launched a client editor (%s)", WideToUtf8(args).c_str());
    } else {
        MYE_LOG_ERROR("[net] connect: launch failed (%s)", WideToUtf8(exePath).c_str());
    }
    return ok;
}

void NetWindow::OnImGui(EngineContext& ctx)
{
    if (!open) {
        wasOpen_ = false;
        return;
    }
    // 開いた瞬間は前面へ (起動引数でセッションを張ったとき、他の浮動窓の下に隠れないため)
    if (!wasOpen_) {
        ImGui::SetNextWindowFocus();
    }
    wasOpen_ = true;
    ImGui::SetNextWindowSize(ImVec2(520.0f, 320.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(Tr(StrId::Win_Net), &open)) {
        ImGui::End();
        return;
    }
    const NetRuntimeInfo* n = ctx.net;
    if (n == nullptr || !n->active) {
        ImGui::TextWrapped("%s", Tr(StrId::Net_Inactive));
        // ---- 専用サーバへ接続するクライアントを起動する (別のエディタとして。この窓のセッションは変えない) ----
        ImGui::Separator();
        ImGui::TextWrapped("%s", Tr(StrId::Net_ConnectHeader));
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputText(Tr(StrId::Net_ConnectHost), hostPort_, sizeof(hostPort_));
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputText(Tr(StrId::Net_ConnectSession), playerSessionId_, sizeof(playerSessionId_));
        if (ImGui::Button(Tr(StrId::Net_ConnectButton))) {
            launchResult_ = LaunchClient(ctx) ? 1 : -1;
        }
        if (launchResult_ > 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Success);
            ImGui::TextWrapped("%s", Tr(StrId::Net_ConnectLaunched));
            ImGui::PopStyleColor();
        } else if (launchResult_ < 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Error);
            ImGui::TextWrapped("%s", Tr(StrId::Net_ConnectFailed));
            ImGui::PopStyleColor();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", Tr(StrId::Net_ConnectNote));
        ImGui::PopStyleColor();
        ImGui::End();
        return;
    }
    if (!n->connected) {
        ImGui::TextUnformatted(Tr(StrId::Net_Connecting));
        ImGui::End();
        return;
    }

    if (n->role == kRoleServerClient) {
        ShowServerClient(*n);
        ImGui::End();
        return;
    }

    ImGui::Text(Tr(StrId::Net_Role), n->role == kRoleHost ? Tr(StrId::Net_RoleHost) : Tr(StrId::Net_RoleJoin),
                n->localPlayer, n->playerCount, n->inputDelay);
    ImGui::Text(Tr(StrId::Net_Ping), static_cast<double>(n->pingMs));
    ImGui::Separator();

    if (n->rollbackEnabled) {
        ImGui::Text(Tr(StrId::Net_Rollback), static_cast<unsigned long long>(n->rollbacks),
                    static_cast<unsigned long long>(n->rollbackTicks),
                    static_cast<unsigned long long>(n->maxRollbackDepth));
        ImGui::Text(Tr(StrId::Net_Predicted), static_cast<unsigned long long>(n->predictedTicks),
                    n->speculation);
    } else {
        ImGui::TextUnformatted(Tr(StrId::Net_RollbackOff));
    }
    ImGui::Separator();

    ImGui::Text(Tr(StrId::Net_Confirmed), static_cast<unsigned long long>(n->confirmedTick),
                static_cast<unsigned long long>(n->localHash),
                static_cast<unsigned long long>(n->peerHash),
                static_cast<unsigned long long>(n->peerTick));
    ImGui::TextUnformatted(Tr(StrId::Net_HashNote));
    if (n->desync) {
        // ★ここへ来た時点でセッションは既に止まっている (--net-no-halt-on-desync
        //   でない限り)。窓は事後の報告先であって、判断はエンジン側で済んでいる
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Error);
        ImGui::Text(Tr(StrId::Net_Desync), static_cast<unsigned long long>(n->desyncTick));
        ImGui::PopStyleColor();
    }
    ImGui::Separator();
    ImGui::Text(Tr(StrId::Net_Packets), static_cast<unsigned long long>(n->packetsSent),
                static_cast<unsigned long long>(n->packetsRecv),
                static_cast<unsigned long long>(n->packetsDropped),
                static_cast<unsigned long long>(n->stalls), n->stallMs);
    ImGui::End();
}

} // namespace mye
