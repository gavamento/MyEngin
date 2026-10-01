//====================================================================================
//                          NetProtocol.cpp
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          入力確定型サーバ構成のパケット組み立て / 確定 tick レコード
//====================================================================================
#include "Engine/Engine/Net/NetProtocol.h"

#include <algorithm>

namespace mye {

const char* NetResyncReasonName(NetResyncReason r)
{
    switch (r) {
    case NetResyncReason::None: return "none";
    case NetResyncReason::EventGap: return "eventSeq gap";
    case NetResyncReason::Desync: return "checkpoint hash mismatch (desync)";
    case NetResyncReason::BadSnapshot: return "restored world hash differs from the snapshot meta";
    case NetResyncReason::Behind: return "fell out of the server's history";
    }
    return "?";
}

namespace {

bool IsZeroInput(const InputSnapshot& in)
{
    static const InputSnapshot kZero = {};
    return std::memcmp(&in, &kZero, sizeof(InputSnapshot)) == 0;
}

} // namespace

size_t NetTickRecordBytes(const NetConfirmedTick& t)
{
    size_t n = sizeof(NetTickRecordHeader);
    for (uint32_t p = 0; p < kMaxPlayers; ++p) {
        if (!IsZeroInput(t.inputs[p])) {
            n += sizeof(InputSnapshot);
        }
    }
    n += static_cast<size_t>((std::min)(t.sys.eventCount, kMaxSystemEventsPerTick)) * sizeof(SystemEvent);
    return n;
}

void NetWriteTickRecord(const NetConfirmedTick& t, std::vector<uint8_t>& out)
{
    const SystemInputTick sys = NormalizeSystemInput(t.sys);
    NetTickRecordHeader h = {};
    for (uint32_t p = 0; p < kMaxPlayers; ++p) {
        if (!IsZeroInput(t.inputs[p])) {
            h.laneMask = static_cast<uint8_t>(h.laneMask | (1u << p));
        }
    }
    h.eventCount = static_cast<uint8_t>(sys.eventCount);
    const size_t at = out.size();
    out.resize(at + sizeof(h));
    std::memcpy(out.data() + at, &h, sizeof(h));
    for (uint32_t p = 0; p < kMaxPlayers; ++p) {
        if ((h.laneMask & (1u << p)) != 0) {
            const size_t o = out.size();
            out.resize(o + sizeof(InputSnapshot));
            std::memcpy(out.data() + o, &t.inputs[p], sizeof(InputSnapshot));
        }
    }
    for (uint32_t i = 0; i < sys.eventCount; ++i) {
        const size_t o = out.size();
        out.resize(o + sizeof(SystemEvent));
        std::memcpy(out.data() + o, &sys.events[i], sizeof(SystemEvent));
    }
}

size_t NetReadTickRecord(const uint8_t* data, size_t size, uint64_t tick, NetConfirmedTick& out)
{
    if (size < sizeof(NetTickRecordHeader)) {
        return 0;
    }
    NetTickRecordHeader h;
    std::memcpy(&h, data, sizeof(h));
    if (h.eventCount > kMaxSystemEventsPerTick || (h.laneMask >> kMaxPlayers) != 0) {
        return 0;
    }
    size_t at = sizeof(h);
    NetConfirmedTick t;
    t.tick = tick;
    for (uint32_t p = 0; p < kMaxPlayers; ++p) {
        if ((h.laneMask & (1u << p)) != 0) {
            if (size < at + sizeof(InputSnapshot)) {
                return 0;
            }
            std::memcpy(&t.inputs[p], data + at, sizeof(InputSnapshot));
            at += sizeof(InputSnapshot);
        }
    }
    if (size < at + static_cast<size_t>(h.eventCount) * sizeof(SystemEvent)) {
        return 0;
    }
    t.sys.eventCount = h.eventCount;
    for (uint32_t i = 0; i < h.eventCount; ++i) {
        std::memcpy(&t.sys.events[i], data + at, sizeof(SystemEvent));
        at += sizeof(SystemEvent);
    }
    t.sys = NormalizeSystemInput(t.sys);
    out = t;
    return at;
}

bool NetConfirmedTickEqual(const NetConfirmedTick& a, const NetConfirmedTick& b, uint32_t playerCount)
{
    const uint32_t n = (std::min)(playerCount, kMaxPlayers);
    if (std::memcmp(a.inputs, b.inputs, sizeof(InputSnapshot) * n) != 0) {
        return false;
    }
    const SystemInputTick sa = NormalizeSystemInput(a.sys);
    const SystemInputTick sb = NormalizeSystemInput(b.sys);
    return std::memcmp(&sa, &sb, sizeof(SystemInputTick)) == 0;
}

bool NetParseHeader(const uint8_t* data, size_t size, NetPacketHeader& outHeader)
{
    if (size < sizeof(NetPacketHeader)) {
        return false;
    }
    std::memcpy(&outHeader, data, sizeof(NetPacketHeader));
    return outHeader.magic == kNetMagic && outHeader.proto == static_cast<uint16_t>(kNetProtoVersion);
}

void NetBuildPacket(std::vector<uint8_t>& out, NetMsg type, const NetPacketHeader& base,
                    const void* payload, size_t payloadSize, const void* tail, size_t tailSize)
{
    NetPacketHeader h = base;
    h.magic = kNetMagic;
    h.proto = static_cast<uint16_t>(kNetProtoVersion);
    h.type = static_cast<uint16_t>(type);
    out.resize(sizeof(NetPacketHeader) + payloadSize + tailSize);
    std::memcpy(out.data(), &h, sizeof(h));
    if (payloadSize > 0) {
        std::memcpy(out.data() + sizeof(h), payload, payloadSize);
    }
    if (tailSize > 0) {
        std::memcpy(out.data() + sizeof(h) + payloadSize, tail, tailSize);
    }
}

} // namespace mye
