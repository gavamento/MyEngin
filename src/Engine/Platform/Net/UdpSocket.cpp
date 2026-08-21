#include "Engine/Platform/Net/UdpSocket.h"

// winsock2.h は windows.h より**先**に入れる (後だと winsock.h が先に取り込まれて再定義)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>

// SIO_UDP_CONNRESET は SDK のバージョンによって mstcpip.h から見えないことがある
// (この環境の SDK では未定義だった)。値は公開されている固定の ioctl コードなので、
// 見えないときだけ自前で置く — 定義を諦めると下の WSAIoctl ごと消える = ICMP 由来の
// WSAECONNRESET で受信が死ぬ罠 (Open のコメント参照) がそのまま戻ってくる
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#include <cstdio>
#include <cstring>

#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"

#pragma comment(lib, "ws2_32.lib")

namespace mye {
namespace {

// WSAStartup の参照カウント。**エンジンは単一スレッドからしかソケットを触らない**
// ので素の int で足りる (ここをスレッド安全にすると、使ってもいない同時実行を
// 保証しているように読めてしまう)。
int g_wsaRefs = 0;

bool WsaAcquire()
{
    if (g_wsaRefs > 0) {
        ++g_wsaRefs;
        return true;
    }
    WSADATA data = {};
    const int rc = WSAStartup(MAKEWORD(2, 2), &data);
    if (rc != 0) {
        MYE_LOG_ERROR("[net] WSAStartup failed (%d)", rc);
        return false;
    }
    g_wsaRefs = 1;
    return true;
}

void WsaRelease()
{
    if (g_wsaRefs <= 0) {
        return;
    }
    if (--g_wsaRefs == 0) {
        WSACleanup();
    }
}

} // namespace

std::string NetAddressToString(const NetAddress& a)
{
    in_addr in = {};
    in.S_un.S_addr = a.ipv4;
    char ip[INET_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET, &in, ip, sizeof(ip)) == nullptr) {
        std::snprintf(ip, sizeof(ip), "?");
    }
    char out[64] = {};
    std::snprintf(out, sizeof(out), "%s:%u", ip, static_cast<unsigned>(ntohs(a.port)));
    return std::string(out);
}

bool ResolveEndpoint(const std::wstring& hostPort, NetAddress& out)
{
    const size_t colon = hostPort.rfind(L':');
    if (colon == std::wstring::npos || colon + 1 >= hostPort.size()) {
        MYE_LOG_ERROR("[net] bad endpoint '%s' (expected host:port)",
                      WideToUtf8(hostPort).c_str());
        return false;
    }
    const std::string host = WideToUtf8(hostPort.substr(0, colon));
    const std::string port = WideToUtf8(hostPort.substr(colon + 1));
    if (host.empty() || port.empty()) {
        MYE_LOG_ERROR("[net] bad endpoint '%s' (expected host:port)",
                      WideToUtf8(hostPort).c_str());
        return false;
    }
    if (!WsaAcquire()) { // getaddrinfo も Winsock の初期化を要求する
        return false;
    }
    addrinfo hints = {};
    hints.ai_family = AF_INET; // IPv4 限定 (2 人 P2P の範囲)
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    addrinfo* res = nullptr;
    const int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (rc != 0 || res == nullptr) {
        MYE_LOG_ERROR("[net] cannot resolve '%s' (getaddrinfo %d)", WideToUtf8(hostPort).c_str(),
                      rc);
        WsaRelease();
        return false;
    }
    const sockaddr_in* sin = reinterpret_cast<const sockaddr_in*>(res->ai_addr);
    out.ipv4 = sin->sin_addr.S_un.S_addr;
    out.port = sin->sin_port;
    freeaddrinfo(res);
    WsaRelease();
    return true;
}

UdpSocket::~UdpSocket()
{
    Close();
}

