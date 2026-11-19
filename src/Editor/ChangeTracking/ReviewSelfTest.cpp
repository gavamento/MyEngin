//====================================================================================
//                          ReviewSelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          レビュー (承認・却下・要確認・再開) の Editor 側の回帰テスト
//====================================================================================
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <Windows.h>

#include "nlohmann/json.hpp"

#include "Editor/App/OwnSaveRecord.h"
#include "Editor/ChangeTracking/ChangeFormat.h"
#include "Editor/ChangeTracking/ChangeModel.h"
#include "Editor/ChangeTracking/ChangeTrackingFakeIo.h"
#include "Editor/ChangeTracking/ChangeTrackingSelfTest.h"
#include "Editor/ChangeTracking/ChangeTrackingSession.h"
#include "Editor/ChangeTracking/ReviewManager.h"
#include "Editor/ChangeTracking/TrackProtocol.h"
#include "Editor/Windows/Project/AiChangesWindow.h"
#include "Engine/Core/Localization/Localization.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace mye {
namespace {

using namespace tracktest;
using Check = std::function<void(bool, const char*)>;

// 偽の I/O で Ready まで進め、書かれた要求に偽の応答を返す道具。節ごとに作って使い回さない
struct Rig {
    std::shared_ptr<FakeState> s = std::make_shared<FakeState>();
    ChangeTrackingSession session;
    ChangeModel& model = session.Model();
    size_t answered = 0;
    // 一覧
    json listItems = json::array();
    // セッションの全体を集める list (limit 200) の応答。ページごとの items。最後のページ以外は nextCursor が付く。空なら 1 ページ 0 件
    std::vector<json> collectPages;
    bool collectEndless = false;
    // describe の応答に足す項目
    json describeExtra = json::object();
    json hunkItems = json::array();
    json sceneItems = json::array();
    json sessionItems = json::array();
    // review_set の応答: failAt 番目 (1 始まり) の要求は失敗にする (0 = しない)。silent なら応答しない
    int failAt = 0;
    const char* failCode = "not_found";
    bool silent = false;
    // 応答を保留する (AnswerAll が review_set を飛ばす)。Submit 直後の 1 件だけが飛んでいる状態を見る
    bool holdReview = false;
    int reviewCalls = 0;

    Rig()
    {
        session.StartWithIo(std::make_unique<FakeIo>(s), kRoot, true);
        session.Poll();
        ReplyHelloOk(*s);
        session.Poll();
        model.SetInterest(kTrackAllParts);
    }

    size_t Count(const char* op) const { return Requests(*s, op).size(); }

    void AnswerAll()
    {
        std::string batch;
        for (; answered < s->written.size(); ++answered) {
            const json req = json::parse(s->written[answered], nullptr, false);
            const std::string op = req.value("op", std::string());
            const uint64_t id = req["id"].get<uint64_t>();
            if (op == trackop::kHello || op == trackop::kSubscribe) {
                if (op == trackop::kSubscribe) {
                    batch += OkReply(id, json::object()).dump() + "\n";
                }
                continue;
            }
            if (op == trackop::kReviewSet) {
                if (holdReview) {
                    break; // 保留: 次の AnswerAll で答える (answered はこの要求を指したまま)
                }
                ++reviewCalls;
                if (silent) {
                    continue;
                }
                if (failAt != 0 && reviewCalls == failAt) {
                    batch += ErrReply(id, failCode).dump() + "\n";
                } else {
                    batch += OkReply(id, { { "recorded", req["args"]["targets"].size() } }).dump() + "\n";
                }
                continue;
            }
            json result = json::object();
            if (op == trackop::kStatus) {
                result = { { "phase", "watching" }, { "db", { { "state", "recording" } } } };
            } else if (op == trackop::kSessions) {
                result = { { "items", sessionItems } };
            } else if (op == trackop::kList) {
                const json& args = req["args"];
                if (args.contains("sessionId") && args.value("limit", 0) == kReviewCollectPageSize) {
                    size_t page = 0;
                    if (args.contains("cursor")) {
                        page = static_cast<size_t>(std::stoul(args["cursor"].get<std::string>()));
                    }
                    const bool more = collectEndless || page + 1 < collectPages.size();
                    result = { { "items", page < collectPages.size() ? collectPages[page] : json::array() },
                               { "nextCursor", more ? json(std::to_string(page + 1)) : json() } };
                } else {
                    result = { { "items", listItems }, { "nextCursor", nullptr } };
                }
            } else if (op == trackop::kDescribe) {
                result = describeExtra;
                result["revisionId"] = req["args"]["revisionId"];
                if (!result.contains("diffMode")) {
                    result["diffMode"] = "lines";
                }
                if (!result.contains("hunks")) {
                    result["hunks"] = json::array();
                }
            } else if (op == trackop::kHunks) {
                result = { { "items", hunkItems }, { "nextCursor", nullptr } };
            } else if (op == trackop::kSceneDiff) {
                result = { { "items", sceneItems }, { "nextCursor", nullptr } };
            }
            batch += OkReply(id, result).dump() + "\n";
        }
        if (!batch.empty()) {
            s->chunks.push_back(batch);
        }
    }

