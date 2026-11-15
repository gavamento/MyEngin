//====================================================================================
//                          SceneChangeOverlay.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          開いているシーンにかかっている AI の変更の表 (取り直し・引き当て)
//====================================================================================
#include "Editor/ChangeTracking/SceneChangeOverlay.h"

#include <algorithm>
#include <cwctype>

#include "Editor/ChangeTracking/TrackClient.h"
#include "Editor/ChangeTracking/TrackProtocol.h"
#include "Editor/Scene/Selection.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {

namespace {

std::string Str(const nlohmann::json& j, const char* key)
{
    return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}

int64_t Int(const nlohmann::json& j, const char* key, int64_t fallback)
{
    return j.is_object() && j.contains(key) && j[key].is_number_integer() ? j[key].get<int64_t>() : fallback;
}

bool Bool(const nlohmann::json& j, const char* key)
{
    return j.is_object() && j.contains(key) && j[key].is_boolean() && j[key].get<bool>();
}

OverlayStatus ParseStatus(const std::string& text)
{
    if (text == "added") {
        return OverlayStatus::Added;
    }
    if (text == "removed") {
        return OverlayStatus::Removed;
    }
    return text == "modified" ? OverlayStatus::Modified : OverlayStatus::None;
}

bool EndsWithNoCase(const std::wstring& text, const wchar_t* suffix)
{
    const std::wstring tail = suffix;
    if (text.size() < tail.size()) {
        return false;
    }
    for (size_t i = 0; i < tail.size(); ++i) {
        if (std::towlower(text[text.size() - tail.size() + i]) != tail[i]) {
            return false;
        }
    }
    return true;
}

bool IsOk(const nlohmann::json& response)
{
    return response.is_object() && Bool(response, "ok") && response.contains("result") && response["result"].is_object();
}

} // namespace

bool SceneChangeOverlay::IsTrackedDocumentPath(const std::wstring& path)
{
    return EndsWithNoCase(path, L".scene.json") || EndsWithNoCase(path, L".prefab.json") || EndsWithNoCase(path, L".actor.json");
}

std::vector<OverlayField> SceneChangeOverlay::ParseFields(const nlohmann::json& fields)
{
    std::vector<OverlayField> out;
    if (!fields.is_array()) {
        return out;
    }
    for (const nlohmann::json& item : fields) {
        if (!item.is_object()) {
            continue;
        }
        OverlayField f;
        f.name = Str(item, "name");
        f.hasBefore = item.contains("before") && item["before"].is_string();
        f.hasAfter = item.contains("after") && item["after"].is_string();
        f.before = f.hasBefore ? item["before"].get<std::string>() : std::string();
        f.after = f.hasAfter ? item["after"].get<std::string>() : std::string();
        f.lastRevisionId = Int(item, "lastRevisionId", 0);
        f.changeType = Str(item, "changeType");
        f.overrideField = Bool(item, "override");
        f.oversize = Bool(item, "oversize");
        out.push_back(std::move(f));
    }
    return out;
}

void SceneChangeOverlay::ParseEntities(const nlohmann::json& result, std::unordered_map<int64_t, OverlayEntity>& out)
{
    if (!result.is_object() || !result.contains("entities") || !result["entities"].is_array()) {
        return;
    }
    for (const nlohmann::json& item : result["entities"]) {
        if (!item.is_object() || !item.contains("fileId") || !item["fileId"].is_number_integer()) {
            continue;
        }
        OverlayEntity e;
        e.fileId = item["fileId"].get<int64_t>();
        e.status = ParseStatus(Str(item, "status"));
        e.name = Str(item, "name");
        e.hierarchyChanged = Bool(item, "hierarchyChanged");
        e.hierarchy = ParseFields(item.contains("hierarchy") ? item["hierarchy"] : nlohmann::json());
        e.entityFields = ParseFields(item.contains("entityFields") ? item["entityFields"] : nlohmann::json());
        if (item.contains("components") && item["components"].is_array()) {
            for (const nlohmann::json& c : item["components"]) {
                OverlayComponent comp;
                comp.name = Str(c, "name");
                comp.status = ParseStatus(Str(c, "status"));
                comp.fields = ParseFields(c.contains("fields") ? c["fields"] : nlohmann::json());
                e.components.push_back(std::move(comp));
            }
        }
        if (item.contains("revisionIds") && item["revisionIds"].is_array()) {
            for (const nlohmann::json& r : item["revisionIds"]) {
                if (r.is_number_integer()) {
                    e.revisionIds.push_back(r.get<int64_t>());
                }
            }
        }
        if (e.status != OverlayStatus::None) {
            out[e.fileId] = std::move(e);
        }
    }
}

void SceneChangeOverlay::SetDocumentPath(const std::wstring& absPath)
{
    const std::wstring effective = IsTrackedDocumentPath(absPath) ? absPath : std::wstring();
    if (effective == docPath_) {
        return;
    }
    // 文書が変わった: 古い表は別の文書のものなので、すぐ捨てる (取り直しの応答は世代で捨てる)
    docPath_ = effective;
    docPathUtf8_ = WideToUtf8(effective);
    documentId_.clear();
    ++generation_;
    inFlight_ = false;
    documentInFlight_ = false;
    ClearTable();
    needDocument_ = !effective.empty();
    documentRetryAtMs_ = 0;
    stale_ = false;
}

