#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace mye {

// UDP ソケットの薄いラッパ (M52h)。**Winsock のヘッダをこの .h から出さない** —
// winsock2.h は windows.h より先に入らないと再定義の山になるため、公開側は
// SOCKET を uintptr_t で持つだけの不透明な器にしてある (Win32Window.h と同じ流儀)。
//
// 使うのはネット対戦の入力交換だけで、**sim レーンからは 1 バイトも見えない**
// (決定台帳 5: 「いつ tick が回るか」は非決定論でよいが「tick が何を消費するか」は
// 確定入力だけで決まる)。したがって受信順序もタイミングもハッシュに影響しない。

// IPv4 の宛先。**両フィールドともネットワークバイト順のまま**持つ
// (sockaddr_in へそのまま詰めるため。人間に見せるときだけ NetAddressToString を通す)。
// 2 人 P2P に限定しているので IPv6 は M53 送り (計画「見送り」)。
struct NetAddress {
    uint32_t ipv4 = 0; // in_addr.S_un.S_addr そのもの
    uint16_t port = 0; // sockaddr_in.sin_port そのもの
    bool Valid() const { return port != 0; }
    bool operator==(const NetAddress& o) const { return ipv4 == o.ipv4 && port == o.port; }
    bool operator!=(const NetAddress& o) const { return !(*this == o); }
};

std::string NetAddressToString(const NetAddress& a);

// "host:port" を解決する ("127.0.0.1:7777" / "localhost:7777" / "::1" は非対応)。
// 名前解決は接続時の 1 回だけなので同期で行う (sim レーン外)
bool ResolveEndpoint(const std::wstring& hostPort, NetAddress& out);

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // localPort = 0 で任意ポート (参加側)。常に非ブロッキングで開く
    bool Open(uint16_t localPort);
    void Close();
    bool IsOpen() const { return sock_ != kInvalid; }
    uint16_t LocalPort() const { return localPort_; }

    bool Send(const NetAddress& to, const void* data, size_t size);
    // 戻り値: >0 = 受信バイト数 / 0 = 今は何も無い / <0 = エラー。
    // ★UDP でも「宛先が居ない」ICMP が WSAECONNRESET として recvfrom に返るのが Windows の
    //   既定挙動で、これを素通しすると「相手がまだ起動していないだけ」で受信ループが
    //   死ぬ。Open で SIO_UDP_CONNRESET を切ってあるので、ここには来ない
    int Recv(void* buf, size_t cap, NetAddress& from);

private:
    static constexpr uintptr_t kInvalid = static_cast<uintptr_t>(~0ull);
    uintptr_t sock_ = kInvalid;
    uint16_t localPort_ = 0;
};

} // namespace mye
