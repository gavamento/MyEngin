//====================================================================================
//                          EosOnlineService.cpp
//  MyEngin/ 秋田蓮音                                                     10/14/2026
//                                          Epic Online Services によるオンラインサービス
//====================================================================================
#include "Engine/Engine/Online/Eos/EosOnlineService.h"

// SDK (external\eos-sdk) を置いてビルドしたときだけ中身を持つ。build\Common.props の MyeHasEos を参照
#if defined(MYE_HAS_EOS)

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>

#include <Windows.h>

#include <eos_sdk.h>
#include <eos_logging.h>
#include <eos_connect.h>
#include <eos_lobby.h>
#include <eos_p2p.h>
#include <eos_rtc.h>
#include <eos_rtc_audio.h>
#include <Windows/eos_Windows.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/App/Project.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {

namespace {

// ゲームのパケットを運ぶ P2P のソケット名 (英数字 32 字以内)。相手も同じ名前で受ける
constexpr char kSocketName[] = "mye";
// ロビーの属性: 参加コード (検索の鍵) と、メンバーの表示名
constexpr char kAttrJoinCode[] = "JOINCODE";
constexpr char kAttrName[] = "NAME";
constexpr char kBucketId[] = "myengine";
// RTC (ボイス) の再生に要る XAudio2.9 の再配布 DLL。SDK の ThirdParty から exe の隣へコピーする (BUILD.md)
constexpr wchar_t kXAudioRedist[] = L"xaudio2_9redist.dll";

void CopyText(char* dst, size_t cap, const char* src)
{
    const size_t n = (std::min)(std::strlen(src), cap - 1);
    std::memcpy(dst, src, n);
    dst[n] = '\0';
}

std::string IdString(EOS_ProductUserId id)
{
    char buf[EOS_PRODUCTUSERID_MAX_LENGTH + 1] = {};
    int32_t len = static_cast<int32_t>(sizeof(buf));
    if (id == nullptr || EOS_ProductUserId_ToString(id, buf, &len) != EOS_EResult::EOS_Success) {
        return std::string();
    }
    return std::string(buf);
}

void EOS_CALL OnEosLog(const EOS_LogMessage* msg)
{
    if (msg->Level <= EOS_ELogLevel::EOS_LOG_Error) {
        MYE_LOG_ERROR("[eos] %s: %s", msg->Category, msg->Message);
    } else if (msg->Level <= EOS_ELogLevel::EOS_LOG_Warning) {
        MYE_LOG_WARN("[eos] %s: %s", msg->Category, msg->Message);
    } else {
        MYE_LOG_INFO("[eos] %s: %s", msg->Category, msg->Message);
    }
}

// ★EOS のコールバックは EOS_Platform_Tick を呼んだスレッド (= メインスレッド) で来る。
//   IOnlineService の契約 (全部メインスレッド) のままなので、キューもロックも要らない
class EosOnlineService final : public IOnlineService {
public:
    ~EosOnlineService() override { Shutdown(); }

