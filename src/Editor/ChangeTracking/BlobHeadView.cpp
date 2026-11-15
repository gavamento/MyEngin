//====================================================================================
//                          BlobHeadView.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          大きいファイルの Before / After の先頭を blob_get で取って見せる
//====================================================================================
#include "Editor/ChangeTracking/BlobHeadView.h"

#include "Editor/ChangeTracking/TrackClient.h"
#include "Editor/ChangeTracking/TrackProtocol.h"

namespace mye {

std::string BlobHeadView::Base64Decode(const std::string& encoded, bool* ok)
{
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z') {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9') {
            return c - '0' + 52;
        }
        if (c == '+') {
            return 62;
        }
        if (c == '/') {
            return 63;
        }
        return -1;
    };
    std::string out;
    out.reserve(encoded.size() / 4 * 3);
    uint32_t buffer = 0;
    int bits = 0;
    bool good = true;
    for (const char c : encoded) {
        if (c == '=') {
            break;
        }
        const int v = value(c);
        if (v < 0) {
            good = false;
            break;
        }
        buffer = (buffer << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    if (ok != nullptr) {
        *ok = good;
    }
    return good ? out : std::string();
}

void BlobHeadView::Clear()
{
    ++generation_;
    revisionId_ = 0;
    before_ = Side();
    after_ = Side();
}

void BlobHeadView::Request(TrackClient& client, const RestoreRevision& revision)
{
    if (revision.revisionId == revisionId_ || !client.CanRequest()) {
        return;
    }
    Clear();
    revisionId_ = revision.revisionId;
    RequestSide(client, revision.before, true);
    RequestSide(client, revision.after, false);
}

void BlobHeadView::RequestSide(TrackClient& client, const RestoreSide& source, bool isBefore)
{
    Side& side = isBefore ? before_ : after_;
    if (source.state != RestoreBlob::Present || source.hash.empty()) {
        if (source.state != RestoreBlob::NoSnapshot) {
            side.state = State::Failed; // 内容が無い (欠けている・除外)
        }
        return;
    }
    side.state = State::Loading;
    const uint64_t generation = generation_;
    client.Request(trackop::kBlobGet, { { "hash", source.hash }, { "offset", 0 }, { "length", kBlobHeadBytes } },
                   [this, generation, isBefore](const nlohmann::json& response) {
                       if (generation != generation_) {
                           return; // 消した・取り直したあとに届いた古い応答
                       }
                       Side& target = isBefore ? before_ : after_;
                       const bool ok = response.is_object() && response.contains("ok") && response["ok"].is_boolean() && response["ok"].get<bool>()
                                       && response.contains("result") && response["result"].is_object() && response["result"].contains("data")
                                       && response["result"]["data"].is_string();
                       bool decoded = false;
                       if (ok) {
                           target.text = Base64Decode(response["result"]["data"].get<std::string>(), &decoded);
                       }
                       if (!ok || !decoded) {
                           target = Side();
                           target.state = State::Failed;
                           return;
                       }
                       const nlohmann::json& result = response["result"];
                       target.total = result.contains("size") && result["size"].is_number_integer() ? result["size"].get<int64_t>() : static_cast<int64_t>(target.text.size());
                       target.shown = static_cast<int64_t>(target.text.size());
                       target.binary = target.text.find('\0') != std::string::npos;
                       if (target.binary) {
                           target.text.clear();
                       }
                       target.state = State::Ready;
                   });
}

} // namespace mye
