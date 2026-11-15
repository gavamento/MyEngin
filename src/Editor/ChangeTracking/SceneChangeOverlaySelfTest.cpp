//====================================================================================
//                          SceneChangeOverlaySelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          開いているシーンの変更の強調・一覧・選択の同期の回帰テスト
//====================================================================================
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <Windows.h>
#include <DirectXMath.h>

#include "imgui.h"
#include "nlohmann/json.hpp"

#include "Editor/App/EditorSettings.h"
#include "Editor/ChangeTracking/ChangeFormat.h"
#include "Editor/ChangeTracking/ChangeTrackingFakeIo.h"
#include "Editor/ChangeTracking/ChangeTrackingSelfTest.h"
#include "Editor/ChangeTracking/ChangeTrackingSession.h"
#include "Editor/ChangeTracking/SceneChangeOverlay.h"
#include "Editor/ChangeTracking/SceneChangeWireBoxes.h"
#include "Editor/Scene/Selection.h"
#include "Editor/Undo/UndoStack.h"
#include "Editor/Windows/Project/AiChangesWindow.h"
#include "Editor/Asset/AssetPreviewCache.h"
#include "Editor/Windows/Scene/HierarchyWindow.h"
#include "Editor/Windows/Scene/InspectorWindow.h"
#include "Editor/Windows/Scene/SceneViewWindow.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Platform/PathUtil.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace mye {
namespace {

using namespace tracktest;
using Check = std::function<void(bool, const char*)>;

const std::wstring kScenePath = std::wstring(kRoot) + L"\\assets\\scenes\\a.scene.json";
const std::wstring kOtherScenePath = std::wstring(kRoot) + L"\\assets\\scenes\\b.scene.json";

// 偽のサービス: 書かれた要求に、決めた応答を返す。節ごとに作る
struct OverlayRig {
    std::shared_ptr<FakeState> s = std::make_shared<FakeState>();
    ChangeTrackingSession session;
    SceneChangeOverlay& overlay = session.Overlay();
    size_t answered = 0;
    std::string documentId = "DOC1";
    // scene_overlay のページ (最後以外は nextCursor が付く)。空なら 1 ページ分の entities
    std::vector<json> pages;
    json entities = json::array();
    int64_t latest = 10;
    bool holdOverlay = false; // 真なら scene_overlay に答えない (飛んでいる要求の再現)
    bool failDocument = false; // 真なら document_for_path を断る
    std::vector<json> held;

    explicit OverlayRig(bool enabled = true)
    {
        session.StartWithIo(std::make_unique<FakeIo>(s), kRoot, enabled);
        if (enabled) {
            session.Poll();
            ReplyHelloOk(*s);
            session.Poll();
        }
    }

    size_t Count(const char* op) const { return Requests(*s, op).size(); }
    json Last(const char* op) const
    {
        const std::vector<json> all = Requests(*s, op);
        return all.empty() ? json() : all.back();
    }

    void Answer()
    {
        for (; answered < s->written.size(); ++answered) {
            const json req = json::parse(s->written[answered], nullptr, false);
            const std::string op = req.value("op", std::string());
            const uint64_t id = req["id"].get<uint64_t>();
            if (op == trackop::kDocumentForPath && failDocument) {
                FeedJson(*s, ErrReply(id, "bad_args"));
            } else if (op == trackop::kDocumentForPath) {
                FeedJson(*s, OkReply(id, { { "documentId", documentId }, { "idSource", "meta" }, { "kind", "scene" }, { "known", true } }));
            } else if (op == trackop::kStatus) {
                FeedJson(*s, OkReply(id, { { "phase", "watching" } }));
            } else if (op == trackop::kSessions) {
                FeedJson(*s, OkReply(id, { { "items", json::array() }, { "nextCursor", nullptr } }));
            } else if (op == trackop::kList) {
                FeedJson(*s, OkReply(id, { { "items", json::array({ { { "revisionId", 7 }, { "path", "assets/scenes/a.scene.json" }, { "changeType", "modified" },
                                                                       { "actor", "unknown" }, { "attribution", "unknown" } } }) },
                                           { "nextCursor", nullptr }, { "coverage", json::object() } }));
            } else if (op == trackop::kDescribe) {
                FeedJson(*s, OkReply(id, { { "revisionId", 7 }, { "path", "assets/scenes/a.scene.json" }, { "changeType", "modified" }, { "diffMode", "none" },
                                           { "actor", "unknown" }, { "attribution", "unknown" }, { "hunks", json::array() }, { "evidence", json::array() },
                                           { "sceneSummary", { { "documentId", "DOC1" }, { "resolvedDocumentId", "DOC1" }, { "counts", json::object() } } } }));
            } else if (op == trackop::kSceneDiff) {
                const bool second = req["args"].contains("cursor");
                json items = json::array();
                if (!second) {
                    items.push_back({ { "sceneChangeId", 1 }, { "documentId", "DOC1" }, { "resolvedDocumentId", "DOC1" }, { "fileId", 5 },
                                      { "changeType", "FieldModified" }, { "component", "Light" }, { "field", "intensity" },
                                      { "beforeValue", "1.0" }, { "afterValue", "2.5" }, { "oversize", false } });
                    items.push_back({ { "sceneChangeId", 2 }, { "documentId", "DOC1" }, { "resolvedDocumentId", "DOC1" }, { "fileId", 9 },
                                      { "changeType", "EntityRemoved" }, { "component", nullptr }, { "field", nullptr }, { "beforeValue", "{}" },
                                      { "afterValue", nullptr }, { "oversize", false } });
                } else {
                    items.push_back({ { "sceneChangeId", 3 }, { "documentId", "DOC1" }, { "resolvedDocumentId", "DOC1" }, { "fileId", nullptr },
                                      { "changeType", "unparsed" }, { "afterValue", "\"JSON を読めない (後)\"" }, { "oversize", false } });
                }
                FeedJson(*s, OkReply(id, { { "items", items }, { "nextCursor", second ? json(nullptr) : json("2") } }));
            } else if (op == trackop::kSceneOverlay) {
                if (holdOverlay) {
                    held.push_back(req);
                    continue;
                }
                FeedJson(*s, OkReply(id, OverlayResult(req)));
            }
        }
    }

    json OverlayResult(const json& req) const
    {
        const std::string cursor = req["args"].value("cursor", std::string());
        size_t index = cursor.empty() ? 0 : static_cast<size_t>(std::stoi(cursor));
        json result = { { "documentId", documentId }, { "entities", json::array() }, { "nextCursor", nullptr }, { "incomplete", false },
                        { "truncated", false }, { "unparsedRevisionIds", json::array() }, { "latestRevisionId", latest } };
        if (pages.empty()) {
            result["entities"] = entities;
        } else {
            result["entities"] = pages[index];
            if (index + 1 < pages.size()) {
                result["nextCursor"] = std::to_string(index + 1);
            }
        }
        if (cursor.empty()) {
            result["documentChanges"] = json::array();
        }
        return result;
    }