bool UdpSocket::Open(uint16_t localPort)
{
    Close();
    if (!WsaAcquire()) {
        return false;
    }
    const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        MYE_LOG_ERROR("[net] socket() failed (%d)", WSAGetLastError());
        WsaRelease();
        return false;
    }
    // ★SIO_UDP_CONNRESET を切る。既定の Windows は「送った先の UDP ポートが閉じている」
    //   ICMP を次の recvfrom へ WSAECONNRESET として返してくる。参加側が先に起動して
    //   ホストへ JOIN を撃つのは正常な流れなので、ここを潰さないと「相手がまだ居ない」
    //   だけで受信が落ちる (ハンドシェイクが確率的に失敗する形で出る)
    BOOL off = FALSE;
    DWORD bytes = 0;
    if (WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &bytes, nullptr, nullptr)
        == SOCKET_ERROR) {
        MYE_LOG_WARN("[net] SIO_UDP_CONNRESET could not be cleared (%d)", WSAGetLastError());
    }
    u_long nonBlocking = 1;
    if (ioctlsocket(s, FIONBIO, &nonBlocking) == SOCKET_ERROR) {
        MYE_LOG_ERROR("[net] ioctlsocket(FIONBIO) failed (%d)", WSAGetLastError());
        closesocket(s);
        WsaRelease();
        return false;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(localPort);
    if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        MYE_LOG_ERROR("[net] bind(port %u) failed (%d)", static_cast<unsigned>(localPort),
                      WSAGetLastError());
        closesocket(s);
        WsaRelease();
        return false;
    }
    // 任意ポート (localPort = 0) で開いた場合、実際に付いた番号をログへ出せるように拾う
    sockaddr_in bound = {};
    int boundLen = sizeof(bound);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0) {
        localPort_ = ntohs(bound.sin_port);
    } else {
        localPort_ = localPort;
    }
    sock_ = static_cast<uintptr_t>(s);
    return true;
}

void UdpSocket::Close()
{
    if (sock_ == kInvalid) {
        return;
    }
    closesocket(static_cast<SOCKET>(sock_));
    sock_ = kInvalid;
    localPort_ = 0;
    WsaRelease();
}

bool UdpSocket::Send(const NetAddress& to, const void* data, size_t size)
{
    if (sock_ == kInvalid || !to.Valid()) {
        return false;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.S_un.S_addr = to.ipv4;
    addr.sin_port = to.port;
    const int sent = sendto(static_cast<SOCKET>(sock_), static_cast<const char*>(data),
                            static_cast<int>(size), 0,
                            reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    if (sent == SOCKET_ERROR) {
        const int err = WSAGetLastError();
        // 送信バッファが一杯 = 落ちたのと同じ扱いでよい (再送機構を持たない設計。
        // 入力は直近 8 tick を毎回冗長送信しているので次のパケットで埋まる)
        if (err != WSAEWOULDBLOCK) {
            MYE_LOG_WARN("[net] sendto failed (%d)", err);
        }
        return false;
    }
    return true;
}

int UdpSocket::Recv(void* buf, size_t cap, NetAddress& from)
{
    if (sock_ == kInvalid) {
        return -1;
    }
    sockaddr_in addr = {};
    int addrLen = sizeof(addr);
    const int got = recvfrom(static_cast<SOCKET>(sock_), static_cast<char*>(buf),
                             static_cast<int>(cap), 0, reinterpret_cast<sockaddr*>(&addr),
                             &addrLen);
    if (got == SOCKET_ERROR) {
        const int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            return 0;
        }
        // WSAEMSGSIZE = 想定より大きいパケット = 別プロトコルの流入。捨てて続ける
        if (err != WSAEMSGSIZE) {
            MYE_LOG_WARN("[net] recvfrom failed (%d)", err);
        }
        return -1;
    }
    from.ipv4 = addr.sin_addr.S_un.S_addr;
    from.port = addr.sin_port;
    return got;
}

} // namespace mye
