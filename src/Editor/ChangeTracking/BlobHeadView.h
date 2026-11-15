//====================================================================================
//                          BlobHeadView.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          大きいファイルの Before / After の先頭を blob_get で取って見せる (宣言)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>

#include "Editor/ChangeTracking/RestorePlan.h"

namespace mye {

class TrackClient;

// 先頭として取るバイト数。行 diff に落とせない大きいファイル (diff_mode = whole、Hunk が oversize) で、
// 両側の中身がまったく見えなくならないための上限 (1 回の blob_get で足りる大きさ)
constexpr int64_t kBlobHeadBytes = 8 * 1024;

// 選んだ Revision の Before / After の先頭 (各 kBlobHeadBytes まで)。要求はサービスの blob_get だけで、ファイルには触れない
class BlobHeadView {
public:
    enum class State : uint8_t { None, Loading, Ready, Failed };
    struct Side {
        State state = State::None;
        std::string text;
        bool binary = false;  // NUL を含む (文字として見せない)
        int64_t total = 0;    // Blob の総バイト数
        int64_t shown = 0;    // 見せているバイト数
    };

    // revision の両側を取りに行く。同じ Revision で呼び直しても取り直さない。接続できなければ何もしない
    void Request(TrackClient& client, const RestoreRevision& revision);
    void Clear();

    int64_t RevisionId() const { return revisionId_; }
    const Side& Before() const { return before_; }
    const Side& After() const { return after_; }

    // base64 (パディングあり) を戻す。不正な文字があれば ok = false
    static std::string Base64Decode(const std::string& encoded, bool* ok);

private:
    void RequestSide(TrackClient& client, const RestoreSide& source, bool isBefore);

    int64_t revisionId_ = 0;
    Side before_;
    Side after_;
    // 取り直した・消したあとに届いた古い応答を捨てる
    uint64_t generation_ = 0;
};

} // namespace mye