    // 1 フレームぶん進める (答えて、Poll)
    void Frame()
    {
        Answer();
        session.Poll();
    }
    // 落ち着くまで回す
    void Settle()
    {
        for (int i = 0; i < 6; ++i) {
            Frame();
        }
    }
};

json EntityJson(int64_t fileId, const char* status, const char* name = "")
{
    return { { "fileId", fileId }, { "status", status }, { "name", name }, { "hierarchyChanged", false }, { "hierarchy", json::array() },
             { "entityFields", json::array() }, { "components", json::array() }, { "revisionIds", json::array({ 3 }) } };
}

json EntityWithField(int64_t fileId, const char* comp, const char* field)
{
    json e = EntityJson(fileId, "modified");
    e["components"] = json::array({ { { "name", comp }, { "status", "modified" },
                                      { "fields", json::array({ { { "name", field }, { "before", "1" }, { "after", "2" }, { "lastRevisionId", 3 },
                                                                  { "changeType", "FieldModified" }, { "override", false }, { "oversize", false } } }) } } });
    return e;
}

template <typename F>
void InBothLanguages(F&& body)
{
    const Lang previous = CurrentLanguage();
    for (const Lang lang : { Lang::En, Lang::Ja }) {
        SetLanguage(lang);
        body(lang);
    }
    SetLanguage(previous);
}

// ---- 解釈と文面 ----

void TestParseAndFormat(const Check& check)
{
    std::unordered_map<int64_t, OverlayEntity> table;
    json page = { { "entities", json::array({ EntityJson(5, "added", "Box"), EntityWithField(6, "Light", "intensity"), EntityJson(7, "removed"),
                                              EntityJson(8, "weird") }) } };
    SceneChangeOverlay::ParseEntities(page, table);
    check(table.size() == 3 && table[5].status == OverlayStatus::Added && table[5].name == "Box" && table[7].status == OverlayStatus::Removed
              && table.count(8) == 0,
          "overlay parse: entities by fileId; an unknown status is dropped");
    check(table[6].components.size() == 1 && table[6].components[0].fields.size() == 1 && table[6].components[0].fields[0].before == "1"
              && table[6].components[0].fields[0].hasAfter && table[6].revisionIds == std::vector<int64_t>({ 3 }),
          "overlay parse: components, fields with their literal values, revision ids");

    // 種類の文面: 11 種 + unparsed を英日の両方で (空でなく、キーそのものでなく、プレハブの 3 種が互いに違う)
    InBothLanguages([&](Lang lang) {
        bool all = true;
        std::set<std::string> labels;
        for (const std::string& kind : SceneChangeKinds()) {
            const std::string label = SceneKindLabel(kind);
            all = all && !label.empty() && label != kind;
            labels.insert(label);
        }
        check(all && SceneChangeKinds().size() == 12 && labels.size() == 12,
              lang == Lang::En ? "scene kind labels: all 11 kinds + unparsed have distinct English text"
                               : "scene kind labels: all 11 kinds + unparsed have distinct Japanese text");
    });
    check(SceneKindLabel("SomethingNew") == "SomethingNew", "scene kind labels: an unknown kind is shown as its raw value");

    // 値の切り方
    check(ClipValue("abc", 10) == "abc" && ClipValue("abcdef", 3) == "abc...", "clip value: short values stay, long ones are cut with an ellipsis");
    check(ClipValue("a\nb\tc", 10) == "a b c", "clip value: newlines and tabs become spaces");
    const std::string japanese = "あいうえお"; // 1 文字 3 バイト。文字の途中で切らない
    check(ClipValue(japanese, 2) == "あい...", "clip value: cuts at a UTF-8 character boundary");

    // scene_diff の 1 件の整形
    TrackSceneChange change;
    change.hasFileId = true;
    change.fileId = 12;
    change.changeType = "FieldModified";
    change.component = "Light";
    change.field = "intensity";
    change.hasBefore = change.hasAfter = true;
    change.beforeValue = "1.0";
    change.afterValue = "2.5";
    SceneChangeText text = FormatSceneChange(change, "Lamp");
    check(text.target == "Lamp (#12)" && text.component == "Light" && text.field == "intensity" && text.before == "1.0" && text.after == "2.5",
          "scene change text: target with name and fileId, component, field, before and after");
    check(FormatSceneChange(change, "").target == "(#12)", "scene change text: without a live name only the fileId is shown");
    change.beforeValue = std::string(500, 'x');
    check(FormatSceneChange(change, "").before.size() == kSceneValueMaxChars + 3, "scene change text: a long value is cut");
    change.oversize = true;
    change.beforeBytes = 900000;
    check(FormatSceneChange(change, "").before == std::string(Tr(StrId::DiffView_Oversize)), "scene change text: an oversize value says so instead of showing nothing");
    TrackSceneChange unparsed;
    unparsed.changeType = "unparsed";
    unparsed.afterValue = "\"JSON を読めない (後)\"";
    check(FormatSceneChange(unparsed, "").note == "JSON を読めない (後)" && FormatSceneChange(unparsed, "").target == std::string(Tr(StrId::AiChg_ScWholeScene)),
          "scene change text: an unparsed row shows the decoded reason");
    check(UnparsedReason("not json") == "not json", "scene change text: an undecodable reason is shown as is");

    // バッジのツールチップ: コンポーネント名とフィールド名を最大 3 つ
    OverlayEntity many;
    many.status = OverlayStatus::Modified;
    OverlayComponent comp;
    comp.name = "Light";
    for (const char* f : { "a", "b", "c", "d" }) {
        OverlayField field;
        field.name = f;
        comp.fields.push_back(field);
    }
    many.components.push_back(comp);
    const std::string tip = OverlayBadgeTooltip(&many, false);
    check(tip.find("Light.a") != std::string::npos && tip.find("Light.c") != std::string::npos && tip.find("Light.d") == std::string::npos
              && tip.find(Tr(StrId::AiChg_BadgeMore)) != std::string::npos,
          "badge tooltip: lists up to three component.field names then says there are more");
    check(OverlayBadgeTooltip(nullptr, true) == std::string(Tr(StrId::AiChg_BadgeDescendant)), "badge tooltip: a descendant-only badge says so");
}

// ---- 取り直しの契機 ----

void TestRefresh(const Check& check)
{
    OverlayRig r;
    r.entities = json::array({ EntityJson(5, "added", "Box"), EntityWithField(6, "Light", "intensity"), EntityJson(9, "removed") });
    r.overlay.SetDocumentPath(kScenePath);
    r.Settle();
    check(r.Count(trackop::kDocumentForPath) == 1 && r.Count(trackop::kSceneOverlay) == 1 && r.overlay.DocumentId() == "DOC1" && r.overlay.Loaded(),
          "overlay refresh: opening a scene asks for its document id once, then fetches the overlay once");
    const json askedPath = r.Last(trackop::kDocumentForPath)["args"];
    check(askedPath.value("path", std::string()) == WideToUtf8(kScenePath) && askedPath.size() == 1,
          "overlay refresh: the document id is asked by the open scene's path (the service owns the id rule)");
    check(r.overlay.BadgeFor(5).status == OverlayStatus::Added && r.overlay.BadgeFor(6).status == OverlayStatus::Modified
              && !r.overlay.BadgeFor(7).Any() && !r.overlay.BadgeFor(9).Any() && !r.overlay.BadgeFor(0).Any(),
          "overlay lookup: added / modified get a badge; unknown ids, removed entities and fileId 0 do not");
    const OverlayCounts counts = r.overlay.Counts();
    check(counts.added == 1 && counts.modified == 1 && counts.removed == 1, "overlay counts: added / modified / removed");
    check(r.Last(trackop::kSceneOverlay)["args"].size() == 1 && r.Last(trackop::kSceneOverlay)["args"].contains("documentId"),
          "overlay refresh: the first fetch has no afterRevisionId or cursor");

    // 一致しない documentId の通知では取り直さない
    r.session.Client().DispatchLine(json({ { "event", "change_recorded" }, { "documentId", "OTHER" }, { "revisionId", 11 }, { "path", "b.scene.json" } }).dump());
    r.Settle();
    check(r.Count(trackop::kSceneOverlay) == 1 && !r.overlay.IsStale(), "overlay refresh: a notification for another document does not refetch");
    // 一致する通知: 1 回だけ取り直す。連続の通知は 1 回にまとまり、間隔が空くまで次を取らない
    r.s->nowMs += 1000;
    for (int i = 0; i < 3; ++i) {
        r.session.Client().DispatchLine(json({ { "event", "change_recorded" }, { "documentId", "DOC1" }, { "revisionId", 12 + i }, { "path", "a.scene.json" } }).dump());
    }
    r.Settle();
    check(r.Count(trackop::kSceneOverlay) == 2, "overlay refresh: a notification for the open document refetches once (a burst of three is one fetch)");
    r.session.Client().DispatchLine(json({ { "event", "change_recorded" }, { "documentId", "DOC1" }, { "revisionId", 20 }, { "path", "a.scene.json" } }).dump());
    r.Settle();
    check(r.Count(trackop::kSceneOverlay) == 2 && r.overlay.IsStale(), "overlay refresh: a notification right after a fetch is held back (throttled)");
    r.s->nowMs += kOverlayMinRefetchMs;
    r.Settle();
    check(r.Count(trackop::kSceneOverlay) == 3 && !r.overlay.IsStale(), "overlay refresh: the held-back refetch runs once the interval has passed");
    // 毎フレームは取り直さない
    for (int i = 0; i < 30; ++i) {
        r.s->nowMs += 100;
        r.Frame();
    }
    check(r.Count(trackop::kSceneOverlay) == 3 && r.Count(trackop::kDocumentForPath) == 1, "overlay refresh: nothing is fetched on quiet frames");

    // 行為者が変わる通知 (attribution_updated) と、取りこぼし (resync)
    r.s->nowMs += 1000;
    r.session.Client().DispatchLine(json({ { "event", "attribution_updated" }, { "revisionId", 11 } }).dump());
    r.Settle();
    check(r.Count(trackop::kSceneOverlay) == 4, "overlay refresh: attribution_updated refetches (the actor decides which revisions count)");
    r.session.Client().DispatchLine(json({ { "event", "resync" } }).dump());
    r.Settle();
    check(r.Count(trackop::kSceneOverlay) == 5 && r.Count(trackop::kDocumentForPath) == 2, "overlay refresh: resync asks for the document id and the overlay again at once");
    // .meta の変更 (guid が変わりうる): 文書の ID を引き直す
    r.s->nowMs += 5000;
    r.session.Client().DispatchLine(json({ { "event", "change_recorded" }, { "revisionId", 30 }, { "path", "assets/scenes/a.scene.json.meta" } }).dump());
    r.Settle();
    check(r.Count(trackop::kDocumentForPath) == 3, "overlay refresh: a .meta change asks for the document id again");
}

void TestPaging(const Check& check)
{
    OverlayRig r;
    r.pages = { json::array({ EntityJson(1, "added") }), json::array({ EntityJson(2, "added") }), json::array({ EntityJson(3, "modified") }) };
    r.overlay.SetDocumentPath(kScenePath);
    r.session.Poll();
    r.Frame(); // document_for_path
    r.Frame(); // 1 ページ目
    check(r.Count(trackop::kSceneOverlay) >= 1 && !r.overlay.Loaded() && !r.overlay.BadgeFor(1).Any(),
          "overlay paging: pages are collected before the table is swapped in (no half-loaded table is shown)");
    r.Settle();
    const std::vector<json> asked = Requests(*r.s, trackop::kSceneOverlay);
    check(asked.size() == 3 && asked[1]["args"].value("cursor", std::string()) == "1" && asked[2]["args"].value("cursor", std::string()) == "2",
          "overlay paging: each page is asked with the previous page's cursor");
    check(r.overlay.Loaded() && r.overlay.EntityCount() == 3 && r.overlay.BadgeFor(3).status == OverlayStatus::Modified, "overlay paging: the entities of all pages are in the table");
}

void TestSwitchAndStale(const Check& check)
{
    OverlayRig r;
    r.entities = json::array({ EntityJson(5, "added") });
    r.overlay.SetDocumentPath(kScenePath);
    r.Settle();
    check(r.overlay.BadgeFor(5).status == OverlayStatus::Added, "overlay switch: (setup) the first scene has its badge");

    // 別のシーンへ切り替える: 古い表はすぐ消え、同じ fileId にバッジは出ない。新しい文書の表に入れ替わる
    r.documentId = "DOC2";
    r.entities = json::array({ EntityJson(7, "modified") });
    r.overlay.SetDocumentPath(kOtherScenePath);
    check(!r.overlay.BadgeFor(5).Any() && !r.overlay.Loaded() && r.overlay.DocumentId().empty(),
          "overlay switch: switching scenes drops the old table at once (the same fileId gets no badge)");
    r.Settle();
    check(r.overlay.DocumentId() == "DOC2" && !r.overlay.BadgeFor(5).Any() && r.overlay.BadgeFor(7).status == OverlayStatus::Modified,
          "overlay switch: the new scene's table replaces the old one");

    // 古い文書の応答が切り替えのあとに届いても、表に入れない
    OverlayRig late;
    late.entities = json::array({ EntityJson(5, "added") });
    late.holdOverlay = true;
    late.overlay.SetDocumentPath(kScenePath);
    late.Settle();
    check(late.held.size() == 1, "overlay switch: (setup) the overlay request for scene A is in flight");
    late.holdOverlay = false;
    late.documentId = "DOC2";
    late.entities = json::array({ EntityJson(7, "added") });
    late.overlay.SetDocumentPath(kOtherScenePath);
    FeedJson(*late.s, OkReply(late.held[0]["id"].get<uint64_t>(), late.OverlayResult(late.held[0])));
    late.session.Poll();
    check(!late.overlay.BadgeFor(5).Any(), "overlay switch: a late answer for the previous scene is dropped");
    late.Settle();
    check(late.overlay.BadgeFor(7).status == OverlayStatus::Added && !late.overlay.BadgeFor(5).Any(), "overlay switch: only the open scene's changes show");

    // 切断: 取り直しはつながり直してから
    OverlayRig lost;
    lost.entities = json::array({ EntityJson(5, "added") });
    lost.overlay.SetDocumentPath(kScenePath);
    lost.Settle();
    const size_t before = lost.Count(trackop::kSceneOverlay);
    lost.s->peerClosed = true;
    lost.session.Poll();
    lost.s->nowMs += 10;
    lost.session.Poll();
    check(lost.session.State() != TrackState::Ready && lost.Count(trackop::kSceneOverlay) == before, "overlay reconnect: nothing is fetched while disconnected");
}

// AI Changes 窓: 選んだ Revision の scene_diff を全ページ取り、帯と一覧を描いて崩れない (ヘッドレスの 1 フレーム)
void TestSceneChangesWindow(const Check& check)
{
    OverlayRig r;
    r.entities = json::array({ EntityJson(5, "added", "Box"), EntityJson(6, "modified"), EntityJson(9, "removed") });
    r.overlay.SetDocumentPath(kScenePath);
    r.session.Model().SetInterest(kTrackAllParts);
    r.session.Model().SelectRevision(7);
    r.Settle();
    const ChangeModel& model = r.session.Model();
    check(model.SceneChangesComplete() && !model.SceneChangesFailed() && model.SceneChanges().size() == 3 && model.SceneChanges()[2].changeType == "unparsed"
              && !model.SceneChanges()[2].hasFileId && model.SceneChanges()[0].fileId == 5 && model.SceneChanges()[1].afterValue.empty(),
          "scene changes: the model follows describe.sceneSummary and fetches every scene_diff page");
    check(r.Count(trackop::kSceneDiff) == 2 && r.Last(trackop::kSceneDiff)["args"].value("cursor", std::string()) == "2", "scene changes: the second page is asked with the first page's cursor");

    AiChangesWindow window;
    window.open = true;
    AiChangesHost host;
    host.allowTracking = true;
    host.enabled = true;
    host.projectRoot = kRoot;
    InBothLanguages([&](Lang lang) {
        WithHeadlessImGuiFrame([&] { window.OnImGui(r.session, host); });
        check(window.HasActivation() == false,
              lang == Lang::En ? "scene changes: the window draws the scene band and the change list without trouble (English)"
                               : "scene changes: the window draws the scene band and the change list without trouble (Japanese)");
    });
    check(r.overlay.Counts().added == 1 && r.overlay.Counts().removed == 1, "scene changes: the band's counts come from the overlay table");
    // 別の Revision を選ぶと、前の一覧は捨てる
    const uint64_t serial = model.SceneChangesSerial();
    r.session.Model().SelectRevision(8);
    check(model.SceneChanges().empty() && model.SceneChangesSerial() != serial && !model.SceneChangesComplete(),
          "scene changes: selecting another revision drops the previous list at once");
}

void TestDocumentFailure(const Check& check)
{
    OverlayRig r;
    r.failDocument = true;
    r.overlay.SetDocumentPath(kScenePath);
    r.Settle();
    check(r.Count(trackop::kDocumentForPath) == 1 && r.Count(trackop::kSceneOverlay) == 0 && r.overlay.DocumentId().empty(),
          "overlay document: a refused document_for_path leaves the table empty and is not retried in a tight loop");
    r.s->nowMs += kOverlayDocumentRetryMs;
    r.failDocument = false;
    r.Settle();
    check(r.Count(trackop::kDocumentForPath) == 2 && r.overlay.DocumentId() == "DOC1" && r.Count(trackop::kSceneOverlay) == 1,
          "overlay document: after the retry interval the id is asked again and the overlay follows");
}

void TestDisabled(const Check& check)
{
    OverlayRig off(false);
    off.overlay.SetDocumentPath(kScenePath);
    for (int i = 0; i < 20; ++i) {
        off.s->nowMs += 1000;
        off.session.Poll();
    }
    check(off.s->c.Total() == 0 && !off.overlay.BadgeFor(5).Any(), "overlay disabled: with tracking off (or a non-interactive launch) no I/O is touched and the table stays empty");

    OverlayRig plain;
    plain.overlay.SetDocumentPath(L"C:\\mye_track_selftest\\proj\\assets\\notes.txt");
    plain.Settle();
    check(plain.Count(trackop::kDocumentForPath) == 0 && plain.Count(trackop::kSceneOverlay) == 0, "overlay: a path that is not a scene / prefab / actor is never asked about");

    // 強調が切で窓も閉じていれば、取りに行かない
    OverlayRig quiet;
    quiet.overlay.SetShow(false);
    quiet.overlay.SetDocumentPath(kScenePath);
    quiet.Settle();
    check(quiet.Count(trackop::kDocumentForPath) == 0 && quiet.Count(trackop::kSceneOverlay) == 0, "overlay: with the highlight off and the window closed nothing is fetched");
    quiet.overlay.SetWindowOpen(true);
    quiet.Settle();
    check(quiet.Count(trackop::kSceneOverlay) == 1, "overlay: an open AI Changes window fetches even with the highlight off");
}

// ---- 「ここまで確認済みにする」 ----

void TestAcknowledge(const Check& check)
{
    OverlayRig r;
    r.entities = json::array({ EntityJson(5, "added"), EntityJson(6, "modified") });
    r.latest = 40;
    std::map<std::string, int64_t> sunk;
    int sinkCalls = 0;
    r.overlay.SetAckSink([&](const std::map<std::string, int64_t>& acks) {
        sunk = acks;
        ++sinkCalls;
    });
    r.overlay.SetDocumentPath(kScenePath);
    r.Settle();
    check(r.overlay.BadgeFor(5).Any() && r.overlay.LatestRevisionId() == 40, "overlay ack: (setup) badges are shown and the latest revision is known");

    r.overlay.AcknowledgeUpTo();
    check(!r.overlay.BadgeFor(5).Any() && !r.overlay.BadgeFor(6).Any() && r.overlay.HasAck() && sinkCalls == 1 && sunk.at("DOC1") == 40,
          "overlay ack: marking as seen removes the earlier badges at once and hands the mark to the settings sink");
    r.entities = json::array({ EntityJson(8, "modified") }); // 確認のあとの新しい変更
    r.Settle();
    check(r.Last(trackop::kSceneOverlay)["args"].value("afterRevisionId", int64_t(0)) == 40 && r.overlay.BadgeFor(8).status == OverlayStatus::Modified
              && !r.overlay.BadgeFor(5).Any(),
          "overlay ack: later fetches ask only for revisions after the mark; a new change still gets its badge");

    // サービスへは読み取りの op しか送らない (記録の op は 1 つも出ない)
    std::set<std::string> ops;
    for (const std::string& w : r.s->written) {
        ops.insert(json::parse(w, nullptr, false).value("op", std::string()));
    }
    const std::set<std::string> allowed = { trackop::kHello, trackop::kSubscribe, trackop::kStatus, trackop::kDocumentForPath, trackop::kSceneOverlay };
    check(std::includes(allowed.begin(), allowed.end(), ops.begin(), ops.end()), "overlay ack: nothing but read ops is ever sent (the mark is display-only, never recorded)");

    // 設定へ保存して読み直しても同じ
    const fs::path dir = fs::temp_directory_path() / (L"mye_overlay_ack_" + std::to_wstring(::GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    EditorSettings saved;
    saved.Load(dir.wstring());
    saved.aiChangeAcks = sunk;
    saved.Save();
    EditorSettings loaded;
    loaded.Load(dir.wstring());
    check(loaded.aiChangeAcks == sunk && loaded.aiChangeAcks.at("DOC1") == 40, "overlay ack: the mark round-trips through editor_settings.json");
    OverlayRig again;
    again.overlay.LoadAcks(loaded.aiChangeAcks);
    again.entities = json::array({ EntityJson(8, "modified") });
    again.overlay.SetDocumentPath(kScenePath);
    again.Settle();
    check(again.Last(trackop::kSceneOverlay)["args"].value("afterRevisionId", int64_t(0)) == 40 && again.overlay.HasAck(),
          "overlay ack: a fresh editor session starts from the saved mark");
    fs::remove_all(dir, ec);

    // 全部もう一度表示
    r.overlay.ClearAck();
    r.Settle();
    check(!r.overlay.HasAck() && !r.Last(trackop::kSceneOverlay)["args"].contains("afterRevisionId") && sinkCalls == 2,
          "overlay ack: showing everything again drops the mark and refetches without afterRevisionId");
}

// ---- Hierarchy の描画 (ヘッドレス) ----

struct SceneRig {
    Scene scene;
    World& world = scene.GetWorld();
    GameObject root = scene.CreateGameObjectTracked("Root");
    GameObject child = scene.CreateGameObjectTracked("Child");
    GameObject grand = scene.CreateGameObjectTracked("Grand");
    GameObject other = scene.CreateGameObjectTracked("Other");
    GameObject plain = scene.CreateGameObject("NoFileId"); // fileId を持たない (表示のために採番してはいけない)

    SceneRig()
    {
        world.SetParent(child.Id(), root.Id());
        world.SetParent(grand.Id(), child.Id());
        world.ApplyStructuralChanges();
    }
    uint64_t Fid(const GameObject& g) { return world.GetComponent<FileIdComponent>(g.Id())->value; }
};

struct WorldFingerprint {
    uint64_t hash = 0;
    uint32_t alive = 0;
    size_t archetypes = 0;
    bool operator==(const WorldFingerprint&) const = default;
};

WorldFingerprint Fingerprint(World& world)
{
    return { HashWorld(world), world.AliveCount(), world.Archetypes().size() };
}

void DrawHierarchy(HierarchyWindow& window, SceneRig& rig, Selection& selection, UndoStack& undo)
{
    EngineContext ctx;
    ctx.scene = &rig.scene;
    WithHeadlessImGuiFrame([&] { window.OnImGui(ctx, selection, undo); });
}

void TestHierarchyBadges(const Check& check)
{
    SceneRig rig;
    Selection selection;
    UndoStack undo;
    OverlayRig r;
    r.entities = json::array({ EntityJson(static_cast<int64_t>(rig.Fid(rig.grand)), "added", "Grand"),
                               EntityWithField(static_cast<int64_t>(rig.Fid(rig.other)), "Light", "intensity"), EntityJson(9999, "removed") });
    r.overlay.SetDocumentPath(kScenePath);
    r.Settle();
    check(r.overlay.NeedsAncestorRebuild(), "hierarchy badges: a fresh table asks for the descendant aggregate once");
    r.overlay.RebuildAncestors(rig.scene);
    check(!r.overlay.NeedsAncestorRebuild(), "hierarchy badges: the aggregate is built once per refresh, not per frame");
    check(r.overlay.BadgeFor(rig.Fid(rig.root)).descendantOnly && r.overlay.BadgeFor(rig.Fid(rig.child)).descendantOnly
              && r.overlay.BadgeFor(rig.Fid(rig.root)).status == OverlayStatus::None,
          "hierarchy badges: ancestors of a changed entity get the faint descendant mark");

    const WorldFingerprint before = Fingerprint(rig.world);

    HierarchyWindow window;
    window.SetChangeOverlay(&r.overlay);
    // 通常表示 (木。ノードは畳まれている): 変更の Other に印、畳まれた Root には子孫の薄い印 (見えない Grand の変更を知らせる)
    DrawHierarchy(window, rig, selection, undo);
    check(window.DrawnBadgeCount() == 1 && window.DrawnFaintBadgeCount() == 1,
          "hierarchy badges: the tree view draws a solid badge for a changed row and a faint one on a collapsed ancestor");
    // 検索表示 (別の経路): 名前に r を含む Root / Grand / Other
    window.SetSearchText("r");
    DrawHierarchy(window, rig, selection, undo);
    check(window.DrawnBadgeCount() == 2 && window.DrawnFaintBadgeCount() == 1, "hierarchy badges: the search view (a separate drawing path) draws its badges too");
    window.SetSearchText("");

    // 強調が切なら何も描かない
    r.overlay.SetShow(false);
    DrawHierarchy(window, rig, selection, undo);
    check(window.DrawnBadgeCount() == 0 && window.DrawnFaintBadgeCount() == 0, "hierarchy badges: with the highlight off nothing is drawn");
    r.overlay.SetShow(true);

    // World は変わらない: 描いたあとも同じ。fileId の無いエンティティに FileIdComponent が付かない
    const WorldFingerprint after = Fingerprint(rig.world);
    check(before == after, "hierarchy badges: drawing with badges leaves HashWorld, the entity count and the archetype count unchanged");
    check(rig.world.GetComponent<FileIdComponent>(rig.plain.Id()) == nullptr, "hierarchy badges: an entity without a fileId did not get one (the overlay never calls EnsureFileId)");

    // 空の表と、バッジを描いた表で World が同じ (表示の有無で World が変わらない)
    SceneRig twin;
    HierarchyWindow plainWindow;
    OverlayRig none;
    plainWindow.SetChangeOverlay(&none.overlay);
    Selection selection2;
    UndoStack undo2;
    DrawHierarchy(plainWindow, twin, selection2, undo2);
    check(Fingerprint(twin.world) == Fingerprint(rig.world) && plainWindow.DrawnBadgeCount() == 0,
          "hierarchy badges: a world drawn with an empty overlay equals one drawn with badges");

    // 別の文書の変更は、同じ fileId でもバッジが出ない
    r.documentId = "DOC2";
    r.overlay.SetDocumentPath(kOtherScenePath);
    r.entities = json::array();
    r.Settle();
    DrawHierarchy(window, rig, selection, undo);
    check(window.DrawnBadgeCount() == 0 && window.DrawnFaintBadgeCount() == 0, "hierarchy badges: changes recorded for another document never badge this scene's entities");
}

// Play 中に強調を切り替えても、同じ Tick 数のあとで HashWorld が変わらない (切り替えと描画は World に触れない)
void TestPlayModeInvariance(const Check& check)
{
    auto step = [](SceneRig& rig, int tick) {
        // 決定的な「1 Tick」の代わり: 位置を Tick 数から決める
        rig.child.SetLocalPosition(static_cast<float>(tick) * 0.5f, 1.0f, 2.0f);
        rig.grand.SetLocalPosition(1.0f, static_cast<float>(tick) * 0.25f, 3.0f);
    };
    SceneRig withOverlay;
    SceneRig without;
    Selection selection;
    UndoStack undo;
    OverlayRig r;
    r.entities = json::array({ EntityJson(static_cast<int64_t>(withOverlay.Fid(withOverlay.grand)), "added") });
    r.overlay.SetDocumentPath(kScenePath);
    r.Settle();
    r.overlay.RebuildAncestors(withOverlay.scene);
    HierarchyWindow window;
    window.SetChangeOverlay(&r.overlay);
    for (int tick = 0; tick < 120; ++tick) {
        step(withOverlay, tick);
        step(without, tick);
        withOverlay.world.ApplyStructuralChanges();
        without.world.ApplyStructuralChanges();
        r.overlay.SetShow((tick / 7) % 2 == 0); // 切り替えを繰り返す
        if (tick % 10 == 0) {
            DrawHierarchy(window, withOverlay, selection, undo);
        }
    }
    check(Fingerprint(withOverlay.world) == Fingerprint(without.world), "play invariance: toggling and drawing the highlight over 120 ticks leaves HashWorld identical to a run without it");
}

// ---- 一覧のクリックと選択の同期 ----

void TestSelectionSync(const Check& check)
{
    SceneRig rig;
    Selection selection;
    OverlayRig r;
    r.overlay.SetDocumentPath(kScenePath);
    r.Settle();

    AiChangesHost host;
    host.selectEntity = [&](uint64_t fileId) { return SelectEntityByFileId(rig.scene, selection, fileId); };
    AiChangesWindow window;

    TrackSceneChange change;
    change.hasFileId = true;
    change.fileId = static_cast<int64_t>(rig.Fid(rig.grand));
    change.changeType = "FieldModified";
    change.documentId = "DOC1";
    change.resolvedDocumentId = "DOC1";
    check(window.ActivateSceneChange(change, r.overlay, host) == SceneChangeActivation::Selected && selection.primary == rig.Fid(rig.grand)
              && selection.Contains(rig.Fid(rig.grand)),
          "selection sync: clicking a row of the open scene selects that entity (Hierarchy and Inspector follow the Selection)");
    // 解決後の ID が一致すれば、生の ID が古い仮の ID でも同じ文書
    change.documentId = "PROVISIONAL";
    selection.Clear();
    check(window.ActivateSceneChange(change, r.overlay, host) == SceneChangeActivation::Selected, "selection sync: a row whose raw id was inherited by the open document still selects");

    // 開いていない文書: 選択は変わらない
    selection.SelectOnly(rig.Fid(rig.other));
    TrackSceneChange elsewhere = change;
    elsewhere.documentId = elsewhere.resolvedDocumentId = "NOT_OPEN";
    check(window.ActivateSceneChange(elsewhere, r.overlay, host) == SceneChangeActivation::NotOpen && selection.primary == rig.Fid(rig.other),
          "selection sync: a row of a scene that is not open leaves the selection alone");
    // ライブに居ない (削除された) エンティティ: 選択は変わらない
    TrackSceneChange gone = change;
    gone.fileId = 987654;
    check(window.ActivateSceneChange(gone, r.overlay, host) == SceneChangeActivation::EntityMissing && selection.primary == rig.Fid(rig.other),
          "selection sync: a deleted entity cannot be selected and the selection is unchanged");
    // 文書全体の変更
    TrackSceneChange whole;
    whole.documentId = whole.resolvedDocumentId = "DOC1";
    whole.changeType = "FieldModified";
    check(window.ActivateSceneChange(whole, r.overlay, host) == SceneChangeActivation::NoEntity && selection.primary == rig.Fid(rig.other),
          "selection sync: a whole-document change names no entity and selects nothing");
    // アセット選択とは排他 (Selection::SelectOnly の約束)
    selection.SelectAsset(L"C:\\x.png");
    window.ActivateSceneChange(change, r.overlay, host);
    check(!selection.HasAsset() && selection.primary == rig.Fid(rig.grand), "selection sync: selecting an entity clears an asset selection");
}


// ---- Inspector の Field 差分 / Scene View のワイヤ箱 / 色の設定 (M104m2) ----

json ComponentJson(const char* name, const char* status, json fields)
{
    return { { "name", name }, { "status", status }, { "fields", std::move(fields) } };
}

json FieldJson(const char* name, const char* before, const char* after, bool overrideField = false)
{
    return { { "name", name }, { "before", before }, { "after", after }, { "lastRevisionId", 3 }, { "changeType", overrideField ? "PrefabOverrideChanged" : "FieldModified" },
             { "override", overrideField }, { "oversize", false } };
}

void DrawInspector(InspectorWindow& window, SceneRig& rig, Selection& selection, UndoStack& undo)
{
    EngineContext ctx;
    ctx.scene = &rig.scene;
    AssetPreviewCache preview;
    WithHeadlessImGuiFrame([&] { window.OnImGui(ctx, selection, undo, preview, false); });
}

// 文面: 変更前・変更後の値が字句のまま出て、長い値は切る。変わっていないフィールドには印が引けない
void TestFieldMarkText(const Check& check)
{
    OverlayEntity entity;
    OverlayComponent light;
    light.name = "Light";
    light.status = OverlayStatus::Modified;
    OverlayField intensity;
    intensity.name = "intensity";
    intensity.before = "1.0";
    intensity.after = "2.5";
    intensity.hasBefore = intensity.hasAfter = true;
    light.fields.push_back(intensity);
    OverlayField color;
    color.name = "color";
    color.before = std::string("\"") + std::string(400, 'x') + "\"";
    color.hasBefore = color.hasAfter = true;
    color.after = "\"y\"";
    light.fields.push_back(color);
    OverlayField ov;
    ov.name = "range";
    ov.overrideField = true;
    ov.hasAfter = true;
    ov.after = "9";
    light.fields.push_back(ov);
    entity.components.push_back(light);
    OverlayComponent fresh;
    fresh.name = "Camera";
    fresh.status = OverlayStatus::Added;
    entity.components.push_back(fresh);

    check(FindOverlayField(&entity, "Light", "intensity") != nullptr && FindOverlayField(&entity, "Light", "range") != nullptr
              && FindOverlayField(&entity, "Light", "shadows") == nullptr && FindOverlayField(&entity, "Other", "intensity") == nullptr
              && FindOverlayField(nullptr, "Light", "intensity") == nullptr,
          "inspector marks: only a changed field of a changed component can be looked up (an unchanged one has no mark)");
    InBothLanguages([&](Lang lang) {
        const bool en = lang == Lang::En;
        const std::string tip = FieldMarkTooltip(*FindOverlayField(&entity, "Light", "intensity"));
        check(tip.find(en ? "Before: 1.0" : "変更前: 1.0") != std::string::npos && tip.find(en ? "After: 2.5" : "変更後: 2.5") != std::string::npos,
              en ? "inspector marks: the tooltip shows the before and after values verbatim (English)" : "inspector marks: the tooltip shows the before and after values verbatim (Japanese)");
        const std::string clipped = FieldMarkTooltip(*FindOverlayField(&entity, "Light", "color"));
        check(clipped.size() < 400 && clipped.find("...") != std::string::npos, "inspector marks: a long value is clipped");
        const std::string overridden = FieldMarkTooltip(*FindOverlayField(&entity, "Light", "range"));
        check(overridden != FieldMarkTooltip(intensity) && overridden.find(en ? "(none)" : "(なし)") != std::string::npos,
              "inspector marks: a prefab override change is worded as one, and a missing before value reads as none");
        const std::string managed = ComponentMarkTooltip(light, true);
        check(managed.find("intensity: 1.0") != std::string::npos && managed.find("range") != std::string::npos,
              "inspector marks: a C# component tooltip lists the changed fields with their before values");
        check(ComponentMarkTooltip(fresh, true) != ComponentMarkTooltip(light, false) && !ComponentMarkTooltip(fresh, true).empty(),
              "inspector marks: an added component reads differently from a modified one");
    });
}

// 実際の Inspector を描いて、印の数・World が変わらないこと・強調が切なら出ないことを確かめる
void TestInspectorMarks(const Check& check)
{
    SceneRig rig;
    Selection selection;
    UndoStack undo;
    selection.SelectOnly(rig.Fid(rig.child));
    OverlayRig r;
    json entity = EntityJson(static_cast<int64_t>(rig.Fid(rig.child)), "modified", "Child");
    entity["components"] = json::array({ ComponentJson("LocalTransform", "modified", json::array({ FieldJson("position", "[0,0,0]", "[1,2,3]") })) });
    r.entities = json::array({ entity });
    r.overlay.SetDocumentPath(kScenePath);
    r.Settle();

    const WorldFingerprint before = Fingerprint(rig.world);
    InspectorWindow window;
    window.SetChangeOverlay(&r.overlay);
    DrawInspector(window, rig, selection, undo);
    check(window.ChangeMarks().FieldMarks() == 1 && window.ChangeMarks().ComponentMarks() == 1,
          "inspector marks: the changed field and its component header get a mark; the other fields get none");
    check(Fingerprint(rig.world) == before && rig.world.GetComponent<FileIdComponent>(rig.plain.Id()) == nullptr,
          "inspector marks: drawing the Inspector with marks leaves HashWorld, the entity count and the archetype count unchanged and adds no fileId");

    // 表に無いエンティティを選ぶと印は出ない
    selection.SelectOnly(rig.Fid(rig.other));
    DrawInspector(window, rig, selection, undo);
    check(window.ChangeMarks().FieldMarks() == 0 && window.ChangeMarks().ComponentMarks() == 0, "inspector marks: an entity that is not in the table gets no marks");
    selection.SelectOnly(rig.Fid(rig.child));

    // 強調が切なら出ない
    r.overlay.SetShow(false);
    DrawInspector(window, rig, selection, undo);
    check(window.ChangeMarks().FieldMarks() == 0 && window.ChangeMarks().ComponentMarks() == 0, "inspector marks: with the highlight off nothing is drawn");
    r.overlay.SetShow(true);

    // 空の表と、印を描いた表で World が同じ
    SceneRig twin;
    Selection selection2;
    selection2.SelectOnly(twin.Fid(twin.child));
    UndoStack undo2;
    InspectorWindow plain;
    OverlayRig none;
    plain.SetChangeOverlay(&none.overlay);
    DrawInspector(plain, twin, selection2, undo2);
    check(plain.ChangeMarks().FieldMarks() == 0 && Fingerprint(twin.world) == Fingerprint(rig.world),
          "inspector marks: a world drawn with an empty overlay equals one drawn with marks");

    // 追加されたコンポーネントは見出しだけ (フィールドの差分が無い)
    entity["components"] = json::array({ ComponentJson("LocalTransform", "added", json::array()) });
    r.entities = json::array({ entity });
    r.documentId = "DOC2";
    r.overlay.SetDocumentPath(kOtherScenePath);
    r.Settle();
    DrawInspector(window, rig, selection, undo);
    check(window.ChangeMarks().ComponentMarks() == 1 && window.ChangeMarks().FieldMarks() == 0, "inspector marks: an added component gets a header mark and no field marks");
}

// 候補の選び方 (純粋): 見えているものだけ、近い順、同じ距離は fileId 順、上限で切って省いた数を返す
void TestChangeBoxSelection(const Check& check)
{
    std::vector<ChangeBoxCandidate> all;
    auto add = [&](uint64_t fid, bool visible, float depth) {
        ChangeBoxCandidate c;
        c.fileId = fid;
        c.visible = visible;
        c.depth = depth;
        all.push_back(c);
    };
    add(9, true, 5.0f);
    add(2, true, 5.0f);
    add(7, false, 0.5f); // 近いが見えていない
    add(4, true, 1.0f);
    add(5, true, 9.0f);
    const ChangeBoxSelection pick = SelectChangeBoxes(all, 3);
    check(pick.chosen.size() == 3 && pick.omitted == 1 && pick.chosen[0].fileId == 4 && pick.chosen[1].fileId == 2 && pick.chosen[2].fileId == 9,
          "change boxes: visible ones only, nearest first, ties by fileId; the visible one over the limit is counted as omitted");
    const ChangeBoxSelection none = SelectChangeBoxes(all, 0);
    check(none.chosen.empty() && none.omitted == 4, "change boxes: a limit of 0 draws nothing");
    check(SelectChangeBoxes({}, 5).chosen.empty() && SelectChangeBoxes({}, 5).omitted == 0, "change boxes: no candidates, no boxes");
}

// 実際に積む本数: 上限以下・見えているものだけ・強調が切なら 0。World は変わらない
void TestChangeBoxesDrawn(const Check& check)
{
    using namespace DirectX;
    Scene scene;
    World& world = scene.GetWorld();
    json list = json::array();
    constexpr int kVisible = kMaxChangeWireBoxes + 20;
    constexpr int kBehind = 15;
    std::vector<GameObject> objects;
    for (int i = 0; i < kVisible + kBehind; ++i) {
        GameObject g = scene.CreateGameObjectTracked("E");
        auto* wm = world.GetComponent<WorldMatrixComponent>(g.Id());
        const bool behind = i >= kVisible;
        wm->value._41 = static_cast<float>(i % 20) * 0.1f;
        wm->value._42 = static_cast<float>((i / 20) % 5) * 0.1f;
        wm->value._43 = behind ? -300.0f : static_cast<float>(i) * 0.05f; // カメラは z = -200 から原点を見る。-300 はカメラの後ろ
        list.push_back(EntityJson(static_cast<int64_t>(world.GetComponent<FileIdComponent>(g.Id())->value), i % 2 == 0 ? "added" : "modified"));
        objects.push_back(g);
    }
    world.ApplyStructuralChanges();
    list.push_back(EntityJson(987654321, "added")); // ライブに居ない fileId は候補にならない
    list.push_back(EntityJson(987654322, "removed"));

    OverlayRig r;
    r.entities = list;
    r.overlay.SetDocumentPath(kScenePath);
    r.Settle();

    XMFLOAT4X4 view;
    XMFLOAT4X4 proj;
    XMStoreFloat4x4(&view, XMMatrixLookAtLH(XMVectorSet(0.0f, 0.0f, -200.0f, 1.0f), XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)));
    XMStoreFloat4x4(&proj, XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), 16.0f / 9.0f, 0.1f, 1000.0f));