    bool Init(const OnlineSettings& settings) override
    {
        settings_ = settings;
        displayName_ = settings.displayName;
        if (displayName_.empty()) {
            const char* user = std::getenv("USERNAME");
            displayName_ = user != nullptr ? user : "player";
        }
        if (displayName_.size() > 32) {
            displayName_.resize(32); // Device ID ログインの DisplayName は 32 字まで
        }
        if (settings.productId.empty() || settings.sandboxId.empty() || settings.deploymentId.empty()
            || settings.clientId.empty() || settings.clientSecret.empty()) {
            return FailLogin("online.eos in project_settings.json is incomplete (see docs\\eos-setup.md)");
        }

        EOS_InitializeOptions init = {};
        init.ApiVersion = EOS_INITIALIZE_API_LATEST;
        init.ProductName = "MyEngine";
        init.ProductVersion = kEngineVersion; // 製品の版はエンジンの版 (M98 で統一)
        const EOS_EResult initResult = EOS_Initialize(&init);
        if (initResult != EOS_EResult::EOS_Success && initResult != EOS_EResult::EOS_AlreadyConfigured) {
            return FailLogin(std::string("EOS_Initialize failed: ") + EOS_EResult_ToString(initResult));
        }
        EOS_Logging_SetCallback(&OnEosLog);
        EOS_Logging_SetLogLevel(EOS_ELogCategory::EOS_LC_ALL_CATEGORIES, EOS_ELogLevel::EOS_LOG_Warning);

        // RTC (ボイス) を使うなら XAudio2.9 の再配布 DLL の場所を渡す (無いと RTC が初期化されない)
        const std::string xaudioPath = WideToUtf8(GetExecutableDir() + L"\\" + kXAudioRedist);
        EOS_Windows_RTCOptions winRtc = {};
        winRtc.ApiVersion = EOS_WINDOWS_RTCOPTIONS_API_LATEST;
        winRtc.XAudio29DllPath = xaudioPath.c_str();
        EOS_Platform_RTCOptions rtc = {};
        rtc.ApiVersion = EOS_PLATFORM_RTCOPTIONS_API_LATEST;
        rtc.PlatformSpecificOptions = &winRtc;
        const bool voiceUsable = settings.voiceEnabled && std::filesystem::exists(GetExecutableDir() + L"\\" + kXAudioRedist);
        if (settings.voiceEnabled && !voiceUsable) {
            MYE_LOG_WARN("[online] voice is off: %s is missing next to the exe (see external\\eos-sdk\\BUILD.md)",
                         WideToUtf8(kXAudioRedist).c_str());
        }
        voiceUsable_ = voiceUsable;

        const std::string cacheDir = WideToUtf8(std::filesystem::temp_directory_path().wstring() + L"MyEngineEOS");
        EOS_Platform_Options opts = {};
        opts.ApiVersion = EOS_PLATFORM_OPTIONS_API_LATEST;
        opts.ProductId = settings.productId.c_str();
        opts.SandboxId = settings.sandboxId.c_str();
        opts.DeploymentId = settings.deploymentId.c_str();
        opts.ClientCredentials.ClientId = settings.clientId.c_str();
        opts.ClientCredentials.ClientSecret = settings.clientSecret.c_str();
        opts.bIsServer = EOS_FALSE;
        // オーバーレイは使わない (D3D11 の Present へフックを入れさせない)
        opts.Flags = EOS_PF_DISABLE_OVERLAY | EOS_PF_DISABLE_SOCIAL_OVERLAY;
        opts.CacheDirectory = cacheDir.c_str();
        opts.RTCOptions = voiceUsable ? &rtc : nullptr; // null = この実行では RTC を使わない
        platform_ = EOS_Platform_Create(&opts);
        if (platform_ == nullptr) {
            return FailLogin("EOS_Platform_Create failed (check the product / sandbox / deployment / client ids)");
        }
        connect_ = EOS_Platform_GetConnectInterface(platform_);
        lobby_ = EOS_Platform_GetLobbyInterface(platform_);
        p2p_ = EOS_Platform_GetP2PInterface(platform_);
        if (voiceUsable_) {
            rtcAudio_ = EOS_RTC_GetAudioInterface(EOS_Platform_GetRTCInterface(platform_));
        }
        state_ = OnlineState::LoggingIn;

        EOS_Connect_CreateDeviceIdOptions dev = {};
        dev.ApiVersion = EOS_CONNECT_CREATEDEVICEID_API_LATEST;
        dev.DeviceModel = "PC Windows";
        EOS_Connect_CreateDeviceId(connect_, &dev, this, &EosOnlineService::OnDeviceId);
        MYE_LOG_INFO("[online] EOS: signing in with the device id");
        return true;
    }

    void Tick() override
    {
        if (platform_ != nullptr) {
            EOS_Platform_Tick(platform_);
        }
    }

    void Shutdown() override
    {
        if (platform_ == nullptr) {
            return;
        }
        LeaveLobby();
        EOS_Platform_Tick(platform_); // 退出の要求を送り出す
        RemoveNotifies();
        EOS_Platform_Release(platform_);
        platform_ = nullptr;
        EOS_Shutdown(); // 1 プロセスで 1 回だけ (再初期化はできない)
        state_ = OnlineState::Disabled;
    }

    OnlineState State() const override { return state_; }
    const char* LocalId() const override { return localIdText_.c_str(); }
    const char* LastError() const override { return lastError_.c_str(); }
    const char* BackendName() const override { return "eos"; }

    void CreateLobby(const char* joinCode, uint32_t maxMembers) override
    {
        if (state_ != OnlineState::Ready || view_.state != LobbyState::None) {
            return;
        }
        lastError_.clear();
        pendingCode_ = joinCode;
        view_ = OnlineLobbyView{};
        view_.state = LobbyState::Creating;
        CopyText(view_.joinCode, sizeof(view_.joinCode), joinCode);
        EOS_Lobby_CreateLobbyOptions o = {};
        o.ApiVersion = EOS_LOBBY_CREATELOBBY_API_LATEST;
        o.LocalUserId = localId_;
        o.MaxLobbyMembers = (std::min)(maxMembers, kOnlineMaxMembers);
        o.PermissionLevel = EOS_ELobbyPermissionLevel::EOS_LPL_PUBLICADVERTISED; // コードで検索して入れる
        o.bPresenceEnabled = EOS_FALSE;
        o.bAllowInvites = EOS_TRUE;
        o.BucketId = kBucketId;
        o.bDisableHostMigration = EOS_TRUE; // ホストが抜けたらロビーごと閉じる (サーバはホストの PC にある)
        o.bEnableRTCRoom = voiceUsable_ ? EOS_TRUE : EOS_FALSE; // ロビーの全員が入る無線のボイス
        EOS_Lobby_CreateLobby(lobby_, &o, this, &EosOnlineService::OnLobbyCreated);
    }

