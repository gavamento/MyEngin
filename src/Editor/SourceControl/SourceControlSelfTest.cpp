#include "Editor/SourceControl/SourceControlSelfTest.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <thread>

#include "Editor/EditorSettings.h"
#include "Editor/EditorWidgets.h" // ScmBadgeColor (M66i: バッジの色表を機械で固定する)
#include "Editor/ProjectTemplates.h"
#include "Editor/SourceControl/CollabClient.h"
#include "Editor/SourceControl/GitTransaction.h"
#include "Editor/SourceControl/PairRule.h"
#include "Editor/SourceControl/SourceControlState.h"
#include "Editor/SourceControl/StageClassifier.h"
#include "Editor/Windows/ProjectSettingsWindow.h" // InputActionsDifferFromDisk (M66k)
#include "Editor/Windows/SourceControlWindow.h"   // SaveThenCommit / ShouldClearCommitMessage (M66k)
#include "Engine/Core/Hash.h"
#include "Engine/Core/Localization.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/Project.h"
#include "Engine/Platform/InputActions.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/ImGuiTheme.h" // themeColor (M66i)

#include "imgui.h"

namespace fs = std::filesystem;

namespace mye {
namespace {

// hello の往復に許す時間。git.exe の起動 1 回分 + α (spec §4.4 のタイムアウト表と同値)
constexpr int kHelloTimeoutMs = 5000;

bool CollabRequired()
{
    const char* v = std::getenv("MYE_COLLAB_REQUIRED");
    return v && v[0] == '1';
}

// status 応答の entries を 1 件組む。**手で組んだ偽トランスクリプト**を使うのは、
// 実 git を呼ぶと「この環境の git が出す形」しか検査できず、リネーム (R) や
// 未マージ (u) のような**作るのが面倒な形**が永久に未検査で残るため
// (tools\collab\tests\porcelain.rs と同じ方針)
nlohmann::json Entry(const char* path, char index, char worktree, bool conflict = false,
                     const char* oldPath = nullptr)
{
    nlohmann::json e;
    e["path"] = path;
    e["index"] = std::string(1, index);
    e["worktree"] = std::string(1, worktree);
    e["conflict"] = conflict;
    if (oldPath != nullptr) {
        e["oldPath"] = oldPath;
    }
    return e;
}

// path をキーに行を引く (見つからなければ nullptr)
const PairedEntry* FindRow(const SourceControlModel& m, const char* path)
{
    for (const PairedEntry& e : m.entries) {
        if (e.path == path) {
            return &e;
        }
    }
    return nullptr;
}

// フォルダノードを path で引く
const ScmNode* FindNode(const SourceControlModel& m, const char* path)
{
    for (const ScmNode& n : m.nodes) {
        if (n.path == path) {
            return &n;
        }
    }
    return nullptr;
}

} // namespace

bool RunSourceControlSelfTest()
{
    MYE_LOG_INFO("==== Source control self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- (a) 応答/通知の配線 (DLL 不要) ----
    {
        CollabClient client;
        int called = 0;
        std::string gotVersion;
        const uint64_t id = client.AddPendingForTest([&](const nlohmann::json& msg) {
            ++called;
            gotVersion = msg["result"].value("gitVersion", std::string());
        });
        client.DispatchLine("{\"id\":" + std::to_string(id)
                            + ",\"ok\":true,\"result\":{\"gitVersion\":\"2.48.1\"}}");
        check(called == 1 && gotVersion == "2.48.1", "response is routed to the matching id");
        check(client.PendingCount() == 0, "the callback is removed once fired");

        // 同じ id が二度来ても二度は呼ばれない (worker の再送や CLI の連結で起きうる)
        client.DispatchLine("{\"id\":" + std::to_string(id) + ",\"ok\":true,\"result\":{}}");
        check(called == 1, "a duplicate response does not fire the callback twice");

        int events = 0;
        client.SetEventHandler([&](const nlohmann::json&) { ++events; });
        client.DispatchLine("{\"event\":\"status_changed\"}");
        check(events == 1, "event line reaches the subscriber");

        // service_error は購読者に配るだけでなく、状態を ServiceDied に落とす
        client.DispatchLine("{\"event\":\"service_error\",\"code\":\"internal_panic\",\"detail\":\"x\"}");
        check(events == 2 && client.State() == Unavailable::ServiceDied,
              "service_error switches the client to ServiceDied");

        // 壊れた行で落ちないこと (サービスが暴走しても**エディタは生き残る**)
        client.DispatchLine("{\"id\":");
        client.DispatchLine("not json at all");
        check(true, "malformed lines are survivable");
    }

    // ---- (b) MyeCollab.dll との実往復 ----
    {
        const std::wstring exeDir = GetExecutableDir();
        const fs::path dllPath = fs::path(exeDir) / L"MyeCollab.dll";
        std::error_code ec;
        if (!fs::exists(dllPath, ec)) {
            if (CollabRequired()) {
                MYE_LOG_ERROR("  FAIL: MyeCollab.dll not found but MYE_COLLAB_REQUIRED=1 "
                              "(run tools\\build_collab.bat)");
                ++failCount;
            } else {
                MYE_LOG_INFO("[selftest] SourceControl: MyeCollab.dll not found - SKIP");
            }
        } else {
            CollabClient client;
            const bool loaded = client.Load(exeDir);
            check(loaded, "MyeCollab.dll loads and the proto version matches");
            if (loaded) {
                // リポジトリでなくてよい (hello は git の所在と版しか見ない)
                const fs::path tmp = fs::temp_directory_path() / L"mye_collab_selftest";
                fs::create_directories(tmp, ec);
                check(client.Create(WideToUtf8(tmp.wstring())), "mye_collab_create returns a handle");

                bool done = false;
                bool ok = false;
                std::string gitVersion;
                std::string errCode;
                client.Request(collabop::kHello, { { "fetchIntervalMin", 5 }, { "autoFetch", false } },
                               [&](const nlohmann::json& msg) {
                                   done = true;
                                   ok = msg.value("ok", false);
                                   if (ok) {
                                       gitVersion = msg["result"].value("gitVersion", std::string());
                                   } else if (msg.contains("error")) {
                                       errCode = msg["error"].value("code", std::string());
                                   }
                               });

                const auto start = std::chrono::steady_clock::now();
                while (!done) {
                    client.Poll();
                    if (done) {
                        break;
                    }
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - start)
                                             .count();
                    if (elapsed > kHelloTimeoutMs) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                check(done, "hello gets a response within 5s");
                if (done && !ok) {
                    MYE_LOG_ERROR("  hello failed: error.code=%s", errCode.c_str());
                }
                check(ok && !gitVersion.empty(), "hello reports a non-empty gitVersion");
                MYE_LOG_INFO("  [collab] git version: %s",
                             gitVersion.empty() ? "(none)" : gitVersion.c_str());
                client.Shutdown();
                check(client.State() == Unavailable::NoService, "Shutdown unloads the dll");
            }
        }
    }

    // ---- (c1) 偽 status トランスクリプト -> 対の束ねと合成状態 ----
    {
        nlohmann::json result;
        result["branch"] = "feature/collab";
        result["upstream"] = "origin/feature/collab";
        result["head"] = "3333333333333333333333333333333333333333";
        result["ahead"] = 2;
        result["behind"] = 1;
        result["entries"] = nlohmann::json::array({
            // 本体 M + .meta M -> 1 行
            Entry("assets/textures/a.png", '.', 'M'),
            Entry("assets/textures/a.png.meta", '.', 'M'),
            // 本体 A + .meta A
            Entry("assets/models/new.glb", 'A', '.'),
            Entry("assets/models/new.glb.meta", 'A', '.'),
            // 本体 M + .meta D -> **重い方 (D) を採る**
            Entry("assets/audio/hit.wav", '.', 'M'),
            Entry("assets/audio/hit.wav.meta", 'D', '.'),
            // .meta だけが出ている (本体は無変更) -> 本体のパスで 1 行
            Entry("assets/textures/orphan.png.meta", '.', 'M'),
            // 地形: 本体 + .terrain.edit (.json を .edit に差し替えた形)
            Entry("assets/terrain/level.terrain.json", '.', 'M'),
            Entry("assets/terrain/level.terrain.edit", '.', 'M'),
            // 単独の各種
            Entry("assets/scenes/gone.scene.json", 'D', '.'),
            Entry("assets/scenes/renamed.scene.json", 'R', '.', false,
                  "assets/scenes/old.scene.json"),
            Entry("assets/untracked.txt", '?', '?'),
            Entry("assets/conflict.json", 'U', 'U', true),
            // --ignored を付けたときだけ来る '!' は「変更」ではないので一覧に出さない
            Entry("cache/build.log", '!', '!'),
        });
        const SourceControlModel m = BuildModel(result);
        check(m.valid && m.branch == "feature/collab" && m.upstream == "origin/feature/collab",
              "status: branch / upstream are carried into the model");
        check(m.ahead == 2 && m.behind == 1
                  && m.head == "3333333333333333333333333333333333333333",
              "status: ahead / behind / head are carried into the model");
        check(m.entries.size() == 9,
              "pairing: 14 entries collapse into 9 rows ('!' dropped, 4 sidecars folded)");

        const PairedEntry* png = FindRow(m, "assets/textures/a.png");
        check(png != nullptr && png->sidecars.size() == 1
                  && png->sidecars[0] == "assets/textures/a.png.meta"
                  && png->state == ChangeState::Modified && png->primaryListed,
              "pairing: .meta rides with its owner (one row, one sidecar)");

        const PairedEntry* wav = FindRow(m, "assets/audio/hit.wav");
        check(wav != nullptr && wav->state == ChangeState::Deleted,
              "pairing: the composed state is the heaviest of the pair (M + D -> D)");
        check(wav != nullptr && wav->worktreeState == ChangeState::Modified,
              "pairing: the primary keeps its own index/worktree state for staging (M66c)");

        const PairedEntry* orphan = FindRow(m, "assets/textures/orphan.png");
        check(orphan != nullptr && !orphan->primaryListed && orphan->sidecars.size() == 1,
              "pairing: a lone .meta still lists under the file it belongs to");

        const PairedEntry* terrain = FindRow(m, "assets/terrain/level.terrain.json");
        check(terrain != nullptr && terrain->sidecars.size() == 1
                  && terrain->sidecars[0] == "assets/terrain/level.terrain.edit",
              "pairing: .terrain.edit rides with .terrain.json (.json swapped for .edit)");

        const PairedEntry* ren = FindRow(m, "assets/scenes/renamed.scene.json");
        check(ren != nullptr && ren->state == ChangeState::Renamed
                  && ren->oldPath == "assets/scenes/old.scene.json",
              "status: a rename keeps its old path");
        const PairedEntry* conflict = FindRow(m, "assets/conflict.json");
        check(conflict != nullptr && conflict->conflict
                  && conflict->state == ChangeState::Conflict,
              "status: an unmerged entry outranks everything else");
        check(FindRow(m, "assets/scenes/gone.scene.json") != nullptr
                  && FindRow(m, "assets/scenes/gone.scene.json")->state == ChangeState::Deleted,
              "status: D is deleted");
        check(FindRow(m, "assets/untracked.txt") != nullptr
                  && FindRow(m, "assets/untracked.txt")->state == ChangeState::Untracked,
              "status: ? is untracked");
        check(FindRow(m, "cache/build.log") == nullptr,
              "status: ignored ('!') entries never reach the list");

        // 並びは path 昇順 (決定的)。同じリポジトリを 2 台で開いて順が違ってはいけない
        bool sorted = true;
        for (size_t i = 1; i < m.entries.size(); ++i) {
            if (!(m.entries[i - 1].path < m.entries[i].path)) {
                sorted = false;
            }
        }
        check(sorted, "status: rows are sorted by path (deterministic order)");

        // サイドカーのパス解決そのもの
        check(PrimaryPathFor("a/b.png.meta") == "a/b.png",
              "PrimaryPathFor: .meta is stripped");
        check(PrimaryPathFor("a/b.terrain.edit") == "a/b.terrain.json",
              "PrimaryPathFor: .terrain.edit maps back to .terrain.json");
        check(PrimaryPathFor("a/b.terrain.edit.meta") == "a/b.terrain.json",
              "PrimaryPathFor: a sidecar of a sidecar unwinds all the way");
        check(PrimaryPathFor("a/b.png") == "a/b.png",
              "PrimaryPathFor: a plain file is its own primary");
        check(CombineState(ChangeState::Untracked, ChangeState::Deleted) == ChangeState::Deleted
                  && CombineState(ChangeState::Conflict, ChangeState::Deleted)
                      == ChangeState::Conflict,
              "CombineState: conflict > D > R > A > M > ?");
    }

    // ---- (c2) 対の規則 (PairRule) ----
    // ★ここが M66c の中心。間違えても**画面は自然に見える** (一覧は 1 行のまま) のに、
    //   commit された中身だけが片肺になる = 他人のマシンで GUID が作り直されて
    //   シーン参照が壊れる、という形でしか露見しない
    {
        // ディスクにあることにするパスの集合 (実ファイルを置かずに条件を作る)
        std::vector<std::string> onDisk = {
            "assets/textures/a.png",
            "assets/textures/a.png.meta",
            "assets/terrain/level.terrain.json",
            "assets/terrain/level.terrain.edit",
            "assets/models/fresh.glb", // .meta がまだ無い新規アセット
            "assets/notes.txt",        // 資産ではないファイル (.meta を作らない)
        };
        auto exists = [&onDisk](const std::string& p) {
            return std::find(onDisk.begin(), onDisk.end(), p) != onDisk.end();
        };

        // (1) 本体 + 実在する .meta -> 両方 stage
        PairedEntry png;
        png.path = "assets/textures/a.png";
        png.primaryListed = true;
        png.sidecars = { "assets/textures/a.png.meta" };
        pairrule::PairPlan plan = pairrule::Collect({ png }, exists);
        check(plan.toStage.size() == 2 && plan.toStage[0] == "assets/textures/a.png"
                  && plan.toStage[1] == "assets/textures/a.png.meta"
                  && plan.toEnsureMeta.empty(),
              "PairRule: a file and its existing .meta are staged together");

        // (2) .meta が無い資産 -> toEnsureMeta に載り、生成後の .meta も toStage に入る
        PairedEntry fresh;
        fresh.path = "assets/models/fresh.glb";
        fresh.primaryListed = true;
        plan = pairrule::Collect({ fresh }, exists);
        check(plan.toEnsureMeta.size() == 1 && plan.toEnsureMeta[0] == "assets/models/fresh.glb",
              "PairRule: an asset without a .meta is queued for EnsureMeta");
        check(std::find(plan.toStage.begin(), plan.toStage.end(), "assets/models/fresh.glb.meta")
                  != plan.toStage.end(),
              "PairRule: the .meta that EnsureMeta will create is staged too");

        // (3) 資産でないファイルには .meta を作らない
        PairedEntry notes;
        notes.path = "assets/notes.txt";
        notes.primaryListed = true;
        plan = pairrule::Collect({ notes }, exists);
        check(plan.toEnsureMeta.empty() && plan.toStage.size() == 1,
              "PairRule: a non-asset file never gets a .meta invented for it");

        // (4) 地形: .terrain.edit が同居していれば一緒に stage する
        //     (status に出ていなくてもディスクにあれば渡す)
        PairedEntry terrain;
        terrain.path = "assets/terrain/level.terrain.json";
        terrain.primaryListed = true;
        plan = pairrule::Collect({ terrain }, exists);
        check(std::find(plan.toStage.begin(), plan.toStage.end(),
                        "assets/terrain/level.terrain.edit") != plan.toStage.end(),
              "PairRule: .terrain.edit rides along with .terrain.json");

        // (5) .meta だけが変わった行 (primaryListed=false) でも本体を束ねる。
        //     ★本体はディスクに実在するので渡す = 変更が無ければ git の no-op
        PairedEntry orphan;
        orphan.path = "assets/textures/a.png";
        orphan.primaryListed = false;
        orphan.sidecars = { "assets/textures/a.png.meta" };
        plan = pairrule::Collect({ orphan }, exists);
        check(plan.toStage.size() == 2, "PairRule: a lone .meta still drags its owner along");

        // (6) status にも無く、ディスクにも無いパスは渡さない
        //     ★渡すと git が pathspec エラーで**選択ごと**失敗する
        PairedEntry gone;
        gone.path = "assets/textures/vanished.png";
        gone.primaryListed = false;
        gone.sidecars = { "assets/textures/vanished.png.meta" };
        plan = pairrule::Collect({ gone }, exists);
        check(plan.toStage.size() == 1 && plan.toStage[0] == "assets/textures/vanished.png.meta",
              "PairRule: a primary that is neither listed nor on disk is left out");

        // (7) 削除された本体 (status に D で載る / ディスクには無い) は渡す
        PairedEntry deleted;
        deleted.path = "assets/textures/deleted.png";
        deleted.primaryListed = true;
        deleted.sidecars = { "assets/textures/deleted.png.meta" };
        plan = pairrule::Collect({ deleted }, exists);
        check(plan.toStage.size() == 2 && plan.toEnsureMeta.empty(),
              "PairRule: a deleted pair is staged (the deletion must reach the index)");

        // (8) 重複と並び: 同じ行を 2 回渡しても 1 回ずつ、昇順
        plan = pairrule::Collect({ png, png }, exists);
        check(plan.toStage.size() == 2, "PairRule: duplicate rows collapse");
        bool ordered = true;
        for (size_t i = 1; i < plan.toStage.size(); ++i) {
            if (!(plan.toStage[i - 1] < plan.toStage[i])) {
                ordered = false;
            }
        }
        check(ordered, "PairRule: the staged paths come out sorted (same selection, same command)");

        // (9) unstage は status に出ているものだけ / ディスクを見ない
        const std::vector<std::string> listed = pairrule::ListedPaths({ orphan, terrain });
        check(listed.size() == 2
                  && listed[0] == "assets/terrain/level.terrain.json"
                  && listed[1] == "assets/textures/a.png.meta",
              "PairRule: unstage only touches what git already listed");

        // (10) サイドカーの綴り候補そのもの (PrimaryPathFor の逆写像)
        const std::vector<std::string> cands =
            pairrule::SidecarCandidates("assets/terrain/level.terrain.json");
        check(cands.size() == 3
                  && std::find(cands.begin(), cands.end(), "assets/terrain/level.terrain.edit")
                      != cands.end()
                  && std::find(cands.begin(), cands.end(), "assets/terrain/level.terrain.json.meta")
                      != cands.end()
                  && std::find(cands.begin(), cands.end(), "assets/terrain/level.terrain.edit.meta")
                      != cands.end(),
              "PairRule: SidecarCandidates is the inverse of PrimaryPathFor");
        check(pairrule::SidecarCandidates("assets/a.png").size() == 1,
              "PairRule: a plain asset has exactly one sidecar candidate");
        check(pairrule::IsAssetPath("a/b.png") && pairrule::IsAssetPath("a/b.terrain.json")
                  && !pairrule::IsAssetPath("a/b.terrain.edit") && !pairrule::IsAssetPath("a/b.txt"),
              "PairRule: IsAssetPath follows AssetDatabase::ClassifyPath");
    }

    // ---- (d) フォルダ集約 ----
    {
        nlohmann::json result;
        result["entries"] = nlohmann::json::array({
            Entry("a/x.txt", '?', '?'),  // ? だけの兄弟
            Entry("a/y.txt", '.', 'M'),  // ? と M の混在 -> M
            Entry("b/z.txt", '?', '?'),  // ? だけ -> ?
            Entry("c/d/w.txt", 'D', '.'), // 深い階層 -> D が上まで伝わる
        });
        const SourceControlModel m = BuildModel(result);
        const ScmNode* a = FindNode(m, "a");
        const ScmNode* b = FindNode(m, "b");
        const ScmNode* c = FindNode(m, "c");
        const ScmNode* cd = FindNode(m, "c/d");
        check(a != nullptr && a->folder && a->state == ChangeState::Modified,
              "folders: a mix of ? and M shows M");
        check(b != nullptr && b->state == ChangeState::Untracked,
              "folders: only ? children show ?");
        check(cd != nullptr && cd->state == ChangeState::Deleted && c != nullptr
                  && c->state == ChangeState::Deleted,
              "folders: the heaviest child state climbs every level (a delete never hides)");
        check(m.nodes[0].children.size() == 3,
              "folders: the root holds exactly the three top level folders");
    }

    // ---- (d2) Content Browser のバッジ引き (M66i) ----
    // ★集約は PropagateFolderState (= 一覧と同じ 1 本) の結果を写すだけ。
    //   ここで検査するのは「重い方が勝つ」規則と、バッジ引きの索引が
    //   一覧と食い違わないこと
    {
        nlohmann::json result;
        result["entries"] = nlohmann::json::array({
            Entry("assets/tex/a.png", '.', 'M'),      // 変更
            Entry("assets/tex/a.png.meta", '.', 'M'), // 対 (本体に束ねる)
            Entry("assets/tex/b.png", '?', '?'),      // 未追跡 -> {M, ?} で親は M
            Entry("assets/gone/old.png", 'D', '.'),   // 削除
            Entry("assets/gone/new.png", '?', '?'),   // {D, ?} -> D (削除が勝つ)
            Entry("assets/mix/c.scene.json", '.', '.', /*conflict=*/true),
            Entry("assets/mix/d.mat.json", '.', 'M'), // {競合, M} -> 競合
            Entry("assets/terr/x.terrain.edit", '.', 'M'), // 本体は無変更のサイドカー
        });
        const SourceControlModel m = BuildModel(result);
        check(m.StateFor("assets/tex/a.png") == ChangeState::Modified,
              "badge: a changed file shows M");
        check(m.StateFor("assets/tex/a.png.meta") == ChangeState::Modified,
              "badge: the .meta sidecar borrows the badge of its owner");
        check(m.StateFor("assets/tex/b.png") == ChangeState::Untracked,
              "badge: a new file shows ?");
        check(m.StateFor("assets/terr/x.terrain.json") == ChangeState::Modified
                  && m.StateFor("assets/terr/x.terrain.edit") == ChangeState::Modified,
              "badge: .terrain.edit and .terrain.json share one badge");
        check(m.StateFor("assets/tex/never.png") == ChangeState::None,
              "badge: an unchanged file has no badge");
        // ★キーは必ず ScmPathKey を通してから引く (ディスク側は NormalizePathKey が
        //   同じ小文字化を済ませている)。通さないと大小が違うだけで引けない
        check(m.StateFor(ScmPathKey("ASSETS/Tex/A.png")) == ChangeState::Modified,
              "badge: the lookup key ignores case (git and the disk may differ)");
        check(m.FolderStateFor("assets/tex") == ChangeState::Modified,
              "badge folders: {M, ?} shows M");
        check(m.FolderStateFor("assets/gone") == ChangeState::Deleted,
              "badge folders: {D, ?} shows D");
        check(m.FolderStateFor("assets/mix") == ChangeState::Conflict,
              "badge folders: {conflict, M} shows the conflict");
        check(m.FolderStateFor("assets") == ChangeState::Conflict,
              "badge folders: the heaviest state climbs to the top folder");
        check(m.FolderStateFor("assets/empty") == ChangeState::None,
              "badge folders: a folder with no changed child has no badge");
        check(m.FolderStateFor("assets/tex/a.png") == ChangeState::None,
              "badge folders: a file is never answered by the folder lookup");
        // 索引と一覧が同じことを言っているか (二重実装の検出)
        bool sameAsRows = true;
        for (const PairedEntry& e : m.entries) {
            if (m.StateFor(e.path) != e.state) {
                sameAsRows = false;
            }
        }
        check(sameAsRows, "badge: every row of the change list answers with its own state");
    }

    // ---- (d3) バッジの色表 (spec §4.3 の確定表 + 配色ルール 3) ----
    // ★ここで固定する理由: 色は「画面を見れば分かる」ように思えて、実際には
    //   **選択行の面と同じ色になって初めて読めなくなる** (M66i round 1 の実測)。
    //   人の目でしか分からない性質のものほど、表そのものは機械で止める
    {
        // ScmBadgeColor は ? / None で ImGui::GetStyleColorVec4 (= GImGui) を通るので、
        // ヘッドレスのまま呼ぶと落ちる。**バックエンドは要らない** — 既定スタイルが
        // 入った context を 1 つ作って捨てるだけ
        ImGuiContext* const prevCtx = ImGui::GetCurrentContext();
        ImGuiContext* const tmpCtx = (prevCtx == nullptr) ? ImGui::CreateContext() : nullptr;
        auto sameColor = [](const ImVec4& a, const ImVec4& b) {
            return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
        };
        check(sameColor(ScmBadgeColor(ChangeState::Modified), themeColor::Warning)
                  && sameColor(ScmBadgeColor(ChangeState::Added), themeColor::Success)
                  && sameColor(ScmBadgeColor(ChangeState::Deleted), themeColor::Error)
                  && sameColor(ScmBadgeColor(ChangeState::Renamed), themeColor::Prefab)
                  && sameColor(ScmBadgeColor(ChangeState::Conflict), themeColor::Error),
              "badge colors: the table matches spec 4.3 (M=Warning A=Success D/conflict=Error "
              "R=Prefab)");
        // ★配色ルール 3 の再発防止。Accent は「選択・フォーカス・トグル ON」専用で、
        //   状態の意味色に混ぜると選択行の上で消える
        constexpr ChangeState kAllStates[] = {
            ChangeState::None,    ChangeState::Untracked, ChangeState::Modified,
            ChangeState::Added,   ChangeState::Renamed,   ChangeState::Deleted,
            ChangeState::Conflict,
        };
        bool noAccent = true;
        for (const ChangeState st : kAllStates) {
            const ImVec4 c = ScmBadgeColor(st);
            if (sameColor(c, themeColor::Accent) || sameColor(c, themeColor::AccentSoft)) {
                noAccent = false;
            }
        }
        check(noAccent, "badge colors: no state borrows an Accent token (theme rule 3)");
        if (tmpCtx != nullptr) {
            ImGui::DestroyContext(tmpCtx);
            ImGui::SetCurrentContext(prevCtx); // 借りる前 (= nullptr) へ戻す
        }
    }

    // ---- (e1) repo_check の判定 ----
    {
        const std::wstring root = fs::temp_directory_path().wstring() + L"mye_scm_root";
        nlohmann::json notRepo;
        notRepo["isRepo"] = false;
        notRepo["toplevel"] = "";
        check(EvaluateRepoCheck(notRepo, root) == Unavailable::NotRepo,
              "repo_check: isRepo=false is NotRepo (the editor never runs git init)");

        nlohmann::json same;
        same["isRepo"] = true;
        // git は '/' 区切り・大小はディスクのまま返す。両方を吸収できること
        std::string slashed = WideToUtf8(root);
        for (char& ch : slashed) {
            if (ch == '\\') {
                ch = '/';
            }
        }
        same["toplevel"] = slashed;
        check(EvaluateRepoCheck(same, root) == Unavailable::None,
              "repo_check: the same directory spelled with '/' still matches");

        nlohmann::json child;
        child["isRepo"] = true;
        child["toplevel"] = WideToUtf8(fs::path(root).parent_path().wstring());
        check(EvaluateRepoCheck(child, root) == Unavailable::ToplevelMismatch,
              "repo_check: a repository above the project root is ToplevelMismatch");
    }

    // ---- (i) canonicalRoot の保存往復 ----
    {
        std::error_code ec;
        const fs::path dir = fs::temp_directory_path() / L"mye_scm_manifest";
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);

        ProjectManifest written;
        written.name = "roundtrip";
        written.engineVersion = kEngineVersion;
        written.bootScene = "scenes/main.scene.json";
        written.canonicalRoot = WideToUtf8(dir.wstring());
        check(SaveProjectManifest(dir.wstring(), written), "manifest: saved with canonicalRoot");

        ProjectManifest read;
        check(LoadProjectManifest(dir.wstring(), read) && read.canonicalRoot == written.canonicalRoot
                  && read.name == "roundtrip" && read.bootScene == "scenes/main.scene.json",
              "manifest: canonicalRoot survives the round trip without losing the other keys");

        // 空なら**キーを書かない** (「未記録」と「空に設定した」を区別するため)
        ProjectManifest blank = written;
        blank.canonicalRoot.clear();
        check(SaveProjectManifest(dir.wstring(), blank), "manifest: saved without canonicalRoot");
        std::string text;
        {
            std::ifstream f(dir / L"project.mye.json", std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
        check(text.find("canonicalRoot") == std::string::npos,
              "manifest: an empty canonicalRoot is omitted, not written as \"\"");
        ProjectManifest reread;
        check(LoadProjectManifest(dir.wstring(), reread) && reread.canonicalRoot.empty(),
              "manifest: a manifest without canonicalRoot loads as empty (old projects)");
        fs::remove_all(dir, ec);
    }

    // ---- (j) 競合のモデル (M66g) ----
    // ★ここも**手で組んだ応答**で検査する。実際に競合を作るには 2 つのクローンと
    //   分岐した履歴が要り、セルフテスト (D3D も git も無い前提) では組めない。
    //   git 側の形は cargo test (porcelain / service) が押さえている
    {
        nlohmann::json result;
        result["mergeInProgress"] = true;
        result["rebaseInProgress"] = false;
        result["conflicts"] = nlohmann::json::array();
        auto conflict = [](const char* path, const char* kind, bool ours, bool theirs) {
            nlohmann::json c;
            c["path"] = path;
            c["kind"] = kind;
            c["ours"] = ours;
            c["theirs"] = theirs;
            return c;
        };
        result["conflicts"].push_back(
            conflict("assets/scenes/main.scene.json", "both_modified", true, true));
        result["conflicts"].push_back(
            conflict("assets/textures/wall.png.meta", "deleted_by_them", true, false));
        result["merged"] = nlohmann::json::array();
        {
            nlohmann::json m;
            m["path"] = "assets/textures/floor.png";
            m["status"] = "M";
            result["merged"].push_back(m);
            nlohmann::json d;
            d["path"] = "assets/old.png";
            d["status"] = "D";
            result["merged"].push_back(d);
        }
        const ConflictList list = BuildConflictList(result);
        check(list.valid && list.mergeInProgress && !list.rebaseInProgress,
              "conflicts: the merge flag comes through");
        check(list.files.size() == 2 && list.files[1].path == "assets/textures/wall.png.meta"
                  && list.files[1].kind == "deleted_by_them" && list.files[1].ours
                  && !list.files[1].theirs,
              "conflicts: kind and the available sides survive the trip");
        check(list.merged.size() == 2 && list.merged[0].kind == BatchChange::Kind::Modified
                  && list.merged[1].kind == BatchChange::Kind::Deleted,
              "conflicts: merged files arrive as a stage-classification input (D stays D)");

        // ★欠けている ours / theirs を「採れる」と読まない。読むと、相手が消した
        //   ファイルに theirs を出して git が pathspec で落ちる
        nlohmann::json bare;
        bare["conflicts"] = nlohmann::json::array();
        bare["conflicts"].push_back(nlohmann::json{ { "path", "a.txt" } });
        const ConflictList defaults = BuildConflictList(bare);
        check(defaults.files.size() == 1 && !defaults.files[0].ours
                  && !defaults.files[0].theirs && defaults.files[0].kind == "unmerged",
              "conflicts: a response without ours/theirs is read as 'cannot take either'");

        // 対の束ね: 本体 + `.meta` が両方競合 -> 1 行 2 パス
        std::vector<ConflictFile> files;
        files.push_back({ "assets/textures/wall.png.meta", "both_modified", true, true });
        files.push_back({ "assets/textures/wall.png", "both_modified", true, false });
        files.push_back({ "assets/terrain/l.terrain.edit", "both_modified", true, true });
        std::vector<ConflictRow> rows = BuildConflictRows(files);
        check(rows.size() == 2, "conflicts: the sidecar is folded into its asset");
        check(rows[0].path == "assets/terrain/l.terrain.json"
                  && rows[0].paths.size() == 1
                  && rows[0].paths[0] == "assets/terrain/l.terrain.edit"
                  && !rows[0].primaryConflicted,
              "conflicts: a .terrain.edit alone is listed under its .terrain.json");
        check(rows[1].path == "assets/textures/wall.png" && rows[1].paths.size() == 2
                  && rows[1].paths[0] == "assets/textures/wall.png"
                  && rows[1].paths[1] == "assets/textures/wall.png.meta"
                  && rows[1].primaryConflicted,
              "conflicts: the pair is resolved in one call, in ascending order");
        // ★束の 1 つでも採れない側があれば行としても採れない (片方だけ解決される)
        check(rows[1].ours && !rows[1].theirs,
              "conflicts: a side the whole pair cannot take is not offered as 'take'");

        // 保存のガード (spec §7)。サイドカーだけの競合でも本体の保存を止める
        check(ConflictMatchesPath(files, "assets/textures/wall.png")
                  && ConflictMatchesPath(files, "assets/textures/wall.png.meta")
                  && ConflictMatchesPath(files, "assets/terrain/l.terrain.json"),
              "conflicts: saving is blocked for the asset even if only its sidecar conflicts");
        check(!ConflictMatchesPath(files, "assets/scenes/main.scene.json")
                  && !ConflictMatchesPath(files, ""),
              "conflicts: an unrelated document is still saveable");

        // 競合中の操作のゲート: MergeInProgress **だけ**を外す
        GateInputs merging;
        merging.mergeInProgress = true;
        const std::vector<GateBlocker> all = ComputeBlockers(merging);
        check(all.size() == 1 && all[0] == GateBlocker::MergeInProgress,
              "conflicts: a merge in progress closes the normal write gate");
        check(BlockersForConflictOps(all).empty(),
              "conflicts: abort / resolve / finish are allowed while the merge is open");
        GateInputs both;
        both.mergeInProgress = true;
        both.sceneDirty = true;
        both.playing = true;
        const std::vector<GateBlocker> kept = BlockersForConflictOps(ComputeBlockers(both));
        check(kept.size() == 2 && kept[0] == GateBlocker::SceneDirty
                  && kept[1] == GateBlocker::Playing,
              "conflicts: every other reason still blocks the conflict actions");

        check(CollabOpKindOf(collabop::kConflicts) == CollabOpKind::Read
                  && CollabOpKindOf(collabop::kResolve) == CollabOpKind::Write
                  && CollabOpKindOf(collabop::kMergeAbort) == CollabOpKind::Write
                  && CollabOpKindOf(collabop::kContinue) == CollabOpKind::Write,
              "op kinds: conflicts is a read, the three fixes are writes");
    }

    // ---- op の待ち方 (タイムアウトと OpInFlight) ----
    {
        check(CollabOpKindOf("hello") == CollabOpKind::Handshake
                  && CollabOpKindOf("status") == CollabOpKind::Read
                  && CollabOpKindOf("hint_changed") == CollabOpKind::Read
                  && CollabOpKindOf("commit") == CollabOpKind::Write
                  && CollabOpKindOf("no_such_op") == CollabOpKind::Write,
              "op kinds: reads time out, writes (and unknown ops) never do");

        CollabClient client;
        const uint64_t readId = client.AddPendingForTest([](const nlohmann::json&) {}, "status");
        check(!client.OpInFlight(),
              "OpInFlight: a background status must not disable the write buttons");
        const uint64_t writeId = client.AddPendingForTest([](const nlohmann::json&) {}, "commit");
        check(client.OpInFlight(), "OpInFlight: a pending write op blocks the gate");
        client.DispatchLine("{\"id\":" + std::to_string(writeId) + ",\"ok\":true,\"result\":{}}");
        check(!client.OpInFlight(), "OpInFlight: the gate opens once the write op answers");
        client.DispatchLine("{\"id\":" + std::to_string(readId) + ",\"ok\":true,\"result\":{}}");
        check(client.PendingCount() == 0, "OpInFlight: nothing is left waiting");
    }

    // ---- (e) ゲートの阻害要因を全列挙 (spec §4.1 の 13 種) ----
    {
        // 1 つずつ立てて「その 1 件だけ」が返ること。表の 13 行を機械的に舐める
        struct Row {
            const char* name;
            bool GateInputs::*field;
            GateBlocker expected;
        };
        static const Row kRows[] = {
            { "SceneDirty", &GateInputs::sceneDirty, GateBlocker::SceneDirty },
            { "ActorEdit", &GateInputs::actorEdit, GateBlocker::ActorEdit },
            { "AnimationDirty", &GateInputs::animationDirty, GateBlocker::AnimationDirty },
            { "ControllerDirty", &GateInputs::controllerDirty, GateBlocker::ControllerDirty },
            { "MixerDirty", &GateInputs::mixerDirty, GateBlocker::MixerDirty },
            { "ProjectSettingsDirty", &GateInputs::projectSettingsDirty,
              GateBlocker::ProjectSettingsDirty },
            { "Playing", &GateInputs::playing, GateBlocker::Playing },
            { "NetActive", &GateInputs::netActive, GateBlocker::NetActive },
            { "BuildRunning", &GateInputs::buildRunning, GateBlocker::BuildRunning },
            { "ScriptBuildRunning", &GateInputs::scriptBuildRunning,
              GateBlocker::ScriptBuildRunning },
            { "OpInFlight", &GateInputs::opInFlight, GateBlocker::OpInFlight },
            { "MergeInProgress", &GateInputs::mergeInProgress, GateBlocker::MergeInProgress },
            { "ServiceUnavailable", &GateInputs::serviceUnavailable,
              GateBlocker::ServiceUnavailable },
        };
        static_assert(std::size(kRows) == static_cast<size_t>(GateBlocker::Count),
                      "GateBlocker を足したらこの表にも足すこと (足さないと新しい条件が "
                      "永久に未検査で通る)");
        check(ComputeBlockers(GateInputs{}).empty(),
              "gate: nothing set means the write op may run");
        bool allSingles = true;
        for (const Row& r : kRows) {
            GateInputs in;
            in.*(r.field) = true;
            const std::vector<GateBlocker> got = ComputeBlockers(in);
            if (got.size() != 1 || got[0] != r.expected) {
                MYE_LOG_ERROR("  gate: %s did not produce exactly its own blocker", r.name);
                allSingles = false;
            }
            // 文言が引けること (Tr の表に載せ忘れると空文字列のツールチップになる)
            if (Tr(GateBlockerText(r.expected))[0] == '\0') {
                MYE_LOG_ERROR("  gate: %s has no localized reason", r.name);
                allSingles = false;
            }
        }
        check(allSingles, "gate: each of the 13 conditions maps to exactly one reason with text");

        // 複合: **全件が列挙順で**返る (最初の 1 件で止めない)
        GateInputs many;
        many.sceneDirty = true;
        many.playing = true;
        many.serviceUnavailable = true;
        const std::vector<GateBlocker> got = ComputeBlockers(many);
        check(got.size() == 3 && got[0] == GateBlocker::SceneDirty
                  && got[1] == GateBlocker::Playing
                  && got[2] == GateBlocker::ServiceUnavailable,
              "gate: three reasons come back together, in declaration order");
    }

    // ---- (f) EndBatch の適用順と Deleted の除外 ----
    {
        auto change = [](const wchar_t* p, BatchChange::Kind k) {
            BatchChange c;
            c.path = p;
            c.kind = k;
            return c;
        };
        // わざと「参照される側が後ろ」の並びで渡す
        const std::vector<BatchChange> mixed = {
            change(L"c:/p/assets/a.scene.json", BatchChange::Kind::Modified),
            change(L"c:/p/assets/hero.actor.json", BatchChange::Kind::Modified),
            change(L"c:/p/assets/hero.glb", BatchChange::Kind::Modified),
            change(L"c:/p/assets/wall.mat.json", BatchChange::Kind::Modified),
            change(L"c:/p/assets/b_tex.png", BatchChange::Kind::Modified),
            change(L"c:/p/assets/a_tex.png", BatchChange::Kind::Added),
            change(L"c:/p/assets/lit.hlsl", BatchChange::Kind::Modified),
            change(L"c:/p/assets/gone.png", BatchChange::Kind::Deleted),
        };
        const std::vector<std::wstring> ordered = OrderBatch(mixed);
        check(ordered.size() == 7, "OrderBatch: the deleted file is dropped from the output");
        bool noDeleted = true;
        for (const std::wstring& p : ordered) {
            if (p.find(L"gone.png") != std::wstring::npos) {
                noDeleted = false;
            }
        }
        check(noDeleted, "OrderBatch: a Deleted change never reaches HandleChange");
        // 種別順: hlsl -> png -> mat -> glb -> actor -> scene
        auto indexOf = [&ordered](const wchar_t* needle) {
            for (size_t i = 0; i < ordered.size(); ++i) {
                if (ordered[i].find(needle) != std::wstring::npos) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        };
        check(indexOf(L"lit.hlsl") < indexOf(L"a_tex.png")
                  && indexOf(L"a_tex.png") < indexOf(L"wall.mat.json")
                  && indexOf(L"wall.mat.json") < indexOf(L"hero.glb")
                  && indexOf(L"hero.glb") < indexOf(L"hero.actor.json")
                  && indexOf(L"hero.actor.json") < indexOf(L"a.scene.json"),
              "OrderBatch: texture -> mat -> model -> actor -> scene (the referenced side first)");
        check(indexOf(L"a_tex.png") < indexOf(L"b_tex.png"),
              "OrderBatch: files of the same kind come back in ascending key order");
        // Renamed は新しい名前が Modified と同じ扱いで残る
        BatchChange ren = change(L"c:/p/assets/new.png", BatchChange::Kind::Renamed);
        ren.oldPath = L"c:/p/assets/old.png";
        const std::vector<std::wstring> renamed = OrderBatch({ ren });
        check(renamed.size() == 1 && renamed[0].find(L"new.png") != std::wstring::npos,
              "OrderBatch: a rename applies the new name and forgets the old one");
        check(OrderBatch({}).empty(), "OrderBatch: an empty batch stays empty");
    }

    // ---- (f2) 段階分類の表 (spec §4.1) ----
    {
        auto stageOf = [](std::vector<StageChange> changes, const char* activeScene) {
            StageInputs in;
            in.changes = std::move(changes);
            in.activeScene = activeScene;
            return Classify(in);
        };
        auto one = [](const char* path, BatchChange::Kind k = BatchChange::Kind::Modified) {
            StageChange c;
            c.path = path;
            c.kind = k;
            return c;
        };

        check(stageOf({ one("assets/textures/wall.png") }, "assets/scenes/main.scene.json")
                  == ApplyStage::A,
              "stage: a texture is swapped in place (A)");
        check(stageOf({ one("assets/scenes/other.scene.json") }, "assets/scenes/main.scene.json")
                  == ApplyStage::A,
              "stage: a scene that is not open is a no-op (A)");
        check(stageOf({ one("project.mye.json") }, "") == ApplyStage::A,
              "stage: the manifest is a no-op (A)");
        check(stageOf({ one("readme.md") }, "") == ApplyStage::A,
              "stage: an unknown extension is a no-op (A)");
        check(stageOf({ one("assets/scenes/main.scene.json") }, "assets/scenes/main.scene.json")
                  == ApplyStage::B,
              "stage: the scene that is open forces a reopen (B)");
        check(stageOf({ one("src/GameLogic/Scripts/Player.cpp") }, "") == ApplyStage::B,
              "stage: a C++ script lives under src/GameLogic/Scripts (B)");
        check(stageOf({ one("assets/scripts/Player.cs") }, "") == ApplyStage::B,
              "stage: a C# script needs a recompile (B)");
        check(stageOf({ one("assets/anim/hero.controller.json") }, "") == ApplyStage::B
                  && stageOf({ one("assets/terrain/l.terrain.json") }, "") == ApplyStage::B
                  && stageOf({ one("assets/terrain/l.terrain.edit") }, "") == ApplyStage::B,
              "stage: controller and terrain go through the reopen path (B)");
        check(stageOf({ one("assets/input/actions.json") }, "") == ApplyStage::B
                  && stageOf({ one("assets/project_settings.json") }, "") == ApplyStage::B,
              "stage: the shared settings files force a reopen (B)");
        check(stageOf({ one("assets/textures/wall.png", BatchChange::Kind::Deleted) }, "")
                  == ApplyStage::B,
              "stage: a deleted asset cannot be swapped in place, so it is B");
        check(stageOf({ one("assets/schemas/health.component.schema.json") }, "")
                  == ApplyStage::C,
              "stage: a schema change means TypeIds move - restart (C)");
        {
            StageChange meta = one("assets/textures/wall.png.meta");
            check(Classify(StageInputs{ { meta }, "", kCollabMaxBatchApply }) == ApplyStage::A,
                  "stage: a .meta whose guid did not move rides with its asset (A)");
            meta.metaGuidChanged = true;
            check(Classify(StageInputs{ { meta }, "", kCollabMaxBatchApply }) == ApplyStage::C,
                  "stage: a .meta whose guid moved invalidates every reference (C)");
            StageChange renamedMeta = one("assets/textures/wall.png.meta",
                                          BatchChange::Kind::Renamed);
            check(Classify(StageInputs{ { renamedMeta }, "", kCollabMaxBatchApply })
                      == ApplyStage::C,
                  "stage: a renamed .meta is C as well");
        }
        // 混在は最も重いもの
        check(stageOf({ one("assets/textures/wall.png"),
                        one("assets/schemas/x.component.schema.json"),
                        one("assets/scripts/Player.cs") },
                      "")
                  == ApplyStage::C,
              "stage: a mixed set takes the heaviest stage");
        // actor と scene の同居 (どちらも単体では A)
        check(stageOf({ one("assets/actors/hero.actor.json"),
                        one("assets/scenes/other.scene.json") },
                      "assets/scenes/main.scene.json")
                  == ApplyStage::B,
              "stage: an actor and a scene moving together must be reopened (B)");
        check(stageOf({ one("assets/actors/hero.actor.json") }, "assets/scenes/main.scene.json")
                  == ApplyStage::A,
              "stage: the actor alone is still applied in place (A)");
        // 件数上限
        {
            StageInputs many;
            many.activeScene = "";
            for (int i = 0; i <= kCollabMaxBatchApply; ++i) {
                many.changes.push_back(one(("assets/t/" + std::to_string(i) + ".png").c_str()));
            }
            check(static_cast<int>(many.changes.size()) == kCollabMaxBatchApply + 1
                      && Classify(many) == ApplyStage::C,
                  "stage: more than kCollabMaxBatchApply changes is a restart (C)");
            many.changes.pop_back();
            check(Classify(many) == ApplyStage::A, "stage: exactly the limit is still A");
        }
    }

    // ---- (j) M66e: branches の解析と、checkout の names → 段階分類 ----
    {
        // `branches` 応答 (Rust の ops.rs::branches が返す形そのもの)
        const nlohmann::json branchesResult = nlohmann::json::parse(R"({
            "current": "main",
            "locals": [
                { "name": "main", "oid": "aaaa", "upstream": "origin/main" },
                { "name": "feature", "oid": "bbbb", "upstream": "" },
                { "name": "", "oid": "cccc", "upstream": "" }
            ],
            "remotes": [ { "name": "origin/main", "oid": "aaaa", "upstream": "" } ]
        })");
        const BranchList list = BuildBranchList(branchesResult);
        check(list.valid && list.current == "main" && list.locals.size() == 2
                  && list.remotes.size() == 1,
              "branches: locals and remotes are kept apart and the nameless row is dropped");
        check(list.locals[0].upstream == "origin/main" && list.locals[1].upstream.empty(),
              "branches: the tracking branch comes through per row");
        check(!BuildBranchList(nlohmann::json::object()).current.size()
                  && BuildBranchList(nlohmann::json::object()).valid,
              "branches: an empty answer is still 'valid' (detached HEAD has no current)");

        // checkout / diff_names の `names` → 段階分類の入力
        const nlohmann::json names = nlohmann::json::parse(R"([
            { "path": "assets/textures/wall.png", "status": "M" },
            { "path": "assets/textures/new.png", "status": "A" },
            { "path": "assets/textures/gone.png", "status": "D" },
            { "path": "assets/textures/moved.png", "status": "R", "oldPath": "assets/old.png" },
            { "path": "", "status": "M" }
        ])");
        const std::vector<StageChange> changes = ChangesFromNames(names);
        check(changes.size() == 4, "names: the row without a path is dropped");
        check(changes[0].kind == BatchChange::Kind::Modified
                  && changes[1].kind == BatchChange::Kind::Added
                  && changes[2].kind == BatchChange::Kind::Deleted
                  && changes[3].kind == BatchChange::Kind::Renamed
                  && changes[3].oldPath == "assets/old.png",
              "names: M / A / D / R map onto the ReloadHub kinds");
        // ★D が 1 件混ざるだけで段階は B (消えた資産は差し替えられない)
        check(Classify(StageInputs{ changes, "", kCollabMaxBatchApply }) == ApplyStage::B,
              "names: a deleted asset in the checkout set forces a reopen (B)");
        std::vector<StageChange> onlyEdits = { changes[0], changes[1] };
        check(Classify(StageInputs{ onlyEdits, "", kCollabMaxBatchApply }) == ApplyStage::A,
              "names: textures alone stay in place (A)");
        check(ChangesFromNames(nlohmann::json::object()).empty()
                  && ChangesFromNames(nlohmann::json::array()).empty(),
              "names: a missing or empty array is not a change set");
    }

    // ---- (h) EditorSettings の新キー (spec §4.2、M66f) ----
    {
        std::error_code ec;
        const fs::path dir = fs::temp_directory_path() / L"mye_scm_settings";
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);

        // 1) 既定値 (ファイルが無い) — 「背景で取得する / 5 分ごと」
        {
            EditorSettings fresh;
            fresh.Load(dir.wstring());
            check(fresh.scmAutoFetch && fresh.scmFetchIntervalMin == 5,
                  "settings: background fetch defaults to on, every 5 minutes");
            // M66h: 粒子の個人設定。既定は**旧 project_settings.json と同じ値**
            // (移行しない方針なので、既定がずれると見た目が黙って変わる)
            check(!fresh.particleCompareMode && fresh.particleCompareOffsetX == 4.0f
                      && fresh.particleCpuSimd,
                  "settings: the particle personal keys default to compare off / +4.0 / simd on");
        }
        // 2) 書いて読み直す (往復)
        {
            EditorSettings written;
            written.Load(dir.wstring());
            written.scmAutoFetch = false;
            written.scmFetchIntervalMin = 17;
            written.camMoveSpeed = 12.5f; // 既存キーが巻き添えで消えないことも見る
            written.particleCompareMode = true;
            written.particleCompareOffsetX = 9.5f;
            written.particleCpuSimd = false;
            written.Save();

            EditorSettings read;
            read.Load(dir.wstring());
            check(!read.scmAutoFetch && read.scmFetchIntervalMin == 17
                      && read.camMoveSpeed == 12.5f,
                  "settings: scmAutoFetch / scmFetchIntervalMin survive the round trip");
            check(read.particleCompareMode && read.particleCompareOffsetX == 9.5f
                      && !read.particleCpuSimd,
                  "settings: the particle personal keys survive the round trip");
        }
        // 3) 旧 JSON (キーが無い) は既定値で読める。
        //    ★ここが「前方/後方互換」の実体 — 既存プロジェクトの
        //      editor_settings.json には当然このキーが無い
        {
            {
                std::ofstream f(dir / L"editor_settings.json", std::ios::binary);
                f << "{\"camMoveSpeed\": 3.0, \"gridVisible\": false}";
            }
            EditorSettings legacy;
            legacy.Load(dir.wstring());
            check(legacy.scmAutoFetch && legacy.scmFetchIntervalMin == 5
                      && legacy.camMoveSpeed == 3.0f && !legacy.gridVisible,
                  "settings: an old file without the scm keys loads with the defaults");
            // 保存しても他人のキーを消さない (マージ保存)
            legacy.Save();
            std::string text;
            {
                std::ifstream f(dir / L"editor_settings.json", std::ios::binary);
                text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            }
            check(text.find("scmAutoFetch") != std::string::npos
                      && text.find("scmFetchIntervalMin") != std::string::npos,
                  "settings: saving an old file adds the scm keys");
        }
        fs::remove_all(dir, ec);
    }

    // ---- (g) 推奨 .gitignore (spec §2 の S10、M66h) ----
    // ★検査の中心は「足りない行だけを返す」と「既存行を 1 行も壊さない」の 2 点。
    //   ここが壊れると、押した人の .gitignore が黙って書き換わる = 取り返しがつかない
    {
        std::error_code ec;
        // 1) ファイルが無い (空文字列) → 全 7 行
        const std::vector<std::string> all = MissingGitignoreLines("");
        check(all.size() == 7 && all.front() == "/.mye/" && all.back() == "*.log",
              "gitignore: an empty file is missing all seven recommended lines");

        // 2) 旧テンプレ (3 行) → 不足は 4 行、並びは推奨順のまま
        const std::vector<std::string> four =
            MissingGitignoreLines("/.mye/\n/cache/\n/dist/\n");
        check(four.size() == 4 && four[0] == "/crash/" && four[1] == "/save/"
                  && four[2] == "/assets/scripts/Generated/" && four[3] == "*.log",
              "gitignore: the three old template lines leave exactly the four new ones");

        // 3) 全部ある → 空 (ボタンが無効になる条件)
        check(MissingGitignoreLines(RecommendedGitignoreText()).empty(),
              "gitignore: the template itself needs nothing appended");

        // 4) 末尾改行なし / CRLF / 前後の空白 / コメント混じりでも同じ結論。
        //   ★このリポジトリは core.autocrlf=true 前提 = 手元の .gitignore は CRLF になりうる。
        //     行末の CR を落とさないと「全部足りない」と誤判定して 7 行を二重に追記する
        check(MissingGitignoreLines("# generated\r\n  /.mye/  \r\n/cache/\r\n/dist/\r\n"
                                    "/crash/\r\n/save/\r\n/assets/scripts/Generated/\r\n*.log")
                  .empty(),
              "gitignore: CRLF, padding, comments and a missing final newline are all tolerated");

        // 5) 似ているだけの行は「ある」と見なさない (完全一致のみ)
        const std::vector<std::string> similar = MissingGitignoreLines("cache/\ndist\n!*.log\n");
        check(similar.size() == 7,
              "gitignore: near-misses (no leading slash, a negation) do not count as present");

        // 6) 追記の本文を組む純関数 — 既存部分は**バイト単位で不変**。
        //    ★ここがボタンの実体 (EditorApp は読む→これを呼ぶ→書くだけ)。
        //      人の .gitignore に追記する以上、「触っていない行を 1 バイトも動かさない」は
        //      コメントではなく検査で担保する
        {
            const std::string kept = "# my rules\r\n/.mye/\r\nsecret/\r\n/cache/\r\n/dist/\r\n";
            const std::string grown = GitignoreWithRecommended(kept);
            check(grown.compare(0, kept.size(), kept) == 0,
                  "gitignore: appending never rewrites a single byte of what was there");
            check(grown == kept + "/crash/\n/save/\n/assets/scripts/Generated/\n*.log\n",
                  "gitignore: the four missing lines are appended in the recommended order");
            check(MissingGitignoreLines(grown).empty(),
                  "gitignore: applying once is enough (the button goes quiet)");
            check(GitignoreWithRecommended(grown) == grown,
                  "gitignore: applying twice changes nothing (no duplicated lines)");
            // 末尾に改行が無い形 → 改行を 1 つだけ補ってから足す
            const std::string noEol = "/.mye/\n/cache/\n/dist/";
            check(GitignoreWithRecommended(noEol)
                      == noEol + "\n/crash/\n/save/\n/assets/scripts/Generated/\n*.log\n",
                  "gitignore: a file without a final newline gets exactly one added");
            // 空のファイル (= 新規作成) はテンプレと同じ本文になる
            check(GitignoreWithRecommended("") == RecommendedGitignoreText(),
                  "gitignore: applying to a missing file produces the template itself");
        }

        // 7) テンプレ生成 = CreateProject が書く .gitignore が 7 行
        //    (エンジン assets は空の偽ディレクトリでよい — Empty テンプレの
        //     コピーは error_code 受けなので、中身が無くても作成は成功する)
        const fs::path tmp = fs::temp_directory_path() / L"mye_gitignore_tmpl";
        fs::remove_all(tmp, ec);
        fs::create_directories(tmp / L"engine_assets", ec);
        const fs::path proj = tmp / L"proj";
        std::string err;
        const bool made = CreateProject(proj.wstring(), "tmpl", ProjectTemplate::Empty,
                                        (tmp / L"engine_assets").wstring(), &err);
        std::string written;
        {
            std::ifstream f(proj / L".gitignore", std::ios::binary);
            written.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
        check(made && written == RecommendedGitignoreText()
                  && std::count(written.begin(), written.end(), '\n') == 7,
              "gitignore: a new project is created with all seven lines");
        check(MissingGitignoreLines(written).empty(),
              "gitignore: the file a new project gets never asks to be updated");
        fs::remove_all(tmp, ec);
    }

    // ---- (k) M66f: remote_state の解析と remote_changed の配線 ----
    {
        // 1) 応答 → モデル (純関数)
        nlohmann::json remote;
        remote["upstream"] = "origin/main";
        remote["hasRemote"] = true;
        remote["ahead"] = 2;
        remote["behind"] = 3;
        remote["commits"] = nlohmann::json::array();
        for (int i = 0; i < 2; ++i) {
            nlohmann::json c;
            c["sha"] = std::string(40, static_cast<char>('a' + i));
            c["author"] = i == 0 ? "hina" : "rui";
            c["date"] = "2026-09-03T12:00:00+09:00";
            c["subject"] = i == 0 ? "add the boss room" : "retune the fog";
            remote["commits"].push_back(c);
        }
        const RemoteState parsed = BuildRemoteState(remote);
        check(parsed.valid && parsed.upstream == "origin/main" && parsed.hasRemote
                  && parsed.ahead == 2 && parsed.behind == 3 && parsed.commits.size() == 2
                  && parsed.commits[0].author == "hina"
                  && parsed.commits[1].subject == "retune the fog",
              "remote_state: upstream / ahead / behind / commits come through");
        // 空 (リモートなし) でも valid にする — 「まだ聞いていない」と区別するため
        const RemoteState empty = BuildRemoteState(nlohmann::json::object());
        check(empty.valid && empty.upstream.empty() && !empty.hasRemote && empty.behind == 0
                  && empty.commits.empty(),
              "remote_state: a repository without a remote parses as valid-but-empty");
        check(!BuildRemoteState(nlohmann::json()).valid,
              "remote_state: a null result is not valid (nothing to show yet)");

        // 2) remote_changed 通知 → セッション。**通知はここでしか入ってこない**
        //    (背景 fetch は worker のタイマー発で、応答 id を持たない)。
        // ★Start に「DLL の無い exeDir」を渡す = ロードは失敗するが通知の配線は張られる
        //   (Start が SetEventHandler を Load より先に呼んでいるのはこのため)。
        //   projectRoot が空だと NoProject で即 return するので、実在するどこかを渡す
        SourceControlSession scm;
        std::error_code sec;
        const fs::path noDll = fs::temp_directory_path() / L"mye_scm_no_dll";
        fs::create_directories(noDll, sec);
        scm.Start(noDll.wstring(), noDll.wstring(), false, 5);
        check(scm.State() == Unavailable::NoService,
              "remote_changed: a session without the dll is NoService (but still wired)");
        nlohmann::json ev;
        ev["event"] = "remote_changed";
        ev["remote"] = remote;
        scm.Client().DispatchLine(ev.dump());
        check(scm.Remote().valid && scm.Remote().behind == 3
                  && scm.Remote().commits.size() == 2,
              "remote_changed: the background fetch result lands in the session");
        check(scm.TakeRemoteChanged() && !scm.TakeRemoteChanged(),
              "remote_changed: the toast fires once, not every frame");

        // 3) 失敗の通知は**エラー欄を赤くしない** (オフラインで作業している間ずっと
        //    「壊れている」ように見えるのを防ぐ)。取り出せるのは 1 回だけ
        nlohmann::json fail;
        fail["event"] = "remote_changed";
        fail["error"] = { { "code", "network" }, { "detail", "could not resolve host" } };
        scm.Client().DispatchLine(fail.dump());
        std::string code;
        std::string detail;
        check(scm.TakeFetchError(code, detail) && code == "network",
              "remote_changed: a background failure is reported once");
        check(!scm.TakeFetchError(code, detail),
              "remote_changed: the same failure is not reported twice");
        check(scm.ErrorCode().empty(),
              "remote_changed: a background failure does not turn the window red");

        // 4) op の待ち方: fetch / pull / push は書き込み系 (打ち切らない)、
        //    remote_state は読み取り系。**分類を間違えるとネットワーク待ちが
        //    30 s で打ち切られ、諦めた後に refs だけ書き換わる**
        check(CollabOpKindOf(collabop::kFetch) == CollabOpKind::Write
                  && CollabOpKindOf(collabop::kPull) == CollabOpKind::Write
                  && CollabOpKindOf(collabop::kPush) == CollabOpKind::Write
                  && CollabOpKindOf(collabop::kRemoteState) == CollabOpKind::Read,
              "op kinds: network writes never time out, remote_state does");
    }

    // ---- (j) M66k: review-1 の実害 3 件 (spec の受け入れ条件 18 / 19 / 20) ----
    {
        // (j1) 受け入れ条件 18 前半: 「保存してコミット」の 1 手目が失敗したら
        //      stage も commit もしない。**ここが開いていると、既に stage 済みの
        //      別ファイルだけが「保存してコミット」の名前で共有履歴に残る**
        int staged = 0;
        int committed = 0;
        std::wstring stagedPath;
        auto stage = [&](const std::wstring& p) {
            ++staged;
            stagedPath = p;
        };
        auto commit = [&] { ++committed; };
        check(!SaveThenCommit([] { return std::wstring(); }, stage, commit) && staged == 0
                  && committed == 0,
              "save+commit: a failed save stages nothing and commits nothing");
        check(SaveThenCommit([] { return std::wstring(L"C:\\proj\\assets\\a.scene.json"); },
                             stage, commit)
                  && staged == 1 && stagedPath == L"C:\\proj\\assets\\a.scene.json"
                  && committed == 1,
              "save+commit: a successful save stages that document, then commits");
        check(!SaveThenCommit({}, stage, commit) && staged == 1 && committed == 1,
              "save+commit: with no save hook wired nothing is staged or committed");

        // (j2) 受け入れ条件 18 後半: コミット本文は**成功応答を受けてから**消す
        check(!ShouldClearCommitMessage(false, "fix the fog", "fix the fog"),
              "commit message: a failed commit keeps what the user wrote");
        check(ShouldClearCommitMessage(true, "fix the fog", "fix the fog"),
              "commit message: a successful commit clears the box");
        check(!ShouldClearCommitMessage(true, "fix the fog", "fix the fog and the sky"),
              "commit message: text typed while the commit was in flight is not thrown away");

        // 送れなかった commit も**必ず 1 回応答する** (呼ばないと窓が「投げた本文」を
        // 抱えたまま、消してよいのか分からない状態で固まる)
        SourceControlSession scm;
        std::error_code sec;
        const fs::path noDll = fs::temp_directory_path() / L"mye_scm_no_dll";
        fs::create_directories(noDll, sec);
        scm.Start(noDll.wstring(), noDll.wstring(), false, 5);
        int answers = 0;
        bool sawOk = true;
        std::string code;
        auto record = [&](bool ok, const std::string& c, const std::string&) {
            ++answers;
            sawOk = ok;
            code = c;
        };
        scm.Commit("anything", record);
        check(answers == 1 && !sawOk && code == collaberr::kServiceDead,
              "commit: a commit that could not be sent answers with a failure");
        scm.Commit("", record);
        check(answers == 2 && code == collaberr::kBadRequest,
              "commit: an empty message is refused with an answer, not with silence");

        // (j3) 受け入れ条件 19: actions.json が無いプロジェクトで偽の未保存を立てない
        std::error_code ec;
        const fs::path assets = fs::temp_directory_path() / L"mye_scm_actions";
        fs::remove_all(assets, ec);
        fs::create_directories(assets, ec);
        InputActions ia;
        check(!InputActionsDifferFromDisk(assets.wstring(), ia),
              "project settings: a project without actions.json is not dirty");
        InputActionDef def;
        def.name = "Jump";
        def.nameHash = HashStr(def.name);
        ia.Actions().push_back(def);
        check(InputActionsDifferFromDisk(assets.wstring(), ia),
              "project settings: an action added but not saved is dirty (file still absent)");
        check(ia.Save(assets.wstring()), "project settings: actions.json was written");
        check(!InputActionsDifferFromDisk(assets.wstring(), ia),
              "project settings: saving clears the dirty state");
        // 空の root は「まだ窓を開いていない」= 判定しない
        check(!InputActionsDifferFromDisk(std::wstring(), ia),
              "project settings: without an assets root there is nothing to compare");
        fs::remove_all(assets, ec);

        // (j4) 受け入れ条件 20: 実行中モーダルの回復案内は 15 s を超えたときだけ
        check(!ShouldShowStuckHint(0.0) && !ShouldShowStuckHint(0.2)
                  && !ShouldShowStuckHint(14.9) && !ShouldShowStuckHint(kStuckHintSec),
              "stuck hint: a normal write op (and the threshold itself) shows nothing");
        check(ShouldShowStuckHint(15.1) && ShouldShowStuckHint(600.0),
              "stuck hint: a git that never comes back gets the recovery note");
        check(Tr(StrId::Scm_OpStuckHint)[0] != '\0',
              "stuck hint: the recovery note has a localized text");
    }

    // ---- 実 DLL 経由の結線 (MYE_COLLAB_PROBE=<repo> のときだけ) ----
    // ★窓のボタン -> Session -> DLL -> git の 1 本を、UI を触らずに実走させる。
    //   ここを通していないと「ゲートは正しいがボタンが何にも繋がっていない」に
    //   気付けない (どの純関数テストも配線は見ていない)
    if (const char* probe = std::getenv("MYE_COLLAB_PROBE"); probe != nullptr && probe[0] != '\0') {
        const std::wstring root = Utf8ToWide(probe);
        SourceControlSession scm;
        // ★プローブでは背景 fetch を切る (autoFetch=false)。走らせると
        //   「タイマー由来の通知」と「プローブが起こした変化」が混ざり、
        //   何を観測しているのか読めなくなる (定期 fetch の検査は cargo test 側)
        scm.Start(GetExecutableDir(), root, false, 5);
        auto pump = [&scm](const std::function<bool()>& done, int timeoutMs) {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
            while (std::chrono::steady_clock::now() < deadline) {
                scm.Poll();
                if (done()) {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            scm.Poll();
            return done();
        };
        const bool ready = pump([&scm] { return scm.Model().valid; }, 15000);
        check(ready && scm.State() == Unavailable::None,
              "probe: the session reached a usable state through the real DLL");
        if (ready) {
            GateInputs gi;
            gi.serviceUnavailable = scm.State() != Unavailable::None;
            check(ComputeBlockers(gi).empty(), "probe: a healthy session leaves the gate open");

            // 未追跡ファイルを 1 個置いて revert で消す = 書き込み系の往復
            const fs::path scratch = fs::path(root) / L"mye_probe_scratch.txt";
            {
                std::ofstream f(scratch, std::ios::binary);
                f << "probe";
            }
            // ★取り直しの合図は **M66i の保存ヒント**で出す。RequestStatus と同じ
            //   結果になるが、こちらは「保存 -> HintSaved -> Poll でまとめて送信 ->
            //   応答の status をモデルへ」の 1 本を実 DLL で通す
            //   (監視スレッドも同じ変更を拾うので、これは配線の実走であって
            //    「監視より速い」ことの証明ではない — 速さは実機で見る)
            const auto hintAt = std::chrono::steady_clock::now();
            scm.HintSaved(scratch.wstring());
            const bool listed = pump(
                [&scm] {
                    return scm.Model().FindEntry("mye_probe_scratch.txt") != nullptr;
                },
                15000);
            check(listed, "probe: the new file shows up in status");
            // 実測値をログに残す (spec の「保存した瞬間に反映」= 監視の 300 ms
            // デバウンスより速いこと。閾値では止めない — git の起動時間は
            // 機体とウイルス対策で桁が変わるので、赤くしても原因が読めない)
            MYE_LOG_INFO("[probe] hint_changed round trip: %lld ms",
                         static_cast<long long>(
                             std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - hintAt)
                                 .count()));
            // バッジ引き (M66i): **絶対パス**から toplevel 相対キーへ落ちるか。
            // ここが壊れるとバッジは 1 個も出ないが、画面上は「変更が無い」と
            // 区別が付かない (純関数テストは絶対パスを見ていない)
            check(scm.BadgeForFile(scratch.wstring()) == ChangeState::Untracked,
                  "probe: the badge lookup finds the new file by its absolute path");
            check(scm.BadgeForFile((fs::path(root) / L"no_such_file.txt").wstring())
                      == ChangeState::None,
                  "probe: an unchanged path has no badge");
            check(scm.BadgeForFile(L"C:\\mye_outside_the_repo.txt") == ChangeState::None,
                  "probe: a path outside the repository has no badge");
            bool done = false;
            bool ok = false;
            scm.Revert({ "mye_probe_scratch.txt" },
                       [&done, &ok](bool success, const std::string&, const std::string&) {
                           done = true;
                           ok = success;
                       });
            check(pump([&done] { return done; }, 30000) && ok,
                  "probe: revert answers through the real DLL");
            std::error_code ec;
            check(!fs::exists(scratch, ec),
                  "probe: reverting an untracked file deletes it from disk");

            // M66k: commit の**失敗**を実 DLL で 1 回通す (review-1 #4)。
            // index に何も無ければ git は nothing_to_commit で必ず落ちるので、
            // **リポジトリを 1 バイトも変えずに**「失敗応答 → 本文を残す」を実走できる。
            // ★staged が 1 件でもある / マージ途中なら**やらない** — 走らせると
            //   本物のコミットが増えてしまう (プローブは repo を変えない約束)
            bool anythingStaged = scm.MergeInProgress() || scm.RebaseInProgress();
            for (const PairedEntry& e : scm.Model().entries) {
                if (e.indexState != ChangeState::None || e.conflict) {
                    anythingStaged = true;
                }
            }
            if (anythingStaged) {
                MYE_LOG_INFO("[probe] skipped the failing-commit check (the index is not empty)");
            } else {
                const std::string sent = "probe: this commit must fail";
                bool cDone = false;
                bool cOk = true;
                scm.Commit(sent, [&cDone, &cOk](bool ok, const std::string&, const std::string&) {
                    cDone = true;
                    cOk = ok;
                });
                check(pump([&cDone] { return cDone; }, 30000) && !cOk,
                      "probe: a commit with an empty index answers with a failure");
                check(!ShouldClearCommitMessage(cOk, sent, sent),
                      "probe: a failed commit leaves the message in the box");
            }

            // M66e: ブランチの一覧と checkout を**実 DLL 経由**で 1 往復させる。
            // ★リポジトリを変えない形にしてある (今いるブランチへ切り替える) —
            //   プローブが repo を書き換えると、次に走ったときの前提が変わる
            scm.RequestBranches();
            const bool branched = pump([&scm] { return scm.Branches().valid; }, 15000);
            check(branched && !scm.Branches().current.empty()
                      && !scm.Branches().locals.empty(),
                  "probe: branches comes back with a current branch through the real DLL");
            if (branched && !scm.Branches().current.empty()) {
                bool coDone = false;
                SourceControlSession::TreeOpResult coRes;
                scm.Checkout(scm.Branches().current,
                             [&coDone, &coRes](const SourceControlSession::TreeOpResult& r) {
                                 coDone = true;
                                 coRes = r;
                             });
                check(pump([&coDone] { return coDone; }, 30000) && coRes.ok
                          && coRes.changes.empty(),
                      "probe: switching to the branch we are already on changes no files");
            }

            // サービスが死んだら**ゲートが閉じる**ところまで通す (spec §4.0 / M66b の
            // 積み残し)。DLL 側の panic 注入は v1 で凍結した op 一覧の外なので、
            // サービスが出すのと同じ通知行を配って C++ の経路だけを実走させる
            scm.Client().DispatchLine(
                "{\"event\":\"service_error\",\"code\":\"internal_panic\",\"detail\":\"probe\"}");
            check(scm.State() == Unavailable::ServiceDied,
                  "probe: service_error moves the session to ServiceDied");
            GateInputs dead;
            dead.serviceUnavailable = scm.State() != Unavailable::None;
            const std::vector<GateBlocker> blocked = ComputeBlockers(dead);
            check(blocked.size() == 1 && blocked[0] == GateBlocker::ServiceUnavailable,
                  "probe: a dead service closes the write gate");
        }
        scm.Shutdown();
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Source control self test PASSED ====");
        return true;
    }
    MYE_LOG_ERROR("==== Source control self test FAILED (%d) ====", failCount);
    return false;
}

} // namespace mye
