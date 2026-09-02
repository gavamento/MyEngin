#include "Editor/SourceControl/SourceControlSelfTest.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include "Editor/SourceControl/CollabClient.h"
#include "Editor/SourceControl/PairRule.h"
#include "Editor/SourceControl/SourceControlState.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/Project.h"
#include "Engine/Platform/PathUtil.h"

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

    if (failCount == 0) {
        MYE_LOG_INFO("==== Source control self test PASSED ====");
        return true;
    }
    MYE_LOG_ERROR("==== Source control self test FAILED (%d) ====", failCount);
    return false;
}

} // namespace mye