    void JoinLobbyByCode(const char* joinCode) override
    {
        if (state_ != OnlineState::Ready || view_.state != LobbyState::None) {
            return;
        }
        lastError_.clear();
        view_ = OnlineLobbyView{};
        view_.state = LobbyState::Searching;
        CopyText(view_.joinCode, sizeof(view_.joinCode), joinCode);
        EOS_Lobby_CreateLobbySearchOptions so = {};
        so.ApiVersion = EOS_LOBBY_CREATELOBBYSEARCH_API_LATEST;
        so.MaxResults = 1;
        if (EOS_Lobby_CreateLobbySearch(lobby_, &so, &search_) != EOS_EResult::EOS_Success) {
            FailLobby("could not start the lobby search");
            return;
        }
        EOS_Lobby_AttributeData attr = {};
        attr.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
        attr.Key = kAttrJoinCode;
        attr.Value.AsUtf8 = view_.joinCode;
        attr.ValueType = EOS_ELobbyAttributeType::EOS_AT_STRING;
        EOS_LobbySearch_SetParameterOptions po = {};
        po.ApiVersion = EOS_LOBBYSEARCH_SETPARAMETER_API_LATEST;
        po.Parameter = &attr;
        po.ComparisonOp = EOS_EComparisonOp::EOS_CO_EQUAL;
        EOS_LobbySearch_SetParameter(search_, &po);
        EOS_LobbySearch_FindOptions fo = {};
        fo.ApiVersion = EOS_LOBBYSEARCH_FIND_API_LATEST;
        fo.LocalUserId = localId_;
        EOS_LobbySearch_Find(search_, &fo, this, &EosOnlineService::OnSearchDone);
    }

    void LeaveLobby() override
    {
        if (!lobbyId_.empty() && lobby_ != nullptr) {
            if (view_.isOwner) {
                EOS_Lobby_DestroyLobbyOptions d = {};
                d.ApiVersion = EOS_LOBBY_DESTROYLOBBY_API_LATEST;
                d.LocalUserId = localId_;
                d.LobbyId = lobbyId_.c_str();
                EOS_Lobby_DestroyLobby(lobby_, &d, nullptr, &EosOnlineService::OnIgnoredDestroy);
            } else {
                EOS_Lobby_LeaveLobbyOptions l = {};
                l.ApiVersion = EOS_LOBBY_LEAVELOBBY_API_LATEST;
                l.LocalUserId = localId_;
                l.LobbyId = lobbyId_.c_str();
                EOS_Lobby_LeaveLobby(lobby_, &l, nullptr, &EosOnlineService::OnIgnoredLeave);
            }
            if (p2p_ != nullptr) {
                EOS_P2P_SocketId sock = Socket();
                EOS_P2P_CloseConnectionsOptions c = {};
                c.ApiVersion = EOS_P2P_CLOSECONNECTIONS_API_LATEST;
                c.LocalUserId = localId_;
                c.SocketId = &sock;
                EOS_P2P_CloseConnections(p2p_, &c);
            }
        }
        RemoveRoomNotify();
        ReleaseSearch();
        lobbyId_.clear();
        roomName_.clear();
        view_ = OnlineLobbyView{};
    }

    const OnlineLobbyView& Lobby() const override { return view_; }

    bool SendPacket(const char* peerId, const void* data, size_t size) override
    {
        if (view_.state != LobbyState::InLobby || size > kOnlineMaxPacket) {
            return false;
        }
        const EOS_ProductUserId peer = EOS_ProductUserId_FromString(peerId);
        if (peer == nullptr) {
            return false;
        }
        EOS_P2P_SocketId sock = Socket();
        EOS_P2P_SendPacketOptions o = {};
        o.ApiVersion = EOS_P2P_SENDPACKET_API_LATEST;
        o.LocalUserId = localId_;
        o.RemoteUserId = peer;
        o.SocketId = &sock;
        o.Channel = 0;
        o.DataLengthBytes = static_cast<uint32_t>(size);
        o.Data = data;
        o.bAllowDelayedDelivery = EOS_TRUE; // 接続の確立前に送ったものも、確立後に届ける (Hello の取りこぼしを減らす)
        o.Reliability = EOS_EPacketReliability::EOS_PR_UnreliableUnordered; // 再送はゲームのプロトコルが持つ
        return EOS_P2P_SendPacket(p2p_, &o) == EOS_EResult::EOS_Success;
    }

    bool ReceivePacket(char (&outPeerId)[kOnlineIdChars], void* buf, size_t capacity, size_t& outSize) override
    {
        if (p2p_ == nullptr || localId_ == nullptr) {
            return false;
        }
        for (;;) {
            EOS_P2P_GetNextReceivedPacketSizeOptions so = {};
            so.ApiVersion = EOS_P2P_GETNEXTRECEIVEDPACKETSIZE_API_LATEST;
            so.LocalUserId = localId_;
            so.RequestedChannel = nullptr;
            uint32_t size = 0;
            if (EOS_P2P_GetNextReceivedPacketSize(p2p_, &so, &size) != EOS_EResult::EOS_Success) {
                return false;
            }
            EOS_P2P_ReceivePacketOptions ro = {};
            ro.ApiVersion = EOS_P2P_RECEIVEPACKET_API_LATEST;
            ro.LocalUserId = localId_;
            ro.MaxDataSizeBytes = static_cast<uint32_t>((std::min<size_t>)(capacity, 0xFFFFFFFFu));
            ro.RequestedChannel = nullptr;
            EOS_ProductUserId peer = nullptr;
            EOS_P2P_SocketId sock = {};
            uint8_t channel = 0;
            uint32_t written = 0;
            if (EOS_P2P_ReceivePacket(p2p_, &ro, &peer, &sock, &channel, buf, &written) != EOS_EResult::EOS_Success) {
                return false;
            }
            const std::string id = IdString(peer);
            // ロビーのメンバー以外からのパケットは捨てる (local 実装と同じ線引き)
            if (std::strcmp(sock.SocketName, kSocketName) != 0 || !IsMember(id.c_str())) {
                continue;
            }
            CopyText(outPeerId, kOnlineIdChars, id.c_str());
            outSize = written;
            return true;
        }
    }

