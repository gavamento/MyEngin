//====================================================================================
//                          SceneChangeOverlay.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          開いているシーンにかかっている AI の変更の表 (宣言)
//====================================================================================
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nlohmann/json.hpp"

#include "Editor/ChangeTracking/OverlayPalette.h"

namespace mye {

class TrackClient;
class Scene;
struct Selection;

enum class OverlayStatus : uint8_t { None, Added, Modified, Removed };

struct OverlayField {
    std::string name;
    std::string before; // JSON の字句のまま
    std::string after;
    bool hasBefore = false;
    bool hasAfter = false;
    int64_t lastRevisionId = 0;
    std::string changeType;
    bool overrideField = false;
    bool oversize = false;
};

struct OverlayComponent {
    std::string name;
    OverlayStatus status = OverlayStatus::Modified;
    std::vector<OverlayField> fields;
};

struct OverlayEntity {
    int64_t fileId = 0;
    OverlayStatus status = OverlayStatus::None;
    std::string name;
    bool hierarchyChanged = false;
    std::vector<OverlayField> hierarchy;
    std::vector<OverlayField> entityFields;
    std::vector<OverlayComponent> components;
    std::vector<int64_t> revisionIds;
};

struct OverlayCounts {
    int added = 0;
    int modified = 0;
    int removed = 0;
    int Total() const { return added + modified + removed; }
};

// Hierarchy の 1 行に描く印。descendantOnly = 自分には変更が無いが子孫にある
struct OverlayBadge {
    OverlayStatus status = OverlayStatus::None;
    bool descendantOnly = false;
    bool Any() const { return status != OverlayStatus::None || descendantOnly; }
};

// Scene View が 1 フレームに描いたワイヤ箱の数 (AI Changes 窓の帯が上限超えを知らせる)。表示専用
struct OverlayBoxStats {
    int drawn = 0;
    int omitted = 0;
};

// 取り直しの間隔の下限 (通知が続いても、これより短い間隔では取り直さない)
constexpr int64_t kOverlayMinRefetchMs = 500;
// document_for_path を失敗したあと、取り直すまでの間隔
constexpr int64_t kOverlayDocumentRetryMs = 3000;
// 1 回の取り直しで取るページ数の上限 (サービスが返し続ける暴走を止める)
constexpr int kOverlayMaxPages = 64;

// 開いている文書 (シーン / プレハブ / アクター) にかかっている、AI か不明の構造の変更の表 (表示専用)。
// **World を変えない**: ECS のコンポーネント・イベントを足さず、fileId を採番しない。各窓は毎フレーム fileId で O(1) に引く。
// 取り直しは、文書が変わった・通知が来た・つながり直したときだけ (毎フレームは取らない)。
class SceneChangeOverlay {
public:
    SceneChangeOverlay() = default;
    SceneChangeOverlay(const SceneChangeOverlay&) = delete;
    SceneChangeOverlay& operator=(const SceneChangeOverlay&) = delete;

    void Attach(TrackClient* client) { client_ = client; }

    // 開いている文書の絶対パス (毎フレーム呼んでよい。変わったときだけ表を入れ替える)。空 / 対象外の拡張子は「文書なし」
    void SetDocumentPath(const std::wstring& absPath);
    const std::wstring& DocumentPath() const { return docPath_; }
    // 文書が scene / prefab / actor の JSON か
    static bool IsTrackedDocumentPath(const std::wstring& path);

    // 強調の表示の切り替え (偽なら BadgeFor は常に空)。既定は真
    void SetShow(bool show)
    {
        if (show_ != show) {
            show_ = show;
            NotifyPrefs();
        }
    }
    bool Show() const { return show_; }

    // 強調の色。変えると prefs の受け口へ知らせる (EditorSettings へ保存する)。Hierarchy / Inspector / Scene View が同じ値を読む
    const OverlayPalette& Palette() const { return palette_; }
    void SetPalette(const OverlayPalette& palette)
    {
        if (!(palette_ == palette)) {
            palette_ = palette;
            paletteUnsaved_ = false;
            NotifyPrefs();
        }
    }
    // ドラッグ中の色: すぐ表示へ反映するが、保存 (受け口) は CommitPalette まで待つ (1 ドラッグで何度も書かない)
    void PreviewPalette(const OverlayPalette& palette)
    {
        palette_ = palette;
        paletteUnsaved_ = true;
    }
    bool PaletteUnsaved() const { return paletteUnsaved_; }
    void CommitPalette()
    {
        if (paletteUnsaved_) {
            paletteUnsaved_ = false;
            NotifyPrefs();
        }
    }
    // 保存済みの値を入れる (受け口へは知らせない)
    void LoadPrefs(const OverlayPalette& palette, bool show)
    {
        palette_ = palette;
        show_ = show;
    }
    void SetPrefsSink(std::function<void(const OverlayPalette&, bool)> sink) { prefsSink_ = std::move(sink); }

    // Scene View が描いたワイヤ箱の数 (表示専用)
    void NoteWireBoxes(int drawn, int omitted) { boxStats_ = { drawn, omitted }; }
    const OverlayBoxStats& BoxStats() const { return boxStats_; }
    // AI Changes 窓が開いている (表を使う)。強調が切でも、窓が開いていれば取り直す
    void SetWindowOpen(bool open) { windowOpen_ = open; }

