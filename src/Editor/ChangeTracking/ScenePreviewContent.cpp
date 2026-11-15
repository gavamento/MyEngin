//====================================================================================
//                          ScenePreviewContent.cpp
//  MyEngine/ 秋田蓮音                                                      11/16/2026
//                                          Before / After のシーンの内容の取得と、プレビュー専用 Scene への読み込み (実装)
//====================================================================================
#include "Editor/ChangeTracking/ScenePreviewContent.h"

#include <algorithm>
#include <cstring>
#include <set>

#include "Editor/ChangeTracking/BlobHeadView.h"
#include "Editor/ChangeTracking/PathGuard.h"
#include "Editor/ChangeTracking/TrackClient.h"
#include "Editor/ChangeTracking/TrackProtocol.h"
#include "Engine/Core/Ecs/ComponentRegistry.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Scene/SceneSerializer.h"
#include "Engine/Platform/Sha256.h"

namespace mye {

using nlohmann::json;

namespace {

bool ResponseOk(const json& response)
{
    return response.is_object() && response.contains("ok") && response["ok"].is_boolean() && response["ok"].get<bool>() && response.contains("result")
           && response["result"].is_object();
}

std::string ErrorCode(const json& response)
{
    if (response.is_object() && response.contains("error") && response["error"].is_object() && response["error"].contains("code")
        && response["error"]["code"].is_string()) {
        return response["error"]["code"].get<std::string>();
    }
    return std::string();
}

} // namespace

// ---- PreviewContentFetch ----

struct PreviewContentFetch::Data {
    PreviewFetchState state = PreviewFetchState::Idle;
    PreviewFailure failure = PreviewFailure::None;
    std::string detail;
    std::string text;
    uint64_t total = 0;
    // blob
    std::string hash;
    int64_t expectedSize = -1;
    // 今のファイル
    bool disk = false;
    std::wstring abs;
    RestoreFileIo* io = nullptr;