    bool VoiceAvailable() const override { return voiceUsable_; }

    void SetVoiceSending(bool enabled) override
    {
        voiceSending_ = enabled;
        ApplySending();
    }
    bool VoiceSending() const override { return voiceSending_; }

    void SetMemberMuted(const char* memberId, bool muted) override
    {
        for (uint32_t i = 0; i < view_.memberCount; ++i) {
            if (std::strcmp(view_.members[i].id, memberId) == 0) {
                view_.members[i].mutedByMe = muted;
            }
        }
        if (rtcAudio_ == nullptr || roomName_.empty()) {
            return;
        }
        EOS_RTCAudio_UpdateReceivingOptions o = {};
        o.ApiVersion = EOS_RTCAUDIO_UPDATERECEIVING_API_LATEST;
        o.LocalUserId = localId_;
        o.RoomName = roomName_.c_str();
        o.ParticipantId = EOS_ProductUserId_FromString(memberId);
        o.bAudioEnabled = muted ? EOS_FALSE : EOS_TRUE;
        EOS_RTCAudio_UpdateReceiving(rtcAudio_, &o, nullptr, &EosOnlineService::OnIgnoredReceiving);
    }

private:
    // ---- ログイン ----
    static void EOS_CALL OnDeviceId(const EOS_Connect_CreateDeviceIdCallbackInfo* info)
    {
        auto* self = static_cast<EosOnlineService*>(info->ClientData);
        // 2 回目以降の起動ではこの PC の Device ID が既にある (DuplicateNotAllowed) = そのまま使う
        if (info->ResultCode != EOS_EResult::EOS_Success && info->ResultCode != EOS_EResult::EOS_DuplicateNotAllowed) {
            self->FailLogin(std::string("could not create the device id: ") + EOS_EResult_ToString(info->ResultCode));
            return;
        }
        self->Login();
    }

    void Login()
    {
        EOS_Connect_Credentials cred = {};
        cred.ApiVersion = EOS_CONNECT_CREDENTIALS_API_LATEST;
        cred.Token = nullptr;
        cred.Type = EOS_EExternalCredentialType::EOS_ECT_DEVICEID_ACCESS_TOKEN;
        EOS_Connect_UserLoginInfo info = {};
        info.ApiVersion = EOS_CONNECT_USERLOGININFO_API_LATEST;
        info.DisplayName = displayName_.c_str();
        EOS_Connect_LoginOptions o = {};
        o.ApiVersion = EOS_CONNECT_LOGIN_API_LATEST;
        o.Credentials = &cred;
        o.UserLoginInfo = &info;
        EOS_Connect_Login(connect_, &o, this, &EosOnlineService::OnLogin);
    }

    static void EOS_CALL OnLogin(const EOS_Connect_LoginCallbackInfo* info)
    {
        auto* self = static_cast<EosOnlineService*>(info->ClientData);
        if (info->ResultCode == EOS_EResult::EOS_InvalidUser && info->ContinuanceToken != nullptr) {
            // 初回: この Device ID に紐づく Product User がまだ無い → 作る
            EOS_Connect_CreateUserOptions o = {};
            o.ApiVersion = EOS_CONNECT_CREATEUSER_API_LATEST;
            o.ContinuanceToken = info->ContinuanceToken;
            EOS_Connect_CreateUser(self->connect_, &o, self, &EosOnlineService::OnUserCreated);
            return;
        }
        if (info->ResultCode != EOS_EResult::EOS_Success) {
            self->FailLogin(std::string("sign-in failed: ") + EOS_EResult_ToString(info->ResultCode));
            return;
        }
        self->OnLoggedIn(info->LocalUserId);
    }

    static void EOS_CALL OnUserCreated(const EOS_Connect_CreateUserCallbackInfo* info)
    {
        auto* self = static_cast<EosOnlineService*>(info->ClientData);
        if (info->ResultCode != EOS_EResult::EOS_Success) {
            self->FailLogin(std::string("could not create the user: ") + EOS_EResult_ToString(info->ResultCode));
            return;
        }
        self->OnLoggedIn(info->LocalUserId);
    }