void SceneChangeOverlay::ClearTable()
{
    table_.clear();
    documentChanges_.clear();
    ancestors_.clear();
    ancestorsDirty_ = false;
    incomplete_ = false;
    truncated_ = false;
    loaded_ = false;
    latestRevisionId_ = 0;
}

void SceneChangeOverlay::MarkStale(bool force)
{
    stale_ = true;
    force_ = force_ || force;
}

void SceneChangeOverlay::OnConnected()
{
    ++generation_;
    inFlight_ = false;
    documentInFlight_ = false;
    needDocument_ = !docPath_.empty();
    documentRetryAtMs_ = 0;
    MarkStale(true);
}

void SceneChangeOverlay::OnDisconnected()
{
    // 保留中の要求は TrackClient が失敗で返す。つながり直したら OnConnected が取り直す
    ++generation_;
    inFlight_ = false;
    documentInFlight_ = false;
}

void SceneChangeOverlay::Reset()
{
    ++generation_;
    inFlight_ = false;
    documentInFlight_ = false;
    documentId_.clear();
    ClearTable();
    needDocument_ = !docPath_.empty();
    documentRetryAtMs_ = 0;
    stale_ = false;
    force_ = false;
}

void SceneChangeOverlay::OnNotification(const nlohmann::json& event)
{
    const std::string name = Str(event, "event");
    if (name == trackevent::kChangeRecorded) {
        const std::string doc = Str(event, "documentId");
        if (!doc.empty()) {
            // 開いている文書の変更だけ。別の文書の通知では取り直さない
            if (!documentId_.empty() && doc == documentId_) {
                MarkStale(false);
            }
            return;
        }
        // .meta の変更は、文書の ID (guid) が変わりうる引き金。文書の ID から引き直す
        const std::string path = Str(event, "path");
        if (!docPath_.empty() && path.size() > 5 && path.compare(path.size() - 5, 5, ".meta") == 0) {
            needDocument_ = true;
            documentRetryAtMs_ = 0;
            MarkStale(false);
        }
    } else if (name == trackevent::kAttributionUpdated) {
        // 行為者が変わると対象の Revision が変わる (editor_user を外す・足す)
        if (!documentId_.empty()) {
            MarkStale(false);
        }
    } else if (name == trackevent::kReviewChanged) {
        // 承認された構造の変更は強調から外れる (サービスが返さなくなる)。どの文書の Revision かは通知に無いので、開いている文書は取り直す
        if (!documentId_.empty()) {
            MarkStale(false);
        }
    } else if (name == trackevent::kResync) {
        needDocument_ = !docPath_.empty();
        documentRetryAtMs_ = 0;
        MarkStale(true);
    }
}

void SceneChangeOverlay::AcknowledgeUpTo()
{
    if (documentId_.empty() || latestRevisionId_ <= 0) {
        return;
    }
    acks_[documentId_] = latestRevisionId_;
    // それ以前の変更は強調から外す。新しい変更はサービスから取り直す (afterRevisionId より後だけ)
    table_.clear();
    documentChanges_.clear();
    ancestors_.clear();
    ancestorsDirty_ = false;
    incomplete_ = false;
    truncated_ = false;
    MarkStale(true);
    if (ackSink_) {
        ackSink_(acks_);
    }
}

void SceneChangeOverlay::ClearAck()
{
    if (documentId_.empty() || acks_.erase(documentId_) == 0) {
        return;
    }
    MarkStale(true);
    if (ackSink_) {
        ackSink_(acks_);
    }
}

void SceneChangeOverlay::Tick()
{
    if (client_ == nullptr || !client_->CanRequest() || docPath_.empty() || !WantsData()) {
        return;
    }
    const int64_t now = client_->NowMs();
    if (needDocument_ && !documentInFlight_ && now >= documentRetryAtMs_) {
        FetchDocument();
        return;
    }
    if (documentId_.empty() || !stale_ || inFlight_) {
        return;
    }
    if (force_ || fetches_ == 0 || now - lastFetchMs_ >= kOverlayMinRefetchMs) {
        stale_ = false;
        force_ = false;
        inFlight_ = true;
        lastFetchMs_ = now;
        ++fetches_;
        building_.clear();
        buildingDocumentChanges_.clear();
        buildingIncomplete_ = false;
        buildingTruncated_ = false;
        buildingLatest_ = 0;
        FetchPage(generation_, std::string(), 0);
    }
}