    SceneViewWindow window;
    window.SetChangeOverlay(&r.overlay);
    EngineContext ctx;
    ctx.scene = &scene;
    const WorldFingerprint before = Fingerprint(world);

    window.AppendChangeBoxes(ctx, view, proj);
    check(window.ChangeBoxesDrawn() == kMaxChangeWireBoxes && window.ChangeBoxesOmitted() == kVisible - kMaxChangeWireBoxes,
          "change boxes: at most the limit are drawn; only visible entities count (not the ones behind the camera or missing from the scene); the rest is reported as omitted");
    check(r.overlay.BoxStats().drawn == kMaxChangeWireBoxes && r.overlay.BoxStats().omitted == kVisible - kMaxChangeWireBoxes,
          "change boxes: the omitted count reaches the overlay, where the AI Changes band shows it");
    check(Fingerprint(world) == before, "change boxes: building the boxes leaves HashWorld, the entity count and the archetype count unchanged");

    // 純粋な側: 先頭は決定的で、表の走査順に依らない
    std::vector<ChangeBoxCandidate> candidates;
    CollectChangeBoxCandidates(scene, r.overlay, view, proj, candidates);
    check(candidates.size() == static_cast<size_t>(kVisible + kBehind), "change boxes: candidates are the changed entities that live in the scene (added and modified)");
    const ChangeBoxSelection forward = SelectChangeBoxes(candidates, 10);
    std::reverse(candidates.begin(), candidates.end());
    const ChangeBoxSelection backward = SelectChangeBoxes(candidates, 10);
    bool same = forward.chosen.size() == backward.chosen.size();
    for (size_t i = 0; same && i < forward.chosen.size(); ++i) {
        same = forward.chosen[i].fileId == backward.chosen[i].fileId;
    }
    check(same, "change boxes: the chosen set does not depend on the order the table is walked");