    void OnLoggedIn(EOS_ProductUserId id)
    {
        const bool relogin = localId_ != nullptr;
        localId_ = id;
        localIdText_ = IdString(id);
        state_ = OnlineState::Ready;
        if (relogin) {
            return; // 期限切れからの再ログイン: 通知の登録は済んでいる
        }
        MYE_LOG_INFO("[online] EOS: signed in as %s (\"%s\")", localIdText_.c_str(), displayName_.c_str());

        EOS_Connect_AddNotifyAuthExpirationOptions ae = {};
        ae.ApiVersion = EOS_CONNECT_ADDNOTIFYAUTHEXPIRATION_API_LATEST;
        authExpiration_ = EOS_Connect_AddNotifyAuthExpiration(connect_, &ae, this, &EosOnlineService::OnAuthExpiring);

        EOS_P2P_SocketId sock = Socket();
        EOS_P2P_AddNotifyPeerConnectionRequestOptions cr = {};
        cr.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONREQUEST_API_LATEST;
        cr.LocalUserId = localId_;
        cr.SocketId = &sock;
        connectionRequest_ = EOS_P2P_AddNotifyPeerConnectionRequest(p2p_, &cr, this, &EosOnlineService::OnConnectionRequest);

        EOS_Lobby_AddNotifyLobbyMemberStatusReceivedOptions ms = {};
        ms.ApiVersion = EOS_LOBBY_ADDNOTIFYLOBBYMEMBERSTATUSRECEIVED_API_LATEST;
        memberStatus_ = EOS_Lobby_AddNotifyLobbyMemberStatusReceived(lobby_, &ms, this, &EosOnlineService::OnMemberStatus);

        EOS_Lobby_AddNotifyLobbyMemberUpdateReceivedOptions mu = {};
        mu.ApiVersion = EOS_LOBBY_ADDNOTIFYLOBBYMEMBERUPDATERECEIVED_API_LATEST;
        memberUpdate_ = EOS_Lobby_AddNotifyLobbyMemberUpdateReceived(lobby_, &mu, this, &EosOnlineService::OnMemberUpdate);
    }

    static void EOS_CALL OnAuthExpiring(const EOS_Connect_AuthExpirationCallbackInfo* info)
    {
        // Connect のトークンは約 1 時間で切れる。切れる前に同じ Device ID でログインし直す
        static_cast<EosOnlineService*>(info->ClientData)->Login();
    }

    bool FailLogin(const std::string& reason)
    {
        state_ = OnlineState::Failed;
        lastError_ = reason;
        MYE_LOG_WARN("[online] EOS: %s", reason.c_str());
        return false;
    }

    // ---- ロビー ----
    static void EOS_CALL OnLobbyCreated(const EOS_Lobby_CreateLobbyCallbackInfo* info)
    {
        auto* self = static_cast<EosOnlineService*>(info->ClientData);
        if (info->ResultCode != EOS_EResult::EOS_Success) {
            self->FailLobby(std::string("could not create the lobby: ") + EOS_EResult_ToString(info->ResultCode));
            return;
        }
        self->lobbyId_ = info->LobbyId;
        self->view_.isOwner = true;
        // 参加コード (検索の鍵) と自分の表示名を書いてから InLobby にする
        self->UpdateLobbyAttributes(true);
    }

    void UpdateLobbyAttributes(bool withJoinCode)
    {
        EOS_Lobby_UpdateLobbyModificationOptions mo = {};
        mo.ApiVersion = EOS_LOBBY_UPDATELOBBYMODIFICATION_API_LATEST;
        mo.LocalUserId = localId_;
        mo.LobbyId = lobbyId_.c_str();
        EOS_HLobbyModification mod = nullptr;
        if (EOS_Lobby_UpdateLobbyModification(lobby_, &mo, &mod) != EOS_EResult::EOS_Success) {
            FailLobby("could not edit the lobby attributes");
            return;
        }
        if (withJoinCode) {
            EOS_Lobby_AttributeData code = {};
            code.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
            code.Key = kAttrJoinCode;
            code.Value.AsUtf8 = pendingCode_.c_str();
            code.ValueType = EOS_ELobbyAttributeType::EOS_AT_STRING;
            EOS_LobbyModification_AddAttributeOptions ao = {};
            ao.ApiVersion = EOS_LOBBYMODIFICATION_ADDATTRIBUTE_API_LATEST;
            ao.Attribute = &code;
            ao.Visibility = EOS_ELobbyAttributeVisibility::EOS_LAT_PUBLIC;
            EOS_LobbyModification_AddAttribute(mod, &ao);
        }
        EOS_Lobby_AttributeData name = {};
        name.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
        name.Key = kAttrName;
        name.Value.AsUtf8 = displayName_.c_str();
        name.ValueType = EOS_ELobbyAttributeType::EOS_AT_STRING;
        EOS_LobbyModification_AddMemberAttributeOptions mao = {};
        mao.ApiVersion = EOS_LOBBYMODIFICATION_ADDMEMBERATTRIBUTE_API_LATEST;
        mao.Attribute = &name;
        mao.Visibility = EOS_ELobbyAttributeVisibility::EOS_LAT_PUBLIC;
        EOS_LobbyModification_AddMemberAttribute(mod, &mao);
        EOS_Lobby_UpdateLobbyOptions uo = {};
        uo.ApiVersion = EOS_LOBBY_UPDATELOBBY_API_LATEST;
        uo.LobbyModificationHandle = mod;
        EOS_Lobby_UpdateLobby(lobby_, &uo, this, &EosOnlineService::OnLobbyUpdated);
        EOS_LobbyModification_Release(mod);
    }

    static void EOS_CALL OnLobbyUpdated(const EOS_Lobby_UpdateLobbyCallbackInfo* info)
    {
        auto* self = static_cast<EosOnlineService*>(info->ClientData);
        if (info->ResultCode != EOS_EResult::EOS_Success) {
            self->FailLobby(std::string("could not publish the lobby attributes: ") + EOS_EResult_ToString(info->ResultCode));
            return;
        }
        self->EnterLobby();
    }