    // 「ここまで確認済みにする」の印 (documentId → Revision)。表示だけの印で、記録にもサービスにも何も書かない
    void LoadAcks(std::map<std::string, int64_t> acks) { acks_ = std::move(acks); }
    const std::map<std::string, int64_t>& Acks() const { return acks_; }
    void SetAckSink(std::function<void(const std::map<std::string, int64_t>&)> sink) { ackSink_ = std::move(sink); }
    void AcknowledgeUpTo();
    void ClearAck();
    bool HasAck() const { return !documentId_.empty() && acks_.count(documentId_) != 0; }

    // 通知・接続の変化 (ChangeTrackingSession の配線から)
    void OnConnected();
    void OnDisconnected();
    void OnNotification(const nlohmann::json& event);
    // 状態を捨てて、文書の取り直しからやり直す (Start し直し)。印は残す
    void Reset();
    // 表を、間引きを待たずに取り直す (復元の記録のあと。通知が出ない変更のため)
    void Refresh();

    // 毎フレーム 1 回 (TrackClient::Poll の後)。必要なときだけ要求を出す
    void Tick();

    // 毎フレーム引く口。強調が切のときと、fileId が 0 のとき (採番されていないエンティティ) は空
    OverlayBadge BadgeFor(uint64_t fileId) const;
    const OverlayEntity* Find(uint64_t fileId) const;
    // 表の全件 (Scene View が箱の候補を作る。走査順は不定なので、順序に使うときは fileId で並べ直す)
    const std::unordered_map<int64_t, OverlayEntity>& Entities() const { return table_; }
    const std::string& DocumentId() const { return documentId_; }
    OverlayCounts Counts() const;
    bool Incomplete() const { return incomplete_; }
    bool Truncated() const { return truncated_; }
    bool Loaded() const { return loaded_; }
    int64_t LatestRevisionId() const { return latestRevisionId_; }
    const std::vector<OverlayField>& DocumentChanges() const { return documentChanges_; }
    size_t EntityCount() const { return table_.size(); }

    // 子孫の集計。表を取り直したあとに 1 回だけ、ライブのシーンの親子を**読んで**作る (毎フレーム木は走査しない)
    bool NeedsAncestorRebuild() const { return ancestorsDirty_; }
    void RebuildAncestors(Scene& scene);

    // 試験用
    int FetchCount() const { return fetches_; }
    int DocumentFetchCount() const { return documentFetches_; }
    bool IsStale() const { return stale_; }

    // 応答 1 ページの解釈 (試験が直接叩く)。entities を out へ足す
    static void ParseEntities(const nlohmann::json& result, std::unordered_map<int64_t, OverlayEntity>& out);
    static std::vector<OverlayField> ParseFields(const nlohmann::json& fields);

private:
    bool WantsData() const { return show_ || windowOpen_; }
    void NotifyPrefs()
    {
        if (prefsSink_) {
            prefsSink_(palette_, show_);
        }
    }
    void MarkStale(bool force);
    void ClearTable();
    void FetchDocument();
    void FetchPage(uint64_t generation, const std::string& cursor, int page);
    void FinishFetch(uint64_t generation);

    TrackClient* client_ = nullptr;
    std::wstring docPath_;
    std::string docPathUtf8_;
    std::string documentId_; // 空 = 未解決
    bool needDocument_ = false;
    bool documentInFlight_ = false;
    int64_t documentRetryAtMs_ = 0; // 失敗したときだけ、この時刻まで引き直さない
    int documentFetches_ = 0;

    bool show_ = true;
    OverlayPalette palette_;
    bool paletteUnsaved_ = false;
    OverlayBoxStats boxStats_;
    std::function<void(const OverlayPalette&, bool)> prefsSink_;
    bool windowOpen_ = false;
    bool stale_ = false;
    bool force_ = false;
    bool inFlight_ = false;
    int64_t lastFetchMs_ = 0;
    int fetches_ = 0;
    uint64_t generation_ = 0;

    // 取り直し中の組み立て。最後のページが来たら table_ と入れ替える (途中の状態を見せない)
    std::unordered_map<int64_t, OverlayEntity> building_;
    std::vector<OverlayField> buildingDocumentChanges_;
    bool buildingIncomplete_ = false;
    bool buildingTruncated_ = false;
    int64_t buildingLatest_ = 0;

    std::unordered_map<int64_t, OverlayEntity> table_;
    std::vector<OverlayField> documentChanges_;
    bool incomplete_ = false;
    bool truncated_ = false;
    bool loaded_ = false;
    int64_t latestRevisionId_ = 0;

    std::unordered_set<int64_t> ancestors_;
    bool ancestorsDirty_ = false;

    std::map<std::string, int64_t> acks_;
    std::function<void(const std::map<std::string, int64_t>&)> ackSink_;
};

// 一覧の行から、開いているシーンのエンティティを選ぶ口 (AI Changes 窓の行のクリック)。
// ライブのシーンに居ない fileId は選ばず false。選択はアセット選択と排他 (Selection::SelectOnly)
bool SelectEntityByFileId(Scene& scene, Selection& selection, uint64_t fileId);

} // namespace mye