    // 色: 追加 = 緑、更新 = 青、競合は別の赤
    check(PackOverlayRgba(r.overlay.Palette().Get(OverlayColorId::Added)) == 0x4DC766FFu && PackOverlayRgba(r.overlay.Palette().Get(OverlayColorId::Modified)) == 0x5999F2FFu
              && PackOverlayRgba(r.overlay.Palette().Get(OverlayColorId::Conflict)) != PackOverlayRgba(r.overlay.Palette().Get(OverlayColorId::Modified)),
          "change boxes: the default colors are green for added, blue for modified, and conflict has its own color");

    // 強調が切なら 0 本
    r.overlay.SetShow(false);
    window.AppendChangeBoxes(ctx, view, proj);
    check(window.ChangeBoxesDrawn() == 0 && window.ChangeBoxesOmitted() == 0, "change boxes: with the highlight off no box is drawn");
    r.overlay.SetShow(true);

    // 上限に届かなければ全部
    const ChangeBoxSelection few = SelectChangeBoxes(std::vector<ChangeBoxCandidate>(candidates.begin(), candidates.begin() + 5), kMaxChangeWireBoxes);
    check(few.omitted == 0, "change boxes: below the limit nothing is omitted");

    // Play 中に強調を切り替え、箱を作り続けても、強調の無い世界と同じ結果になる
    Scene plainScene;
    World& plainWorld = plainScene.GetWorld();
    std::vector<GameObject> plainObjects;
    for (size_t i = 0; i < objects.size(); ++i) {
        plainObjects.push_back(plainScene.CreateGameObjectTracked("E"));
    }
    plainWorld.ApplyStructuralChanges();
    for (int tick = 0; tick < 60; ++tick) {
        for (size_t i = 0; i < 8; ++i) {
            const float v = static_cast<float>(tick) * 0.5f + static_cast<float>(i);
            objects[i].SetLocalPosition(v, 1.0f, 2.0f);
            plainObjects[i].SetLocalPosition(v, 1.0f, 2.0f);
        }
        world.ApplyStructuralChanges();
        plainWorld.ApplyStructuralChanges();
        r.overlay.SetShow((tick / 5) % 2 == 0);
        window.AppendChangeBoxes(ctx, view, proj);
    }
    r.overlay.SetShow(true);
    check(world.AliveCount() == plainWorld.AliveCount() && world.Archetypes().size() == plainWorld.Archetypes().size(),
          "change boxes: toggling the highlight and building boxes during 60 ticks does not change the world's shape");
}