    static void EOS_CALL OnSearchDone(const EOS_LobbySearch_FindCallbackInfo* info)
    {
        auto* self = static_cast<EosOnlineService*>(info->ClientData);
        if (info->ResultCode != EOS_EResult::EOS_Success && info->ResultCode != EOS_EResult::EOS_NotFound) {
            self->ReleaseSearch();
            self->FailLobby(std::string("the lobby search failed: ") + EOS_EResult_ToString(info->ResultCode));
            return;
        }
        EOS_LobbySearch_GetSearchResultCountOptions co = {};
        co.ApiVersion = EOS_LOBBYSEARCH_GETSEARCHRESULTCOUNT_API_LATEST;
        if (info->ResultCode == EOS_EResult::EOS_NotFound || EOS_LobbySearch_GetSearchResultCount(self->search_, &co) == 0) {
            self->ReleaseSearch();
            self->FailLobby("no room has that join code");
            return;
        }
        EOS_LobbySearch_CopySearchResultByIndexOptions ro = {};
        ro.ApiVersion = EOS_LOBBYSEARCH_COPYSEARCHRESULTBYINDEX_API_LATEST;
        ro.LobbyIndex = 0;
        EOS_HLobbyDetails details = nullptr;
        const EOS_EResult copied = EOS_LobbySearch_CopySearchResultByIndex(self->search_, &ro, &details);
        self->ReleaseSearch();
        if (copied != EOS_EResult::EOS_Success) {
            self->FailLobby("could not read the lobby that was found");
            return;
        }
        self->view_.state = LobbyState::Joining;
        EOS_Lobby_JoinLobbyOptions jo = {};
        jo.ApiVersion = EOS_LOBBY_JOINLOBBY_API_LATEST;
        jo.LobbyDetailsHandle = details;
        jo.LocalUserId = self->localId_;
        jo.bPresenceEnabled = EOS_FALSE;
        EOS_Lobby_JoinLobby(self->lobby_, &jo, self, &EosOnlineService::OnLobbyJoined);
        EOS_LobbyDetails_Release(details);
    }

    static void EOS_CALL OnLobbyJoined(const EOS_Lobby_JoinLobbyCallbackInfo* info)
    {
        auto* self = static_cast<EosOnlineService*>(info->ClientData);
        if (info->ResultCode != EOS_EResult::EOS_Success) {
            self->FailLobby(info->ResultCode == EOS_EResult::EOS_Lobby_TooManyPlayers
                                ? std::string("the room is full")
                                : std::string("could not join the room: ") + EOS_EResult_ToString(info->ResultCode));
            return;
        }
        self->lobbyId_ = info->LobbyId;
        self->view_.isOwner = false;
        self->UpdateLobbyAttributes(false); // 自分の表示名だけ書く (書き終わったら InLobby)
    }

    void EnterLobby()
    {
        if (view_.state == LobbyState::InLobby) {
            RefreshMembers(); // 属性の書き直し (再ログイン後など)
            return;
        }
        view_.state = LobbyState::InLobby;
        RefreshMembers();
        JoinRoom();
        MYE_LOG_INFO("[online] EOS: in lobby %s (%s, owner %s)", view_.joinCode, view_.isOwner ? "host" : "member",
                     view_.ownerId);
    }

    void RefreshMembers()
    {
        EOS_Lobby_CopyLobbyDetailsHandleOptions o = {};
        o.ApiVersion = EOS_LOBBY_COPYLOBBYDETAILSHANDLE_API_LATEST;
        o.LobbyId = lobbyId_.c_str();
        o.LocalUserId = localId_;
        EOS_HLobbyDetails details = nullptr;
        if (EOS_Lobby_CopyLobbyDetailsHandle(lobby_, &o, &details) != EOS_EResult::EOS_Success) {
            return;
        }
        EOS_LobbyDetails_GetLobbyOwnerOptions oo = {};
        oo.ApiVersion = EOS_LOBBYDETAILS_GETLOBBYOWNER_API_LATEST;
        const std::string owner = IdString(EOS_LobbyDetails_GetLobbyOwner(details, &oo));
        CopyText(view_.ownerId, sizeof(view_.ownerId), owner.c_str());
        EOS_LobbyDetails_GetMemberCountOptions co = {};
        co.ApiVersion = EOS_LOBBYDETAILS_GETMEMBERCOUNT_API_LATEST;
        const uint32_t count = (std::min)(EOS_LobbyDetails_GetMemberCount(details, &co), kOnlineMaxMembers);
        const OnlineLobbyView old = view_;
        view_.memberCount = count;
        for (uint32_t i = 0; i < count; ++i) {
            EOS_LobbyDetails_GetMemberByIndexOptions mo = {};
            mo.ApiVersion = EOS_LOBBYDETAILS_GETMEMBERBYINDEX_API_LATEST;
            mo.MemberIndex = i;
            const EOS_ProductUserId member = EOS_LobbyDetails_GetMemberByIndex(details, &mo);
            const std::string id = IdString(member);
            OnlineMember& m = view_.members[i];
            m = OnlineMember{};
            CopyText(m.id, sizeof(m.id), id.c_str());
            m.isOwner = id == owner;
            m.isLocal = id == localIdText_;
            EOS_LobbyDetails_CopyMemberAttributeByKeyOptions ao = {};
            ao.ApiVersion = EOS_LOBBYDETAILS_COPYMEMBERATTRIBUTEBYKEY_API_LATEST;
            ao.TargetUserId = member;
            ao.AttrKey = kAttrName;
            EOS_Lobby_Attribute* attr = nullptr;
            if (EOS_LobbyDetails_CopyMemberAttributeByKey(details, &ao, &attr) == EOS_EResult::EOS_Success && attr != nullptr) {
                if (attr->Data != nullptr && attr->Data->ValueType == EOS_ELobbyAttributeType::EOS_AT_STRING) {
                    CopyText(m.name, sizeof(m.name), attr->Data->Value.AsUtf8);
                }
                EOS_Lobby_Attribute_Release(attr);
            }
            for (uint32_t k = 0; k < old.memberCount; ++k) {
                if (std::strcmp(old.members[k].id, m.id) == 0) {
                    m.mutedByMe = old.members[k].mutedByMe;
                    m.speaking = old.members[k].speaking;
                }
            }
        }
        EOS_LobbyDetails_Release(details);
    }

