//====================================================================================
//                          OnlineLobbyWindow.cpp
//  MyEngin/ 秋田蓮音                                                     10/14/2026
//                                          Runtime のオンラインのロビー窓 (ImGui の小窓)
//====================================================================================
#include "Runtime/OnlineLobbyWindow.h"

#include <cstring>

#include "imgui.h"

#include "Engine/Core/Localization/Localization.h"
#include "Engine/Engine/Online/OnlineSession.h"

namespace mye {

namespace {

void DrawMembers(IOnlineService& service, bool showVoice)
{
    const OnlineLobbyView& lobby = service.Lobby();
    ImGui::Text(Tr(StrId::Online_Members), lobby.memberCount, kOnlineMaxMembers);
    for (uint32_t i = 0; i < lobby.memberCount; ++i) {
        const OnlineMember& m = lobby.members[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::Bullet();
        ImGui::SameLine();
        ImGui::TextUnformatted(m.name[0] != '\0' ? m.name : m.id);
        if (m.isOwner) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", Tr(StrId::Online_OwnerMark));
        }
        if (m.isLocal) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", Tr(StrId::Online_YouMark));
        }
        if (showVoice && m.speaking) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "%s", Tr(StrId::Online_Speaking));
        }
        if (showVoice && !m.isLocal) {
            ImGui::SameLine();
            bool muted = m.mutedByMe;
            if (ImGui::Checkbox(Tr(StrId::Online_Mute), &muted)) {
                service.SetMemberMuted(m.id, muted);
            }
        }
        ImGui::PopID();
    }
}

} // namespace

void DrawOnlineLobbyWindow(OnlineSession& session, OnlineLobbyWindowState& state)
{
    IOnlineService* service = session.Service();
    if (service == nullptr) {
        return;
    }
    ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(Tr(StrId::Online_Window), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::TextDisabled(Tr(StrId::Online_Backend), service->BackendName(),
                        service->LocalId()[0] != '\0' ? service->LocalId() : "-");
    ImGui::Separator();

    const OnlineState login = service->State();
    if (login == OnlineState::LoggingIn) {
        ImGui::TextUnformatted(Tr(StrId::Online_LoggingIn));
    } else if (login != OnlineState::Ready) {
        ImGui::TextWrapped(Tr(StrId::Online_LoginFailed), service->LastError());
    } else {
        switch (session.Phase()) {
        case OnlinePhase::Offline: {
            if (ImGui::Button(Tr(StrId::Online_Host))) {
                session.Host();
            }
            ImGui::SetNextItemWidth(110.0f);
            ImGui::InputText(Tr(StrId::Online_JoinCode), state.joinCode, kOnlineJoinCodeChars + 1,
                             ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank);
            ImGui::SameLine();
            ImGui::BeginDisabled(std::strlen(state.joinCode) != kOnlineJoinCodeChars);
            if (ImGui::Button(Tr(StrId::Online_Join))) {
                session.Join(state.joinCode);
            }
            ImGui::EndDisabled();
            break;
        }
        case OnlinePhase::CreatingLobby:
            ImGui::TextUnformatted(Tr(StrId::Online_Creating));
            break;
        case OnlinePhase::JoiningLobby:
            ImGui::TextUnformatted(Tr(StrId::Online_Joining));
            break;
        case OnlinePhase::InSession: {
            const OnlineLobbyView& lobby = service->Lobby();
            ImGui::Text(Tr(StrId::Online_RoomCode), lobby.joinCode);
            ImGui::SameLine();
            if (ImGui::SmallButton(Tr(StrId::Online_CopyCode))) {
                ImGui::SetClipboardText(lobby.joinCode);
            }
            DrawMembers(*service, session.VoiceEnabled());
            if (session.VoiceEnabled()) {
                if (session.PushToTalk()) {
                    ImGui::TextDisabled(Tr(StrId::Online_PushToTalk), session.PushToTalkKeyName());
                } else {
                    const bool on = service->VoiceSending();
                    if (ImGui::Button(on ? Tr(StrId::Online_MicOn) : Tr(StrId::Online_MicOff))) {
                        service->SetVoiceSending(!on);
                    }
                }
            }
            if (ImGui::Button(Tr(StrId::Online_Leave))) {
                session.RequestLeave();
            }
            break;
        }
        case OnlinePhase::Error:
            ImGui::TextWrapped("%s", session.Message().c_str());
            if (session.WorldEnded()) {
                ImGui::TextWrapped("%s", Tr(StrId::Online_SessionEnded));
            }
            if (ImGui::Button(Tr(StrId::Online_Back))) {
                session.RequestLeave();
            }
            break;
        }
    }
    ImGui::End();
}

} // namespace mye