    // 要求を出し切って答える (取り直しが連鎖するので数回)
    void Settle(int rounds = 6)
    {
        for (int i = 0; i < rounds; ++i) {
            s->nowMs += 1000;
            session.Poll();
            AnswerAll();
            session.Poll();
        }
    }
};

json ListRow(int64_t id, const char* path, const char* actor, const char* review)
{
    json row = { { "revisionId", id }, { "path", path }, { "changeType", "modified" }, { "actor", actor }, { "attribution", "claimed" } };
    row["review"] = review[0] == '\0' ? json() : json(review);
    return row;
}

json HunkRow(int64_t id, int64_t ordinal, const char* review)
{
    return { { "hunkId", id }, { "ordinal", ordinal }, { "oldStart", 1 + ordinal * 10 }, { "oldCount", 1 }, { "newStart", 1 + ordinal * 10 }, { "newCount", 1 },
             { "beforeText", "a\n" }, { "afterText", "b\n" }, { "contextPre", "" }, { "contextPost", "" }, { "eolOnly", false }, { "review", review } };
}

json DetailOf(bool reviewable, const char* leafKind, const char* state)
{
    json d = { { "reviewable", reviewable }, { "review", reviewable ? json(state) : json() },
               { "reviewCounts", { { "pending", 1 }, { "approved", 1 }, { "rejected", 0 }, { "needsReview", 0 } } },
               { "reviewLeaves", json::array({ { { "kind", leafKind }, { "state", "pending" } } }) },
               { "reviewHistory", json::array({ { { "reviewId", 1 }, { "scopeKind", "revision" }, { "scopeRef", "" }, { "state", "approved" }, { "note", "ok" }, { "decidedAt", 1700000000000LL } },
                                                { { "reviewId", 2 }, { "scopeKind", "hunk" }, { "scopeRef", "7" }, { "state", "rejected" }, { "note", nullptr }, { "decidedAt", 1700000001000LL } } }) },
               { "path", "a.txt" }, { "changeType", "modified" }, { "actor", reviewable ? "ai" : "editor_user" }, { "attribution", "claimed" } };
    return d;
}

std::vector<ReviewTarget> Targets(int64_t n)
{
    std::vector<ReviewTarget> out;
    for (int64_t i = 1; i <= n; ++i) {
        out.push_back(ReviewManager::ForRevision(i));
    }
    return out;
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

// ---- 対象の組み立て (純関数) ----
void TestBuild(const Check& check)
{
    // 1000 件ごとに分ける (順序を保つ)
    const std::vector<std::vector<ReviewTarget>> chunks = ReviewManager::Split(Targets(2500));
    check(chunks.size() == 3 && chunks[0].size() == 1000 && chunks[1].size() == 1000 && chunks[2].size() == 500, "review: 2500 targets are split 1000 / 1000 / 500");
    check(chunks[0].front().revisionId == 1 && chunks[1].front().revisionId == 1001 && chunks[2].back().revisionId == 2500, "review: splitting keeps the order");
    check(ReviewManager::Split(Targets(1000)).size() == 1 && ReviewManager::Split(Targets(1001)).size() == 2 && ReviewManager::Split({}).empty(),
          "review: 1000 is one call, 1001 is two, none is none");

    // 範囲の ref の形 (ipc-contract.md §4.6)
    check(ReviewManager::ForRevision(3).kind == "revision" && ReviewManager::ForRevision(3).ref.empty(), "review: a revision scope has no ref");
    check(ReviewManager::ForHunk(3, 12).ref == "12" && ReviewManager::ForEntity(3, 5).ref == "5", "review: hunk ref = hunkId, entity ref = fileId");
    check(ReviewManager::ForComponent(3, 5, "Transform").ref == "5/Transform", "review: component ref = fileId/Component");
    check(ReviewManager::ForField(3, 5, "Transform", "position").ref == "5/Transform/position", "review: field ref = fileId/Component/field");
    check(ReviewManager::ForField(3, 5, "", "name").ref == "5//name", "review: a field without a component is fileId//field");
    TrackSceneChange change;
    change.hasFileId = true;
    change.fileId = 5;
    change.component = "Transform";
    change.field = "position";
    check(ReviewManager::ForSceneChange(9, change).kind == "field", "review: a scene change row maps to its narrowest scope (field)");
    change.field.clear();
    check(ReviewManager::ForSceneChange(9, change).kind == "component", "review: a component row maps to component");
    change.component.clear();
    check(ReviewManager::ForSceneChange(9, change).kind == "entity", "review: an entity row maps to entity");
    change.hasFileId = false;
    check(ReviewManager::ForSceneChange(9, change).kind == "revision", "review: a whole-document row maps to the whole revision");

    // 引数
    const json args = ReviewManager::BuildArgs({ ReviewManager::ForRevision(1), ReviewManager::ForHunk(2, 7) }, ReviewAction::Reject, "memo");
    check(args["state"] == "rejected" && args["note"] == "memo" && args["targets"].size() == 2, "review: BuildArgs carries state, note and targets");
    check(!args["targets"][0]["scope"].contains("ref") && args["targets"][1]["scope"]["ref"] == "7" && args["targets"][1]["revisionId"] == 2,
          "review: BuildArgs puts the ref only on scopes that have one");
    check(!ReviewManager::BuildArgs({ ReviewManager::ForRevision(1) }, ReviewAction::Approve, "").contains("note"), "review: an empty note is not sent");
    check(std::string(ReviewActionWire(ReviewAction::Approve)) == "approved" && std::string(ReviewActionWire(ReviewAction::Reopen)) == "reopened"
              && std::string(ReviewActionWire(ReviewAction::NeedsReview)) == "needs_review",
          "review: actions map to the service's state names");
    // メモは 4000 文字までに切る (日本語でも文字の途中で切らない)
    std::string longNote;
    for (int i = 0; i < 4100; ++i) {
        longNote += "\xE3\x81\x82"; // あ
    }
    const std::string clipped = ReviewManager::BuildArgs({ ReviewManager::ForRevision(1) }, ReviewAction::Approve, longNote)["note"].get<std::string>();
    check(clipped.size() == 4000 * 3, "review: a long note is cut at 4000 characters, not in the middle of one");

    // 一覧の行から: 対象の行為者 (review あり) だけ。選んだのに対象外のものは数えて知らせる
    std::vector<TrackChangeRow> rows(3);
    rows[0].revisionId = 10;
    rows[0].review = "pending";
    rows[1].revisionId = 11; // editor_user: review が空
    rows[2].revisionId = 12;
    rows[2].review = "approved";
    int skipped = 0;
    const std::vector<int64_t> ids = ReviewManager::RevisionsOf(rows, { 10, 11, 12, 12, 99 }, &skipped);
    check(ids == std::vector<int64_t>({ 10, 12 }) && skipped == 2, "review: bulk targets keep only reviewable rows, once each, and count what was left out");
}

// ---- 送信: 分けて 1 件ずつ・失敗・時間切れ・切断 ----
void TestSubmit(const Check& check)
{
    {
        Rig r;
        r.holdReview = true;
        check(r.session.Review().Submit(Targets(2500), ReviewAction::Approve, "n"), "submit: a Ready connection accepts it");
        r.session.Poll();
        check(r.Count(trackop::kReviewSet) == 1 && r.session.Review().GetStatus() == ReviewManager::Status::Sending,
              "submit: only the first chunk is in flight (the next waits for its answer)");
        check(!r.session.Review().Submit(Targets(1), ReviewAction::Reject, ""), "submit: a second submit while sending is refused");
        const json first = Requests(*r.s, trackop::kReviewSet).front();
        check(first["args"]["targets"].size() == 1000 && first["args"]["note"] == "n", "submit: the first call has 1000 targets and the note");
        r.holdReview = false;
        r.Settle();
        check(r.Count(trackop::kReviewSet) == 3 && r.session.Review().GetStatus() == ReviewManager::Status::Done && r.session.Review().RecordedCount() == 2500,
              "submit: three calls in order, then Done with every target recorded");
        check(Requests(*r.s, trackop::kReviewSet)[2]["args"]["targets"].size() == 500, "submit: the last call has the remaining 500");
        check(r.Count(trackop::kList) >= 2, "submit: after Done the list is fetched again (the screen follows the service)");
    }
    {
        // 2 つ目で失敗: 前の分は記録済み・残りは送らない。画面のデータは先に変えない
        Rig r;
        r.listItems = json::array({ ListRow(5, "a.txt", "ai", "pending") });
        r.Settle();
        const std::string before = r.model.Changes().empty() ? std::string() : r.model.Changes()[0].review;
        r.failAt = 2;
        r.session.Review().Submit(Targets(2500), ReviewAction::Approve, "");
        r.session.Poll();
        r.AnswerAll();
        r.session.Poll();
        r.AnswerAll();
        r.session.Poll();
        const ReviewManager& rv = r.session.Review();
        check(rv.GetStatus() == ReviewManager::Status::Failed && rv.ErrorCode() == "not_found" && rv.RecordedCount() == 1000 && rv.TotalCount() == 2500,
              "failure: the failing call stops the rest and says how many were recorded before it");
        check(r.Count(trackop::kReviewSet) == 2, "failure: no further call is sent after a failure");
        check(!r.model.Changes().empty() && r.model.Changes()[0].review == before && before == "pending",
              "failure: the model's data is not changed by a failed review (the screen shows only what the service said)");
        InBothLanguages([&](Lang) {
            check(!ReviewErrorText("not_found", "x").empty() && !ReviewPartialText(1000, 2500).empty(), "failure: the messages exist in both languages");
        });
    }
    {
        // 時間切れ: 記録されたか分からない → 失敗として出し、取り直す
        Rig r;
        r.silent = true;
        r.Settle();
        const size_t listBefore = r.Count(trackop::kList);
        r.session.Review().Submit(Targets(3), ReviewAction::Reject, "");
        r.session.Poll();
        r.s->nowMs += 6000; // 記録の要求は 5 秒
        r.session.Poll();
        check(r.session.Review().GetStatus() == ReviewManager::Status::Failed && r.session.Review().ErrorCode() == trackerror::kTimeout,
              "timeout: no answer in 5 seconds is a visible failure");
        r.silent = false;
        r.Settle(3);
        check(r.Count(trackop::kList) > listBefore, "timeout: the list is fetched again because the review may have been recorded");
    }
    {
        // 切断・無効: I/O を呼ばず、失敗として知らせる
        auto s = std::make_shared<FakeState>();
        ChangeTrackingSession disabled;
        disabled.StartWithIo(std::make_unique<FakeIo>(s), kRoot, false);
        check(!disabled.Review().Submit(Targets(1), ReviewAction::Approve, "") && disabled.Review().GetStatus() == ReviewManager::Status::Failed
                  && disabled.Review().ErrorCode() == trackerror::kNotConnected && s->c.Total() == 0 && s->c.now == 0,
              "disabled: submitting never touches the I/O, and the failure is reported");
        AiChangesWindow window;
        window.open = true;
        AiChangesHost host;
        host.allowTracking = false;
        for (int i = 0; i < 3; ++i) {
            WithHeadlessImGuiFrame([&] { window.OnImGui(disabled, host); });
        }
        check(s->c.Total() == 0 && s->c.now == 0 && !window.RequestBulkSession(disabled, ReviewAction::Approve) && !window.ReviewRevision(disabled, ReviewAction::Approve),
              "disabled: an open window offers no review and calls no I/O");
    }
}

// ---- まとめての対象の準備 ----
void TestCollect(const Check& check)
{
    {
        Rig r;
        r.collectPages = { json::array({ ListRow(30, "a", "ai", "pending"), ListRow(29, "b", "editor_user", "") }), json::array({ ListRow(28, "c", "unknown", "approved") }) };
        r.session.Review().PrepareSession("s1");
        check(r.session.Review().GetCollect() == ReviewManager::Collect::Collecting, "collect: a session is collected over the list pages");
        r.Settle(4);
        check(r.session.Review().GetCollect() == ReviewManager::Collect::Ready && r.session.Review().PreparedRevisions() == std::vector<int64_t>({ 30, 28 })
                  && r.session.Review().PreparedSkipped() == 1,
              "collect: only AI / unknown revisions are taken, and the editor's own change is counted as left out");
        bool askedPage2 = false;
        for (const json& ask : Requests(*r.s, trackop::kList)) {
            askedPage2 = askedPage2 || (ask["args"].contains("sessionId") && ask["args"].value("cursor", std::string()) == "1");
        }
        check(askedPage2, "collect: the second page is asked with the cursor");
        check(r.session.Review().CommitPrepared(ReviewAction::Approve, "") && r.session.Review().GetCollect() == ReviewManager::Collect::None,
              "collect: committing sends the prepared set and clears the preparation");
        const json call = Requests(*r.s, trackop::kReviewSet).back()["args"];
        check(call["targets"].size() == 2 && call["targets"][0]["scope"]["kind"] == "revision" && call["state"] == "approved",
              "collect: the whole session is recorded with revision scopes");
    }
    {
        // 集めきれないときは、一部だけをレビューしない
        Rig r;
        r.collectEndless = true;
        r.session.Review().PrepareSession("s1");
        r.Settle(kReviewCollectMaxPages + 5);
        check(r.session.Review().GetCollect() == ReviewManager::Collect::Failed && !r.session.Review().CommitPrepared(ReviewAction::Approve, "")
                  && r.Count(trackop::kReviewSet) == 0,
              "collect: when the changes cannot all be collected nothing is recorded");
    }
    {
        Rig r;
        r.session.Review().PrepareRevisions({}, 3);
        check(r.session.Review().GetCollect() == ReviewManager::Collect::Ready && !r.session.Review().CommitPrepared(ReviewAction::Approve, "")
                  && r.Count(trackop::kReviewSet) == 0,
              "collect: an empty selection sends nothing");
    }
}

// ---- 文面 (英日) ----
void TestFormat(const Check& check)
{
    InBothLanguages([&](Lang lang) {
        const std::string tag = lang == Lang::En ? "en" : "ja";
        std::set<std::string> labels;
        bool allDistinct = true;
        for (const std::string& state : ReviewAggregateStates()) {
            const std::string label = ReviewStateLabel(state);
            allDistinct = allDistinct && !label.empty() && label != state && labels.insert(label).second;
        }
        check(allDistinct && labels.size() == 6, ("format: the 6 aggregate states have their own labels (" + tag + ")").c_str());
        check(ReviewStateLabel("reopened") == ReviewStateLabel("pending") && ReviewStateLabel("future") == "future" && ReviewStateLabel("").empty(),
              ("format: reopened reads as pending, unknown states fall back to the value, empty stays empty (" + tag + ")").c_str());
        TrackReviewCounts counts;
        counts.approved = 3;
        counts.rejected = 1;
        const std::string text = ReviewCountsText(counts);
        check(text.find("3") != std::string::npos && text.find("1") != std::string::npos && text.find(" / ") != std::string::npos && ReviewCountsText({}).empty(),
              ("format: counts list only the non-zero states (" + tag + ")").c_str());
        std::set<std::string> actions;
        for (const ReviewAction a : { ReviewAction::Approve, ReviewAction::Reject, ReviewAction::NeedsReview, ReviewAction::Reopen }) {
            actions.insert(ReviewActionLabel(a));
        }
        check(actions.size() == 4 && !actions.count(""), ("format: the 4 actions have labels (" + tag + ")").c_str());
        check(ReviewScopeLabel("revision", "") != ReviewScopeLabel("hunk", "7") && ReviewScopeLabel("hunk", "7").find("7") != std::string::npos
                  && ReviewScopeLabel("field", "5/Transform/position").find("Transform.position") != std::string::npos
                  && ReviewScopeLabel("field", "5//name").find("name") != std::string::npos && ReviewScopeLabel("component", "5/Transform").find("Transform") != std::string::npos
                  && ReviewScopeLabel("entity", "5").find("5") != std::string::npos,
              ("format: scopes read as hunk / entity / component / field (" + tag + ")").c_str());
        const std::string row = FormatReviewHistoryRow(json{ { "state", "rejected" }, { "scopeKind", "hunk" }, { "scopeRef", "7" }, { "note", "later" }, { "decidedAt", 1700000000000LL } });
        check(row.find(ReviewStateLabel("rejected")) != std::string::npos && row.find("7") != std::string::npos && row.find("later") != std::string::npos,
              ("format: a history row has the state, the scope and the note (" + tag + ")").c_str());
        const std::string confirm = ReviewConfirmText(12, 2, ReviewAction::Approve);
        check(confirm.find("12") != std::string::npos && confirm.find(ReviewActionLabel(ReviewAction::Approve)) != std::string::npos && confirm.find("2") != std::string::npos
                  && confirm.find('{') == std::string::npos && ReviewConfirmText(12, 0, ReviewAction::Approve).find('\n') == std::string::npos,
              ("format: the confirmation shows the count, the action and what is left out (" + tag + ")").c_str());
        check(ReviewErrorText("timeout", "").find(ReviewErrorText("not_found", "")) == std::string::npos && !ReviewErrorText("weird", "detail").empty(),
              ("format: failure messages differ by code (" + tag + ")").c_str());
        check(ReviewPartialText(0, 5).empty() && ReviewPartialText(5, 5).empty() && !ReviewPartialText(2, 5).empty() && ReviewPartialText(2, 5).find('{') == std::string::npos,
              ("format: the partial note appears only when some, not all, were recorded (" + tag + ")").c_str());
        // セッション・一覧の行
        TrackSessionRow session;
        session.provider = "claude";
        session.review = "partially_approved";
        session.reviewCounts.approved = 2;
        session.reviewCounts.pending = 1;
        const SessionRowText sessionText = FormatSessionRow(session);
        check(sessionText.review.find(ReviewStateLabel("partially_approved")) != std::string::npos && sessionText.review.find("2") != std::string::npos,
              ("format: a session row shows the aggregate and the counts (" + tag + ")").c_str());
        check(FormatSessionRow(TrackSessionRow()).review.empty(), ("format: a session without a review aggregate shows none (" + tag + ")").c_str());
        TrackChangeRow changeRow;
        check(FormatChangeRow(changeRow).review.empty(), ("format: a row that is not reviewable (editor / restore) shows no review state (" + tag + ")").c_str());
        changeRow.review = "needs_review";
        check(FormatChangeRow(changeRow).review == ReviewStateLabel("needs_review"), ("format: a change row shows the review state (" + tag + ")").c_str());
    });

    // 応答の解釈
    const json sessions = { { "items", json::array({ { { "sessionId", "s1" }, { "review", { { "state", "partially_rejected" }, { "counts", { { "pending", 1 }, { "rejected", 2 }, { "approved", 0 }, { "needsReview", 0 } } }, { "truncated", true } } } } }) } };
    const std::vector<TrackSessionRow> parsed = ChangeModel::ParseSessions(sessions);
    check(parsed.size() == 1 && parsed[0].review == "partially_rejected" && parsed[0].reviewCounts.rejected == 2 && parsed[0].reviewCounts.pending == 1 && parsed[0].reviewTruncated,
          "parse: a session's review aggregate, counts and truncation are read");
    // 統合ターミナルの ID (M106f): 文字列はそのまま、null・欠落は空 (旧サービス・旧 DB のセッション)
    const std::vector<TrackSessionRow> hosted = ChangeModel::ParseSessions(json{ { "items", json::array({
        { { "sessionId", "h1" }, { "hostTerminal", "42-1763500000-2" } }, { { "sessionId", "h2" }, { "hostTerminal", nullptr } }, { { "sessionId", "h3" } } }) } });
    check(hosted.size() == 3 && hosted[0].hostTerminal == "42-1763500000-2" && hosted[1].hostTerminal.empty() && hosted[2].hostTerminal.empty(),
          "parse: a session's hostTerminal is read, and null / missing reads as empty");
    const std::vector<TrackChangeRow> rows = ChangeModel::ParseChanges(json{ { "items", json::array({ ListRow(1, "a", "ai", "approved"), ListRow(2, "b", "editor_user", "") }) } });
    check(rows.size() == 2 && rows[0].review == "approved" && rows[1].review.empty() && ReviewManager::IsReviewable(rows[0]) && !ReviewManager::IsReviewable(rows[1]),
          "parse: a null review (not reviewable) reads as empty");
    check(ChangeModel::ParseHunks(json{ { "items", json::array({ HunkRow(3, 0, "rejected") }) } })[0].review == "rejected"
              && ChangeModel::ParseSceneChanges(json{ { "items", json::array({ { { "sceneChangeId", 1 }, { "review", "needs_review" } } }) } })[0].review == "needs_review",
          "parse: hunks and scene changes carry their review state");
}

// ---- 通知と取り直し ----
void TestNotification(const Check& check)
{
    Rig r;
    r.listItems = json::array({ ListRow(5, "a.txt", "ai", "pending") });
    r.model.SelectRevision(5);
    r.Settle();
    r.s->nowMs += 5000;
    const size_t list = r.Count(trackop::kList);
    const size_t sessions = r.Count(trackop::kSessions);
    const size_t describe = r.Count(trackop::kDescribe);
    FeedJson(*r.s, json{ { "event", "review_changed" }, { "revisionIds", json::array({ 99 }) } });
    r.session.Poll();
    check(r.Count(trackop::kList) == list + 1 && r.Count(trackop::kSessions) == sessions + 1 && r.Count(trackop::kDescribe) == describe,
          "notify: review_changed refreshes the list and the sessions, and the detail only if the selected revision is in revisionIds");
    r.AnswerAll();
    r.s->nowMs += 5000;
    FeedJson(*r.s, json{ { "event", "review_changed" }, { "revisionIds", json::array({ 5, 6 }) } });
    r.session.Poll();
    check(r.Count(trackop::kDescribe) == describe + 1, "notify: the selected revision in revisionIds refreshes its detail");
    check(Requests(*r.s, trackop::kSubscribe).back()["args"]["events"].dump().find("review_changed") != std::string::npos,
          "notify: the session subscribes to review_changed");
}

// ---- 窓 (ヘッドレスの ImGui フレーム) ----
void TestWindow(const Check& check)
{
    AiChangesHost host;
    host.allowTracking = true;
    host.enabled = true;
    host.projectRoot = kRoot;
    {
        // 行為者が editor_user の変更には操作を出さない (サービスへも送らない)
        Rig r;
        r.listItems = json::array({ ListRow(5, "e.txt", "editor_user", "") });
        r.describeExtra = DetailOf(false, "revision", "");
        r.model.SelectRevision(5);
        r.Settle();
        AiChangesWindow window;
        window.open = true;
        window.SetShowEditorChanges(true);
        for (int i = 0; i < 3; ++i) {
            WithHeadlessImGuiFrame([&] { window.OnImGui(r.session, host); });
        }
        check(!window.ReviewRevision(r.session, ReviewAction::Approve) && !window.ReviewSelectedHunk(r.session, ReviewAction::Approve)
                  && !window.ReviewSelectedScene(r.session, ReviewAction::Approve) && r.Count(trackop::kReviewSet) == 0,
              "window: an editor_user / restore change offers no review and sends nothing");
        check(!window.RequestBulkSelected(r.session, ReviewAction::Approve) && !window.BulkConfirmPending(), "window: bulk review of only the editor's own change is refused");
    }
    {
        // 単独の Revision へ
        Rig r;
        r.listItems = json::array({ ListRow(5, "a.txt", "ai", "pending"), ListRow(6, "b.txt", "ai", "approved"), ListRow(7, "c.txt", "unknown", "needs_review"),
                                    ListRow(8, "d.txt", "editor_user", "") });
        r.describeExtra = DetailOf(true, "hunk", "partially_approved");
        r.hunkItems = json::array({ HunkRow(21, 0, "approved"), HunkRow(22, 1, "pending") });
        r.describeExtra["hunks"] = json::array({ { { "ordinal", 0 } }, { { "ordinal", 1 } } });
        r.model.SelectRevision(5);
        r.Settle();
        AiChangesWindow window;
        window.open = true;
        window.SetShowEditorChanges(true);
        auto draw = [&] { WithHeadlessImGuiFrame([&] { window.OnImGui(r.session, host); }); };
        for (int i = 0; i < 3; ++i) {
            draw();
        }
        r.describeExtra["revisionId"] = 5;
        window.SetReviewNote("looks fine");
        check(window.ReviewRevision(r.session, ReviewAction::Approve), "window: the selected revision can be reviewed");
        const json call = Requests(*r.s, trackop::kReviewSet).back()["args"];
        check(call["targets"].size() == 1 && call["targets"][0]["revisionId"] == 5 && call["targets"][0]["scope"]["kind"] == "revision" && call["note"] == "looks fine"
                  && call["state"] == "approved",
              "window: the revision review carries the note and the revision scope");
        r.Settle();

        // Hunk: 選んだ Hunk の ID で送り、詳細を取り直しても選択が残る
        draw();
        window.SelectHunkIndex(r.model, 1);
        check(window.SelectedHunkId() == 22, "window: selecting a hunk remembers its id");
        check(window.ReviewSelectedHunk(r.session, ReviewAction::Reject), "window: the selected hunk can be reviewed");
        const json hunkCall = Requests(*r.s, trackop::kReviewSet).back()["args"];
        check(hunkCall["targets"][0]["scope"]["kind"] == "hunk" && hunkCall["targets"][0]["scope"]["ref"] == "22" && hunkCall["state"] == "rejected",
              "window: the hunk review uses the hunk's id");
        r.Settle();
        r.hunkItems = json::array({ HunkRow(21, 0, "approved"), HunkRow(22, 1, "rejected") });
        FeedJson(*r.s, json{ { "event", "review_changed" }, { "revisionIds", json::array({ 5 }) } });
        r.s->nowMs += 5000;
        r.Settle();
        draw();
        draw();
        check(window.GetDiffViewer().SelectedHunk() == 1 && window.GetDiffViewer().Rows().HunkBadge(1).find(ReviewStateLabel("rejected")) != std::string::npos
                  && window.GetDiffViewer().Rows().HunkBadge(0).find(ReviewStateLabel("approved")) != std::string::npos,
              "window: after the refetch the same hunk stays selected and each header shows its review state");

        // 複数選択 → まとめて (件数を出して 1 回確認。editor_user の行は対象外)
        window.ClickChange(r.model, 6, true, false);
        window.ClickChange(r.model, 8, true, false);
        check(window.MultiSelection() == std::set<int64_t>({ 5, 6, 8 }), "window: ctrl+click adds to the selection (the row shown in the detail is included)");
        window.ClickChange(r.model, 5, false, false);
        check(window.MultiSelection().empty() && r.model.SelectedRevision() == 5, "window: a plain click goes back to a single row");
        window.ClickChange(r.model, 7, false, true);
        check(window.MultiSelection() == std::set<int64_t>({ 5, 6, 7 }), "window: shift+click selects the range from the last clicked row");
        window.ClickChange(r.model, 8, true, false);
        const size_t reviewCalls = r.Count(trackop::kReviewSet);
        check(window.RequestBulkSelected(r.session, ReviewAction::Reject) && window.BulkConfirmPending(), "window: bulk review asks for a confirmation first");
        check(r.Count(trackop::kReviewSet) == reviewCalls, "window: nothing is sent before the confirmation");
        const std::vector<int64_t>& prepared = r.session.Review().PreparedRevisions();
        check(std::is_sorted(prepared.begin(), prepared.end()) && std::find(prepared.begin(), prepared.end(), 8) == prepared.end(),
              "window: the confirmation covers only reviewable rows");
        draw(); // 確認の窓を開く
        draw();
        const std::string confirm = ReviewConfirmText(prepared.size(), r.session.Review().PreparedSkipped(), ReviewAction::Reject);
        check(confirm.find(std::to_string(prepared.size())) != std::string::npos, "window: the confirmation text shows the count");
        window.CancelBulk(r.session);
        check(!window.BulkConfirmPending() && r.Count(trackop::kReviewSet) == reviewCalls && r.session.Review().GetCollect() == ReviewManager::Collect::None,
              "window: cancelling the confirmation sends nothing");
        window.ClickChange(r.model, 6, false, false);
        window.ClickChange(r.model, 7, true, false);
        check(window.RequestBulkSelected(r.session, ReviewAction::Approve) && window.ConfirmBulk(r.session), "window: confirming sends the bulk review");
        const json bulk = Requests(*r.s, trackop::kReviewSet).back()["args"];
        check(bulk["targets"].size() == 2 && bulk["state"] == "approved", "window: the bulk review is one call for all selected revisions");

        // 絞り込み: review をサービスへ渡す
        window.SetReviewFilter(r.model, "partially_approved");
        r.Settle(2);
        check(Requests(*r.s, trackop::kList).back()["args"].value("review", std::string()) == "partially_approved" && window.MultiSelection().empty(),
              "window: the review filter goes to the service and clears the multi-selection");
        draw();
    }
    {
        // セッション全体
        Rig r;
        r.sessionItems = json::array({ { { "sessionId", "s1" }, { "provider", "claude" }, { "status", "running" }, { "startedAt", 5 }, { "changeCount", 3 },
                                         { "review", { { "state", "partially_approved" }, { "counts", { { "pending", 1 }, { "approved", 2 }, { "rejected", 0 }, { "needsReview", 0 } } }, { "truncated", false } } } } });
        r.collectPages = { json::array({ ListRow(30, "a", "ai", "pending"), ListRow(31, "b", "ai", "pending") }) };
        r.Settle();
        AiChangesWindow window;
        window.open = true;
        window.SelectScope(r.model, AiChangesWindow::Scope::Session, "s1");
        for (int i = 0; i < 2; ++i) {
            WithHeadlessImGuiFrame([&] { window.OnImGui(r.session, host); });
        }
        check(window.RequestBulkSession(r.session, ReviewAction::Approve), "window: a whole session can be prepared for review");
        r.Settle(3);
        check(r.session.Review().PreparedRevisions().size() == 2 && r.Count(trackop::kReviewSet) == 0, "window: the session's changes are collected and nothing is sent yet");
        check(window.ConfirmBulk(r.session) && r.Count(trackop::kReviewSet) == 1, "window: confirming records the whole session in one call");
        for (int i = 0; i < 2; ++i) {
            WithHeadlessImGuiFrame([&] { window.OnImGui(r.session, host); });
        }
    }
    {
        // シーンの変更の行: 範囲を選んで送る
        Rig r;
        r.listItems = json::array({ ListRow(5, "a.scene.json", "ai", "pending") });
        r.describeExtra = DetailOf(true, "scene", "pending");
        r.describeExtra["sceneSummary"] = { { "documentId", "d" }, { "counts", { { "FieldModified", 1 } } } };
        r.sceneItems = json::array({ { { "sceneChangeId", 11 }, { "documentId", "d" }, { "resolvedDocumentId", "d" }, { "fileId", 5 }, { "changeType", "FieldModified" },
                                       { "component", "Transform" }, { "field", "position" }, { "beforeValue", "1" }, { "afterValue", "2" }, { "oversize", false }, { "review", "pending" } } });
        r.model.SelectRevision(5);
        r.Settle();
        AiChangesWindow window;
        window.open = true;
        for (int i = 0; i < 3; ++i) {
            WithHeadlessImGuiFrame([&] { window.OnImGui(r.session, host); });
        }
        check(!window.ReviewSelectedHunk(r.session, ReviewAction::Approve), "window: hunk review is not offered when the leaves are scene changes");
        check(!window.ReviewSelectedScene(r.session, ReviewAction::Approve), "window: a scene change must be selected first");
        window.SelectSceneChange(11);
        check(window.ReviewSelectedScene(r.session, ReviewAction::NeedsReview), "window: the selected scene change can be reviewed");
        const json a = Requests(*r.s, trackop::kReviewSet).back()["args"];
        check(a["targets"][0]["scope"]["kind"] == "field" && a["targets"][0]["scope"]["ref"] == "5/Transform/position", "window: the default scope is the narrowest (field)");
        r.Settle();
        window.SetSceneScope(1);
        window.ReviewSelectedScene(r.session, ReviewAction::Approve);
        check(Requests(*r.s, trackop::kReviewSet).back()["args"]["targets"][0]["scope"]["ref"] == "5/Transform", "window: a wider scope can be chosen (component)");
        r.Settle();
        window.SetSceneScope(2);
        window.ReviewSelectedScene(r.session, ReviewAction::Approve);
        check(Requests(*r.s, trackop::kReviewSet).back()["args"]["targets"][0]["scope"]["ref"] == "5", "window: ... and the entity");
        r.Settle();
        window.SetSceneScope(3);
        window.ReviewSelectedScene(r.session, ReviewAction::Reject);
        check(Requests(*r.s, trackop::kReviewSet).back()["args"]["targets"][0]["scope"]["kind"] == "revision", "window: ... and the whole revision");
    }
    {
        // 窓を英日で描く (落ちない)
        Rig r;
        r.listItems = json::array({ ListRow(5, "a.txt", "ai", "partially_approved") });
        r.describeExtra = DetailOf(true, "hunk", "partially_approved");
        r.hunkItems = json::array({ HunkRow(21, 0, "approved") });
        r.describeExtra["hunks"] = json::array({ { { "ordinal", 0 } } });
        r.model.SelectRevision(5);
        r.Settle();
        InBothLanguages([&](Lang) {
            AiChangesWindow window;
            window.open = true;
            for (int i = 0; i < 3; ++i) {
                WithHeadlessImGuiFrame([&] { window.OnImGui(r.session, host); });
            }
        });
        check(true, "window: the review panel draws in both languages");
        std::vector<TrackHunk> hunks = r.model.Hunks();
        std::vector<DiffHunkInput> input = ToDiffHunks(hunks);
        WithHeadlessImGuiFrame([&] { ApplyReviewBadges(input, hunks); });
        check(!hunks.empty() && !input.empty() && !input[0].badge.empty(), "window: hunk headers get a review badge");
    }
}

// ---- 自分の保存の印 (C の 1) ----
void TestOwnSave(const Check& check)
{
    const fs::path dir = fs::temp_directory_path() / (L"mye_ownsave_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path scene = dir / L"a.scene.json";
    auto write = [&](const char* text) { std::ofstream(scene, std::ios::binary | std::ios::trunc) << text; };

    write("{\"a\":1}");
    OwnSaveRecord record;
    check(!record.MatchesNow(scene.wstring()), "ownsave: nothing is noted yet, so nothing matches");
    record.Note(scene.wstring());
    check(record.MatchesNow(scene.wstring()), "ownsave: the file right after our own save matches what was noted");
    check(record.MatchesNow(L"" + scene.wstring()) && record.MatchesNow(dir.wstring() + L"/a.scene.json"), "ownsave: the path is compared by its normalized form");

    // 外から書き換えられた: 大きさが変わる
    write("{\"a\":12345}");
    check(!record.MatchesNow(scene.wstring()), "ownsave: a rewrite with a different size is not ours");
    // 大きさは同じで、更新時刻だけが違う
    write("{\"a\":11111}");
    record.Note(scene.wstring());
    const FileStamp noted = StatFileStamp(scene.wstring());
    write("{\"b\":22222}");
    fs::last_write_time(scene, fs::last_write_time(scene) + std::chrono::seconds(7), ec);
    const FileStamp later = StatFileStamp(scene.wstring());
    check(noted.valid && later.valid && noted.size == later.size && noted.writeTime != later.writeTime, "ownsave: the probe has the same size and a different time");
    check(!record.MatchesNow(scene.wstring()), "ownsave: a rewrite with the same size but a different time is not ours");

    // 純粋な判定
    FileStamp a;
    a.valid = true;
    a.size = 10;
    a.writeTime = 100;
    FileStamp b = a;
    check(OwnSaveRecord::Matches(a, b), "ownsave: the same size and time match");
    b.writeTime = 101;
    check(!OwnSaveRecord::Matches(a, b), "ownsave: a different time does not match");
    b = a;
    b.size = 11;
    check(!OwnSaveRecord::Matches(a, b), "ownsave: a different size does not match");
    b = a;
    b.valid = false;
    check(!OwnSaveRecord::Matches(a, b) && !OwnSaveRecord::Matches(b, a) && !OwnSaveRecord::Matches(b, b), "ownsave: an unreadable file never matches");
    // 別のファイルの印は使わない。消えたファイルは一致しない
    record.Note((dir / L"other.scene.json").wstring());
    write("{\"z\":1}");
    record.Note(scene.wstring());
    fs::remove(scene, ec);
    check(!record.MatchesNow(scene.wstring()), "ownsave: a deleted file does not match");
    fs::remove_all(dir, ec);
}

} // namespace

void RunReviewSelfTests(const std::function<void(bool, const char*)>& check)
{
    TestBuild(check);
    TestSubmit(check);
    TestCollect(check);
    TestFormat(check);
    TestNotification(check);
    TestWindow(check);
    TestOwnSave(check);
}

} // namespace mye