    static void EOS_CALL OnMemberStatus(const EOS_Lobby_LobbyMemberStatusReceivedCallbackInfo* info)
    {
        auto* self = static_cast<EosOnlineService*>(info->ClientData);
        if (self->lobbyId_.empty() || self->lobbyId_ != info->LobbyId) {
            return;
        }
        const bool aboutMe = IdString(info->TargetUserId) == self->localIdText_;
        if (info->CurrentStatus == EOS_ELobbyMemberStatus::EOS_LMS_CLOSED
            || (aboutMe && info->CurrentStatus == EOS_ELobbyMemberStatus::EOS_LMS_KICKED)) {
            // ホストが閉じた / 追い出された: ロビーはもう無い
            self->lobbyId_.clear();
            self->RemoveRoomNotify();
            self->roomName_.clear();
            self->view_ = OnlineLobbyView{};
            self->FailLobby("the host closed the room");
            return;
        }
        self->RefreshMembers();
    }

    static void EOS_CALL OnMemberUpdate(const EOS_Lobby_LobbyMemberUpdateReceivedCallbackInfo* info)
    {
        auto* self = static_cast<EosOnlineService*>(info->ClientData);
        if (!self->lobbyId_.empty() && self->lobbyId_ == info->LobbyId) {
            self->RefreshMembers(); // 表示名の属性が書かれた
        }
    }

    void FailLobby(const std::string& reason)
    {
        view_.state = LobbyState::Failed;
        lastError_ = reason;
        MYE_LOG_WARN("[online] EOS: %s", reason.c_str());
    }

    void ReleaseSearch()
    {
        if (search_ != nullptr) {
            EOS_LobbySearch_Release(search_);
            search_ = nullptr;
        }
    }

    bool IsMember(const char* id) const
    {
        for (uint32_t i = 0; i < view_.memberCount; ++i) {
            if (std::strcmp(view_.members[i].id, id) == 0) {
                return true;
            }
        }
        return false;
    }

    // ---- P2P ----
    static EOS_P2P_SocketId Socket()
    {
        EOS_P2P_SocketId s = {};
        s.ApiVersion = EOS_P2P_SOCKETID_API_LATEST;
        CopyText(s.SocketName, sizeof(s.SocketName), kSocketName);
        return s;
    }

    static void EOS_CALL OnConnectionRequest(const EOS_P2P_OnIncomingConnectionRequestInfo* info)
    {
        auto* self = static_cast<EosOnlineService*>(info->ClientData);
        // ロビーのメンバーからの接続だけ受ける
        if (!self->IsMember(IdString(info->RemoteUserId).c_str())) {
            return;
        }
        EOS_P2P_SocketId sock = Socket();
        EOS_P2P_AcceptConnectionOptions o = {};
        o.ApiVersion = EOS_P2P_ACCEPTCONNECTION_API_LATEST;
        o.LocalUserId = self->localId_;
        o.RemoteUserId = info->RemoteUserId;
        o.SocketId = &sock;
        EOS_P2P_AcceptConnection(self->p2p_, &o);
    }

    // ---- ボイス (ロビーの RTC ルーム) ----
    void JoinRoom()
    {
        if (rtcAudio_ == nullptr) {
            return;
        }
        char room[256] = {};
        uint32_t len = static_cast<uint32_t>(sizeof(room));
        EOS_Lobby_GetRTCRoomNameOptions o = {};
        o.ApiVersion = EOS_LOBBY_GETRTCROOMNAME_API_LATEST;
        o.LobbyId = lobbyId_.c_str();
        o.LocalUserId = localId_;
        if (EOS_Lobby_GetRTCRoomName(lobby_, &o, room, &len) != EOS_EResult::EOS_Success) {
            MYE_LOG_WARN("[online] EOS: the lobby has no voice room");
            return;
        }
        roomName_ = room;
        EOS_RTCAudio_AddNotifyParticipantUpdatedOptions po = {};
        po.ApiVersion = EOS_RTCAUDIO_ADDNOTIFYPARTICIPANTUPDATED_API_LATEST;
        po.LocalUserId = localId_;
        po.RoomName = roomName_.c_str();
        participantUpdated_ = EOS_RTCAudio_AddNotifyParticipantUpdated(rtcAudio_, &po, this, &EosOnlineService::OnParticipant);
        ApplySending();
    }