    void Fail(PreviewFailure why, std::string note = std::string())
    {
        state = PreviewFetchState::Failed;
        failure = why;
        detail = std::move(note);
        text.clear();
        text.shrink_to_fit();
    }
};

PreviewContentFetch::PreviewContentFetch() : data_(std::make_shared<Data>())
{
}

void PreviewContentFetch::Cancel()
{
    // 古い Data は、飛んでいる応答のコールバックが持つ弱い参照が切れて、何もせずに終わる
    data_ = std::make_shared<Data>();
}

void PreviewContentFetch::StartBlob(TrackClient& client, const RestoreSide& side)
{
    Cancel();
    Data& d = *data_;
    if (side.state == RestoreBlob::NoSnapshot) {
        d.Fail(PreviewFailure::NoContent);
        return;
    }
    if (side.state != RestoreBlob::Present || side.hash.empty()) {
        d.Fail(PreviewFailure::Unavailable);
        return;
    }
    if (side.size > 0 && static_cast<uint64_t>(side.size) > kPreviewMaxDocumentBytes) {
        d.Fail(PreviewFailure::TooLarge);
        return;
    }
    if (!client.CanRequest()) {
        d.Fail(PreviewFailure::NotConnected);
        return;
    }
    d.hash = side.hash;
    d.expectedSize = side.size;
    d.state = PreviewFetchState::Loading;
    RequestNext(client, data_);
}

void PreviewContentFetch::RequestNext(TrackClient& client, const std::shared_ptr<Data>& data)
{
    std::weak_ptr<Data> weak = data;
    TrackClient* clientPtr = &client;
    client.Request(trackop::kBlobGet, { { "hash", data->hash }, { "offset", data->text.size() }, { "length", kPreviewBlobChunkBytes } },
                   [weak, clientPtr](const json& response) {
                       const std::shared_ptr<Data> d = weak.lock();
                       if (!d || d->state != PreviewFetchState::Loading) {
                           return; // 取り消したあとに届いた古い応答
                       }
                       if (!ResponseOk(response) || !response["result"].contains("data") || !response["result"]["data"].is_string()) {
                           const std::string code = ErrorCode(response);
                           d->Fail(code == "blob_missing" ? PreviewFailure::Unavailable : PreviewFailure::ServiceError, code);
                           return;
                       }
                       const json& result = response["result"];
                       bool decoded = false;
                       const std::string chunk = BlobHeadView::Base64Decode(result["data"].get<std::string>(), &decoded);
                       const int64_t total = result.contains("size") && result["size"].is_number_integer() ? result["size"].get<int64_t>() : -1;
                       if (!decoded || total < 0 || (d->expectedSize >= 0 && total != d->expectedSize)) {
                           d->Fail(PreviewFailure::Mismatch, "size");
                           return;
                       }
                       if (static_cast<uint64_t>(total) > kPreviewMaxDocumentBytes) {
                           d->Fail(PreviewFailure::TooLarge);
                           return;
                       }
                       d->total = static_cast<uint64_t>(total);
                       if (chunk.empty() && d->text.size() < d->total) {
                           d->Fail(PreviewFailure::ServiceError, "no progress"); // 進まない応答で回り続けない
                           return;
                       }
                       d->text += chunk;
                       if (d->text.size() < d->total) {
                           RequestNext(*clientPtr, d);
                           return;
                       }
                       if (Sha256Hex(d->text) != d->hash) {
                           d->Fail(PreviewFailure::Mismatch, "hash"); // 途中で化けた内容は見せない
                           return;
                       }
                       d->state = PreviewFetchState::Ready;
                   });
}

void PreviewContentFetch::StartDisk(const std::wstring& projectRoot, const std::string& relPath, RestoreFileIo& io)
{
    Cancel();
    Data& d = *data_;
    if (CheckRestorePath(projectRoot, relPath, io) != PathIssue::None) {
        d.Fail(PreviewFailure::DiskError, "path");
        return;
    }
    d.abs = JoinProjectPath(projectRoot, relPath);
    const uint32_t attributes = io.Attributes(d.abs);
    if (attributes == kRestoreAttrAbsent) {
        d.Fail(PreviewFailure::DiskAbsent);
        return;
    }
    if ((attributes & (kRestoreAttrDirectory | kRestoreAttrReparse)) != 0) {
        d.Fail(PreviewFailure::DiskError, "not a regular file");
        return;
    }
    d.disk = true;
    d.io = &io;
    d.state = PreviewFetchState::Loading;
}

void PreviewContentFetch::Tick()
{
    Data& d = *data_;
    if (!d.disk || d.state != PreviewFetchState::Loading || d.io == nullptr) {
        return;
    }
    uint64_t total = 0;
    const size_t before = d.text.size();
    switch (d.io->Read(d.abs, before, kPreviewDiskChunkBytes, d.text, total)) {
    case RestoreFileIo::ReadStatus::Absent: d.Fail(PreviewFailure::DiskAbsent); return;
    case RestoreFileIo::ReadStatus::Error: d.Fail(PreviewFailure::DiskError, "read"); return;
    case RestoreFileIo::ReadStatus::Ok: break;
    }
    d.total = total;
    if (total > kPreviewMaxDocumentBytes) {
        d.Fail(PreviewFailure::TooLarge);
        return;
    }
    if (d.text.size() == before && d.text.size() < total) {
        d.Fail(PreviewFailure::DiskError, "no progress");
        return;
    }
    if (d.text.size() >= total) {
        d.state = PreviewFetchState::Ready;
    }
}

PreviewFetchState PreviewContentFetch::State() const { return data_->state; }
PreviewFailure PreviewContentFetch::Failure() const { return data_->failure; }
uint64_t PreviewContentFetch::Received() const { return data_->text.size(); }
uint64_t PreviewContentFetch::Total() const { return data_->total; }
const std::string& PreviewContentFetch::Text() const { return data_->text; }
const std::string& PreviewContentFetch::Detail() const { return data_->detail; }

// ---- プレビュー専用 Scene への読み込み ----

PreviewLoadInfo LoadPreviewScene(Scene& scene, const std::string& text, const ManagedComponentFilter& isManaged)
{
    PreviewLoadInfo info;
    try {
        json root = json::parse(text, nullptr, false);
        if (root.is_discarded()) {
            info.status = PreviewLoadStatus::BadJson;
            return info;
        }
        if (!root.is_object() || !root.contains("entities") || !root["entities"].is_array()) {
            info.status = PreviewLoadStatus::NotAScene;
            return info;
        }
        if (root.contains("version")) {
            if (!root["version"].is_number_integer()) {
                info.status = PreviewLoadStatus::NotAScene;
                return info;
            }
            info.docVersion = root["version"].get<int>();
            if (info.docVersion > Scene::kDocVersion || info.docVersion < 1) {
                info.status = PreviewLoadStatus::UnsupportedVersion;
                return info;
            }
        } else {
            info.docVersion = 1;
        }
        json& entities = root["entities"];
        if (entities.size() > static_cast<size_t>(kPreviewMaxEntities)) {
            info.entityCount = static_cast<int>(entities.size());
            info.status = PreviewLoadStatus::TooManyEntities;
            return info;
        }
        // C# コンポーネントを取り除く (ManagedHost の問い合わせだけ。インスタンスは作らない)
        if (isManaged) {
            for (json& item : entities) {
                if (!item.is_object() || !item.contains("components") || !item["components"].is_object()) {
                    continue;
                }
                json& components = item["components"];
                std::vector<std::string> managed;
                for (const auto& [name, fields] : components.items()) {
                    if (isManaged(name)) {
                        managed.push_back(name);
                    }
                }
                for (const std::string& name : managed) {
                    components.erase(name);
                    ++info.managedSkipped;
                }
            }
        }
        if (!SceneSerializer::LoadFromJson(scene, root)) {
            info.status = PreviewLoadStatus::NotAScene; // 検査で弾かれた (scene には触れていない)
            return info;
        }
        info.entityCount = static_cast<int>(entities.size());
    } catch (const std::exception& e) {
        info.status = PreviewLoadStatus::NotAScene;
        info.detail = e.what();
    }
    return info;
}

std::vector<uint64_t> CollectPreviewAssetRefs(World& world)
{
    const ComponentRegistry& registry = ComponentRegistry::Get();
    std::set<uint64_t> refs;
    for (ComponentTypeId type = 0; type < registry.Count(); ++type) {
        const ComponentDesc& desc = registry.Desc(type);
        if ((desc.flags & kComponentNoSerialize) != 0) {
            continue;
        }
        std::vector<uint32_t> offsets;
        for (const FieldDesc& field : desc.fields) {
            if (field.type == FieldType::AssetRef && (field.flags & kFieldNoSerialize) == 0) {
                offsets.push_back(field.offset);
            }
        }
        if (offsets.empty()) {
            continue;
        }
        const ComponentTypeId required[] = { type };
        world.ForEachArchetype(required, [&](Archetype& archetype) {
            const int column = archetype.FindTypeIndex(type);
            for (uint32_t row = 0; row < archetype.Count(); ++row) {
                const uint8_t* base = static_cast<const uint8_t*>(archetype.GetPtr(column, row));
                for (const uint32_t offset : offsets) {
                    uint64_t value = 0;
                    std::memcpy(&value, base + offset, sizeof(value)); // AssetID は 8 バイト (FieldTypeSize)
                    if (value != 0) {
                        refs.insert(value);
                    }
                }
            }
        });
    }
    return std::vector<uint64_t>(refs.begin(), refs.end());
}

// ---- PreviewAssetProbe ----

struct PreviewAssetProbe::Data {
    State state = State::Idle;
    int pending = 0;
    int checked = 0;
    int skipped = 0;
    bool failed = false;
    int64_t afterRevisionId = 0;
    std::set<std::string> changed;
    std::vector<std::string> shown;
};

PreviewAssetProbe::PreviewAssetProbe() : data_(std::make_shared<Data>())
{
}

void PreviewAssetProbe::Cancel()
{
    data_ = std::make_shared<Data>();
}

void PreviewAssetProbe::Start(TrackClient& client, std::vector<std::string> paths, int64_t afterRevisionId)
{
    Cancel();
    Data& d = *data_;
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    if (paths.size() > static_cast<size_t>(kPreviewMaxAssetProbes)) {
        d.skipped = static_cast<int>(paths.size()) - kPreviewMaxAssetProbes;
        paths.resize(static_cast<size_t>(kPreviewMaxAssetProbes));
    }
    d.afterRevisionId = afterRevisionId;
    if (paths.empty()) {
        d.state = State::Done;
        return;
    }
    if (!client.CanRequest()) {
        d.state = State::Unknown;
        return;
    }
    d.state = State::Probing;
    d.pending = static_cast<int>(paths.size()); // 全部を数えてから出す (同じ呼び出しの中で返る応答で 0 にならない)
    std::weak_ptr<Data> weak = data_;
    for (const std::string& path : paths) {
        client.Request(trackop::kList, { { "path", path }, { "limit", 1 } }, [weak, path](const json& response) {
            const std::shared_ptr<Data> state = weak.lock();
            if (!state || state->state != State::Probing) {
                return;
            }
            ++state->checked;
            if (!ResponseOk(response) || !response["result"].contains("items") || !response["result"]["items"].is_array()) {
                state->failed = true;
            } else {
                const json& items = response["result"]["items"];
                if (!items.empty() && items[0].is_object() && items[0].contains("revisionId") && items[0]["revisionId"].is_number_integer()
                    && items[0]["revisionId"].get<int64_t>() > state->afterRevisionId) {
                    state->changed.insert(path);
                }
            }
            if (--state->pending == 0) {
                for (const std::string& changedPath : state->changed) {
                    if (state->shown.size() < static_cast<size_t>(kPreviewMaxAssetPathsShown)) {
                        state->shown.push_back(changedPath);
                    }
                }
                state->state = state->failed ? State::Unknown : State::Done;
            }
        });
    }
}

PreviewAssetProbe::State PreviewAssetProbe::GetState() const { return data_->state; }
int PreviewAssetProbe::Checked() const { return data_->checked; }
int PreviewAssetProbe::ChangedCount() const { return static_cast<int>(data_->changed.size()); }
int PreviewAssetProbe::Skipped() const { return data_->skipped; }
const std::vector<std::string>& PreviewAssetProbe::ChangedPaths() const { return data_->shown; }

} // namespace mye