void SceneChangeOverlay::FetchDocument()
{
    documentInFlight_ = true;
    ++documentFetches_;
    const uint64_t generation = generation_;
    client_->Request(trackop::kDocumentForPath, { { "path", docPathUtf8_ } }, [this, generation](const nlohmann::json& response) {
        if (generation != generation_) {
            return; // 文書が変わったあとに届いた古い応答
        }
        documentInFlight_ = false;
        if (!IsOk(response)) {
            documentRetryAtMs_ = (client_ != nullptr ? client_->NowMs() : 0) + kOverlayDocumentRetryMs; // 失敗: 間隔を置いて引き直す
            return;
        }
        const std::string id = Str(response["result"], "documentId");
        if (id.empty()) {
            return;
        }
        needDocument_ = false;
        if (id != documentId_) {
            documentId_ = id;
            ClearTable();
        }
        MarkStale(true);
    });
}

void SceneChangeOverlay::FetchPage(uint64_t generation, const std::string& cursor, int page)
{
    nlohmann::json args = { { "documentId", documentId_ } };
    const auto ack = acks_.find(documentId_);
    if (ack != acks_.end()) {
        args["afterRevisionId"] = ack->second;
    }
    if (!cursor.empty()) {
        args["cursor"] = cursor;
    }
    client_->Request(trackop::kSceneOverlay, args, [this, generation, page](const nlohmann::json& response) {
        if (generation != generation_) {
            return;
        }
        if (!IsOk(response)) {
            inFlight_ = false; // 失敗: 表はそのまま。次の通知か再接続で取り直す
            return;
        }
        const nlohmann::json& result = response["result"];
        ParseEntities(result, building_);
        if (result.contains("documentChanges")) {
            buildingDocumentChanges_ = ParseFields(result["documentChanges"]);
        }
        buildingIncomplete_ = buildingIncomplete_ || Bool(result, "incomplete");
        buildingTruncated_ = buildingTruncated_ || Bool(result, "truncated");
        buildingLatest_ = (std::max)(buildingLatest_, Int(result, "latestRevisionId", 0));
        const std::string next = Str(result, "nextCursor");
        if (!next.empty() && page + 1 < kOverlayMaxPages && client_ != nullptr && client_->CanRequest()) {
            FetchPage(generation, next, page + 1);
            return;
        }
        buildingTruncated_ = buildingTruncated_ || !next.empty();
        FinishFetch(generation);
    });
}

void SceneChangeOverlay::FinishFetch(uint64_t generation)
{
    if (generation != generation_) {
        return;
    }
    table_ = std::move(building_);
    building_.clear();
    documentChanges_ = std::move(buildingDocumentChanges_);
    buildingDocumentChanges_.clear();
    incomplete_ = buildingIncomplete_;
    truncated_ = buildingTruncated_;
    latestRevisionId_ = buildingLatest_;
    loaded_ = true;
    inFlight_ = false;
    ancestors_.clear();
    ancestorsDirty_ = true; // 子孫の集計は、ライブのシーンを読める側が 1 回だけ作る
}

OverlayBadge SceneChangeOverlay::BadgeFor(uint64_t fileId) const
{
    OverlayBadge badge;
    if (!show_ || fileId == 0 || table_.empty()) {
        return badge;
    }
    const auto it = table_.find(static_cast<int64_t>(fileId));
    if (it != table_.end() && (it->second.status == OverlayStatus::Added || it->second.status == OverlayStatus::Modified)) {
        badge.status = it->second.status;
        return badge;
    }
    badge.descendantOnly = ancestors_.count(static_cast<int64_t>(fileId)) != 0;
    return badge;
}

const OverlayEntity* SceneChangeOverlay::Find(uint64_t fileId) const
{
    const auto it = table_.find(static_cast<int64_t>(fileId));
    return it == table_.end() ? nullptr : &it->second;
}

OverlayCounts SceneChangeOverlay::Counts() const
{
    OverlayCounts counts;
    for (const auto& [id, entity] : table_) {
        counts.added += entity.status == OverlayStatus::Added ? 1 : 0;
        counts.modified += entity.status == OverlayStatus::Modified ? 1 : 0;
        counts.removed += entity.status == OverlayStatus::Removed ? 1 : 0;
    }
    return counts;
}

void SceneChangeOverlay::RebuildAncestors(Scene& scene)
{
    ancestorsDirty_ = false;
    ancestors_.clear();
    World& world = scene.GetWorld();
    for (const auto& [id, entity] : table_) {
        if (entity.status != OverlayStatus::Added && entity.status != OverlayStatus::Modified) {
            continue; // 削除されたものはライブのシーンに居ない
        }
        const GameObject g = scene.FindByFileId(static_cast<uint64_t>(id));
        if (!g) {
            continue;
        }
        // 親をたどる (読むだけ)。既に入れた祖先に当たったら、その上も入っているので打ち切る
        for (EntityID p = world.GetParent(g.Id()); !p.IsNull(); p = world.GetParent(p)) {
            const auto* fid = world.GetComponent<FileIdComponent>(p);
            if (fid == nullptr || fid->value == 0) {
                continue;
            }
            if (!ancestors_.insert(static_cast<int64_t>(fid->value)).second) {
                break;
            }
        }
    }
}

bool SelectEntityByFileId(Scene& scene, Selection& selection, uint64_t fileId)
{
    if (fileId == 0 || !scene.FindByFileId(fileId)) {
        return false;
    }
    selection.SelectOnly(fileId);
    return true;
}

} // namespace mye
