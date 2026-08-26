#include "Editor/Windows/NetWindow.h"

#include "Engine/Core/Localization.h"
#include "Engine/Engine/Net/NetRuntime.h"

#include "Engine/Renderer/ImGuiTheme.h" // themeColor (意味色)

#include "imgui.h"

namespace mye {

void NetWindow::OnImGui(EngineContext& ctx)
{
    if (!open) {
        return;
    }
    if (!ImGui::Begin(Tr(StrId::Win_Net), &open)) {
        ImGui::End();
        return;
    }
    const NetRuntimeInfo* n = ctx.net;
    if (n == nullptr || !n->active) {
        ImGui::TextUnformatted(Tr(StrId::Net_Inactive));
        ImGui::End();
        return;
    }
    if (!n->connected) {
        ImGui::TextUnformatted(Tr(StrId::Net_Connecting));
        ImGui::End();
        return;
    }

    ImGui::Text(Tr(StrId::Net_Role), n->role == 1 ? Tr(StrId::Net_RoleHost) : Tr(StrId::Net_RoleJoin),
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