    void ApplySending()
    {
        if (rtcAudio_ == nullptr || roomName_.empty()) {
            return;
        }
        EOS_RTCAudio_UpdateSendingOptions o = {};
        o.ApiVersion = EOS_RTCAUDIO_UPDATESENDING_API_LATEST;
        o.LocalUserId = localId_;
        o.RoomName = roomName_.c_str();
        o.AudioStatus = voiceSending_ ? EOS_ERTCAudioStatus::EOS_RTCAS_Enabled : EOS_ERTCAudioStatus::EOS_RTCAS_Disabled;
        EOS_RTCAudio_UpdateSending(rtcAudio_, &o, nullptr, &EosOnlineService::OnIgnoredSending);
    }

    static void EOS_CALL OnParticipant(const EOS_RTCAudio_ParticipantUpdatedCallbackInfo* info)
    {
        auto* self = static_cast<EosOnlineService*>(info->ClientData);
        const std::string id = IdString(info->ParticipantId);
        for (uint32_t i = 0; i < self->view_.memberCount; ++i) {
            if (id == self->view_.members[i].id) {
                self->view_.members[i].speaking = info->bSpeaking == EOS_TRUE;
            }
        }
    }

    void RemoveRoomNotify()
    {
        if (rtcAudio_ != nullptr && participantUpdated_ != EOS_INVALID_NOTIFICATIONID) {
            EOS_RTCAudio_RemoveNotifyParticipantUpdated(rtcAudio_, participantUpdated_);
        }
        participantUpdated_ = EOS_INVALID_NOTIFICATIONID;
    }

    void RemoveNotifies()
    {
        RemoveRoomNotify();
        if (connectionRequest_ != EOS_INVALID_NOTIFICATIONID) {
            EOS_P2P_RemoveNotifyPeerConnectionRequest(p2p_, connectionRequest_);
        }
        if (memberStatus_ != EOS_INVALID_NOTIFICATIONID) {
            EOS_Lobby_RemoveNotifyLobbyMemberStatusReceived(lobby_, memberStatus_);
        }
        if (memberUpdate_ != EOS_INVALID_NOTIFICATIONID) {
            EOS_Lobby_RemoveNotifyLobbyMemberUpdateReceived(lobby_, memberUpdate_);
        }
        if (authExpiration_ != EOS_INVALID_NOTIFICATIONID) {
            EOS_Connect_RemoveNotifyAuthExpiration(connect_, authExpiration_);
        }
        connectionRequest_ = memberStatus_ = memberUpdate_ = authExpiration_ = EOS_INVALID_NOTIFICATIONID;
    }

    // 結果を使わない非同期呼び出しの受け口 (失敗はログだけ)
    static void EOS_CALL OnIgnoredDestroy(const EOS_Lobby_DestroyLobbyCallbackInfo*) {}
    static void EOS_CALL OnIgnoredLeave(const EOS_Lobby_LeaveLobbyCallbackInfo*) {}
    static void EOS_CALL OnIgnoredSending(const EOS_RTCAudio_UpdateSendingCallbackInfo* info)
    {
        if (info->ResultCode != EOS_EResult::EOS_Success) {
            MYE_LOG_WARN("[online] EOS: could not change the microphone state: %s", EOS_EResult_ToString(info->ResultCode));
        }
    }
    static void EOS_CALL OnIgnoredReceiving(const EOS_RTCAudio_UpdateReceivingCallbackInfo* info)
    {
        if (info->ResultCode != EOS_EResult::EOS_Success) {
            MYE_LOG_WARN("[online] EOS: could not change a mute: %s", EOS_EResult_ToString(info->ResultCode));
        }
    }

    OnlineSettings settings_;
    std::string displayName_;
    std::string lastError_;
    std::string localIdText_;
    std::string lobbyId_;
    std::string roomName_;
    std::string pendingCode_;
    OnlineState state_ = OnlineState::Disabled;
    OnlineLobbyView view_ = {};
    bool voiceUsable_ = false;
    bool voiceSending_ = true;

    EOS_HPlatform platform_ = nullptr;
    EOS_HConnect connect_ = nullptr;
    EOS_HLobby lobby_ = nullptr;
    EOS_HP2P p2p_ = nullptr;
    EOS_HRTCAudio rtcAudio_ = nullptr;
    EOS_HLobbySearch search_ = nullptr;
    EOS_ProductUserId localId_ = nullptr;
    EOS_NotificationId connectionRequest_ = EOS_INVALID_NOTIFICATIONID;
    EOS_NotificationId memberStatus_ = EOS_INVALID_NOTIFICATIONID;
    EOS_NotificationId memberUpdate_ = EOS_INVALID_NOTIFICATIONID;
    EOS_NotificationId authExpiration_ = EOS_INVALID_NOTIFICATIONID;
    EOS_NotificationId participantUpdated_ = EOS_INVALID_NOTIFICATIONID;
};

} // namespace

std::unique_ptr<IOnlineService> CreateEosOnlineService()
{
    return std::make_unique<EosOnlineService>();
}

bool EosAvailableInThisBuild()
{
    return true;
}

} // namespace mye

#else // MYE_HAS_EOS

namespace mye {

std::unique_ptr<IOnlineService> CreateEosOnlineService()
{
    return nullptr;
}

bool EosAvailableInThisBuild()
{
    return false;
}

} // namespace mye

#endif // MYE_HAS_EOS