// 色と強調の入・切: 設定の保存・読み直し・既定に戻す。表示側は同じ Palette() を読む
void TestPaletteSettings(const Check& check)
{
    OverlayRig r;
    OverlayPalette sunkPalette;
    bool sunkShow = true;
    int sinkCalls = 0;
    r.overlay.SetPrefsSink([&](const OverlayPalette& palette, bool show) {
        sunkPalette = palette;
        sunkShow = show;
        ++sinkCalls;
    });
    check(r.overlay.Show() && r.overlay.Palette() == OverlayPalette(), "palette: the highlight is on and the colors are the defaults at first");

    OverlayPalette custom;
    custom.Set(OverlayColorId::Added, { 0.1f, 0.2f, 0.3f, 0.4f });
    custom.Set(OverlayColorId::Conflict, { 1.0f, 0.5f, 0.25f, 1.0f });
    r.overlay.SetPalette(custom);
    check(sinkCalls == 1 && sunkPalette == custom && r.overlay.Palette() == custom, "palette: changing a color hands the palette to the settings sink and every window reads the new value");
    r.overlay.SetPalette(custom);
    check(sinkCalls == 1, "palette: setting the same palette again saves nothing");
    r.overlay.SetShow(false);
    check(sinkCalls == 2 && !sunkShow && sunkPalette == custom, "palette: switching the highlight off is saved together with the colors");
    r.overlay.SetShow(false);
    check(sinkCalls == 2, "palette: switching to the same state saves nothing");

    // ドラッグ中は保存しない。離したら 1 回だけ
    OverlayPalette dragging = custom;
    dragging.Set(OverlayColorId::Modified, { 0.9f, 0.9f, 0.0f, 1.0f });
    r.overlay.PreviewPalette(dragging);
    r.overlay.PreviewPalette(dragging);
    check(sinkCalls == 2 && r.overlay.PaletteUnsaved() && r.overlay.Palette() == dragging, "palette: a color being dragged shows at once but is not saved yet");
    r.overlay.CommitPalette();
    r.overlay.CommitPalette();
    check(sinkCalls == 3 && !r.overlay.PaletteUnsaved() && sunkPalette == dragging, "palette: releasing the drag saves once");

    // 既定に戻す
    r.overlay.SetPalette(OverlayPalette());
    check(r.overlay.Palette() == OverlayPalette() && sinkCalls == 4 && sunkPalette == OverlayPalette(), "palette: reset to default restores the four colors and saves them");

    // 読み込みは受け口へ知らせない
    r.overlay.LoadPrefs(custom, true);
    check(sinkCalls == 4 && r.overlay.Palette() == custom && r.overlay.Show(), "palette: loading the saved values does not write them back");

    // editor_settings.json を通した往復。壊れた値は無視して既定のまま
    const fs::path dir = fs::temp_directory_path() / (L"mye_overlay_palette_" + std::to_wstring(::GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    EditorSettings saved;
    saved.Load(dir.wstring());
    check(saved.aiChangeShowOverlay && saved.aiChangePalette == OverlayPalette(), "palette: a fresh settings file starts from the defaults");
    saved.aiChangePalette = custom;
    saved.aiChangeShowOverlay = false;
    saved.Save();
    EditorSettings loaded;
    loaded.Load(dir.wstring());
    check(loaded.aiChangePalette == custom && !loaded.aiChangeShowOverlay, "palette: the four colors and the on/off state round-trip through editor_settings.json");
    OverlayRig fresh;
    fresh.overlay.LoadPrefs(loaded.aiChangePalette, loaded.aiChangeShowOverlay);
    check(fresh.overlay.Palette() == custom && !fresh.overlay.Show(), "palette: a new editor session starts from the saved colors and on/off state");

    {
        std::ofstream broken(dir / L"editor_settings.json", std::ios::binary);
        broken << "{\"aiChangeColorAdded\":[1,2],\"aiChangeColorModified\":\"red\",\"aiChangeColorRemoved\":[0.5,0.5,0.5,\"x\"],\"aiChangeColorConflict\":[0.0,1.0,0.0,1.0]}";
    }
    EditorSettings damaged;
    damaged.Load(dir.wstring());
    check(damaged.aiChangePalette.Get(OverlayColorId::Added) == OverlayPalette().Get(OverlayColorId::Added)
              && damaged.aiChangePalette.Get(OverlayColorId::Modified) == OverlayPalette().Get(OverlayColorId::Modified)
              && damaged.aiChangePalette.Get(OverlayColorId::Removed) == OverlayPalette().Get(OverlayColorId::Removed)
              && damaged.aiChangePalette.Get(OverlayColorId::Conflict) == OverlayColor{ 0.0f, 1.0f, 0.0f, 1.0f },
          "palette: a malformed color is ignored (the default stays) while a valid one is kept");
    fs::remove_all(dir, ec);

    // Hierarchy のバッジも同じ設定で描く (色を変えても数は同じ)
    SceneRig rig;
    Selection selection;
    UndoStack undo;
    OverlayRig badge;
    badge.entities = json::array({ EntityJson(static_cast<int64_t>(rig.Fid(rig.other)), "added") });
    badge.overlay.SetDocumentPath(kScenePath);
    badge.Settle();
    HierarchyWindow window;
    window.SetChangeOverlay(&badge.overlay);
    DrawHierarchy(window, rig, selection, undo);
    const int plainCount = window.DrawnBadgeCount();
    badge.overlay.SetPalette(custom);
    DrawHierarchy(window, rig, selection, undo);
    check(plainCount == 1 && window.DrawnBadgeCount() == 1, "palette: the Hierarchy badge is drawn with the shared palette");

    // 歯車と帯 (ヘッドレスの AI Changes 窓): 上限超えの注記を含めて英日で崩れず描ける
    AiChangesWindow band;
    band.open = true;
    AiChangesHost host;
    host.allowTracking = true;
    host.enabled = true;
    host.projectRoot = kRoot;
    badge.overlay.NoteWireBoxes(kMaxChangeWireBoxes, 12);
    InBothLanguages([&](Lang lang) {
        WithHeadlessImGuiFrame([&] { band.OnImGui(badge.session, host); });
        check(badge.overlay.BoxStats().omitted == 12, lang == Lang::En ? "palette: the band with the gear and the omitted notice draws (English)" : "palette: the band with the gear and the omitted notice draws (Japanese)");
    });
}

} // namespace

void RunSceneChangeOverlaySelfTests(const std::function<void(bool, const char*)>& check)
{
    TestParseAndFormat(check);
    TestRefresh(check);
    TestPaging(check);
    TestSwitchAndStale(check);
    TestDocumentFailure(check);
    TestSceneChangesWindow(check);
    TestDisabled(check);
    TestAcknowledge(check);
    TestHierarchyBadges(check);
    TestPlayModeInvariance(check);
    TestSelectionSync(check);
    TestFieldMarkText(check);
    TestInspectorMarks(check);
    TestChangeBoxSelection(check);
    TestChangeBoxesDrawn(check);
    TestPaletteSettings(check);
}

} // namespace mye
