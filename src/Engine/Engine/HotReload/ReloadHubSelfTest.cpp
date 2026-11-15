#include "Engine/Engine/HotReload/ReloadHubSelfTest.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/AI/BehaviorTreeLibrary.h"
#include "Engine/Engine/AI/BlackboardLibrary.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Engine/HotReload/ReloadHub.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Scene/SceneSerializer.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {

// ReloadHub の private を、Init (assets 監視の起動) を通さずに組むための試験用の窓口
struct ReloadHubTestAccess {
    static void Attach(ReloadHub& hub, Scene& scene, const std::wstring& scenePath)
    {
        hub.scene_ = &scene;
        hub.SetActiveScenePath(scenePath);
    }
    static void Change(ReloadHub& hub, const std::wstring& path) { hub.HandleChange(NormalizePathKey(path)); }
};

bool RunReloadHubSelfTest()
{
    MYE_LOG_INFO("==== ReloadHub (asset kind table) self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const std::string& what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what.c_str());
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what.c_str());
            ++failCount;
        }
    };

    // パスは NormalizePathKey 済みの形 (小文字 + '\\')
    struct Case {
        const wchar_t* path;
        ReloadKind kind;
        int rank;
    };
    const Case cases[] = {
        { L"c:\\p\\assets\\shaders\\lit.hlsl", ReloadKind::Shader, 0 },
        { L"c:\\p\\assets\\shaders\\common.hlsli", ReloadKind::Shader, 0 },
        { L"c:\\p\\assets\\textures\\wall.png", ReloadKind::Texture, 1 },
        { L"c:\\p\\assets\\textures\\wall.tga", ReloadKind::Texture, 1 },
        { L"c:\\p\\assets\\textures\\wall.jpg", ReloadKind::Texture, 1 },
        { L"c:\\p\\assets\\textures\\wall.jpeg", ReloadKind::Texture, 1 },
        { L"c:\\p\\assets\\textures\\wall.dds", ReloadKind::Texture, 1 },
        { L"c:\\p\\assets\\audio\\step.wav", ReloadKind::AudioClip, 2 },
        { L"c:\\p\\assets\\audio\\step.ogg", ReloadKind::AudioClip, 2 },
        { L"c:\\p\\assets\\model\\hero.glb", ReloadKind::Gltf, 4 },
        { L"c:\\p\\assets\\model\\hero.gltf", ReloadKind::Gltf, 4 },
        { L"c:\\p\\assets\\model\\hero.fbx", ReloadKind::Fbx, 4 },
        { L"c:\\p\\assets\\materials\\wall.mat.json", ReloadKind::Material, 3 },
        { L"c:\\p\\assets\\anims\\walk.anim.json", ReloadKind::Anim, 5 },
        { L"c:\\p\\assets\\audio\\door.sound.json", ReloadKind::Sound, 6 },
        { L"c:\\p\\assets\\audio\\impact\\glass.impact.json", ReloadKind::ImpactSound, 6 },
        { L"c:\\p\\assets\\audio\\default.mixer.json", ReloadKind::Mixer, 6 },
        { L"c:\\p\\assets\\physics\\metal.physmat.json", ReloadKind::PhysMat, 6 },
        { L"c:\\p\\assets\\2d\\walk.spriteanim.json", ReloadKind::SpriteAnim, 6 },
        { L"c:\\p\\assets\\2d\\wall.shape2d.json", ReloadKind::Shape2D, 6 },
        { L"c:\\p\\assets\\2d\\ground.tilepalette.json", ReloadKind::TilePalette, 6 },
        { L"c:\\p\\assets\\2d\\level.tilemap.json", ReloadKind::Tilemap, 6 },
        { L"c:\\p\\assets\\2d\\hill.spriteshape.json", ReloadKind::SpriteShape, 6 },
        { L"c:\\p\\assets\\ai\\guard.bt.json", ReloadKind::BehaviorTree, 6 },
        { L"c:\\p\\assets\\ai\\guard.bb.json", ReloadKind::Blackboard, 6 },
        { L"c:\\p\\assets\\feel\\heavyhit.feel.json", ReloadKind::Feel, 6 }, // M94d
        { L"c:\\p\\assets\\deepmodal\\deepmodal.dmnet", ReloadKind::ModalNet, 6 },
        { L"c:\\p\\assets\\actors\\hero.actor.json", ReloadKind::Compose, 7 },
        { L"c:\\p\\assets\\prefabs\\door.prefab.json", ReloadKind::Compose, 7 },
        { L"c:\\p\\assets\\scenes\\main.scene.json", ReloadKind::Scene, 8 },
        // 未知の .json も「開いているシーンなら差分適用」の対象。順位は最後
        { L"c:\\p\\assets\\notes.json", ReloadKind::Scene, 9 },
        // どの行にも当たらない (.hlsl.bak は末尾が .hlsl ではない)
        { L"c:\\p\\assets\\readme.txt", ReloadKind::None, 9 },
        { L"c:\\p\\assets\\shaders\\lit.hlsl.bak", ReloadKind::None, 9 },
        { L"c:\\p\\assets\\noext", ReloadKind::None, 9 },
    };
    for (const Case& c : cases) {
        const std::wstring path = c.path;
        const ReloadKind kind = ReloadKindOf(path);
        const int rank = ReloadRank(path);
        check(kind == c.kind && rank == c.rank,
              "kind / rank of " + WideToUtf8(path) + " (kind=" + std::to_string(static_cast<int>(kind))
                  + " rank=" + std::to_string(rank) + ")");
    }

    // ---- BT / BB: 内容が同じ再読込は登録を置き換えない (保存直後の ReloadHub で走っている木が 2 回やり直すのを防ぐ) ----
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path dir = fs::temp_directory_path(ec) / L"mye_reloadhub_selftest";
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        const std::wstring btPath = (dir / L"Same.bt.json").wstring();
        const std::wstring bbPath = (dir / L"Same.bb.json").wstring();

        BlackboardAsset board;
        BbKeyDef key;
        key.name = "Alarm";
        key.type = BbType::Bool;
        board.keys.push_back(key);
        BehaviorTreeAsset tree;
        BtNodeDef node;
        node.id = 0;
        node.kind = BtNodeKind::Wait;
        const BtNodeTypeInfo& info = BtNodeTypeOf(BtNodeKind::Wait);
        node.params.assign(static_cast<size_t>(info.paramCount), BtParamValue{});
        node.keys.assign(static_cast<size_t>(info.keyCount), std::string());
        tree.nodes.push_back(node);
        tree.rootId = 0;
        {
            std::ofstream(fs::path(bbPath), std::ios::binary) << BlackboardLibrary::ToJson(board).dump(2);
            std::ofstream(fs::path(btPath), std::ios::binary) << BehaviorTreeLibrary::ToJson(tree).dump(2);
        }

        // ReloadHub は正規化 (小文字) 済みのパスで読み直す。登録は大文字小文字の残るパスで済んでいる
        const std::wstring bbReload = NormalizePathKey(bbPath);
        const std::wstring btReload = NormalizePathKey(btPath);
        BlackboardLibrary boards;
        BehaviorTreeLibrary trees;
        bool unchanged = false;
        const uint64_t bbGuid = boards.LoadFromFile(bbPath);
        const uint64_t btGuid = trees.LoadFromFile(btPath);
        const auto bbBefore = boards.GetShared(bbGuid);
        const auto btBefore = trees.GetShared(btGuid);
        check(bbBefore && btBefore, "(前提) BB / BT を読める");
        check(boards.LoadFromFile(bbReload, &unchanged) == bbGuid && unchanged && boards.GetShared(bbGuid) == bbBefore,
              "BB: 同じ内容の再読込は置き換えない (登録の同一性が保たれる)");
        check(trees.LoadFromFile(btReload, &unchanged) == btGuid && unchanged && trees.GetShared(btGuid) == btBefore,
              "BT: 同じ内容の再読込は置き換えない (走っている木が無駄にやり直さない)");
        check(trees.LoadFromFile(btPath) == btGuid && trees.GetShared(btGuid) != btBefore,
              "(対照) outUnchanged を渡さない読み込みは従来どおり必ず置き換える");

        key.name = "Alert";
        board.keys.push_back(key);
        tree.nodes[0].params[0].i = 99;
        {
            std::ofstream(fs::path(bbPath), std::ios::binary) << BlackboardLibrary::ToJson(board).dump(2);
            std::ofstream(fs::path(btPath), std::ios::binary) << BehaviorTreeLibrary::ToJson(tree).dump(2);
        }
        const auto btReplaced = trees.GetShared(btGuid);
        check(boards.LoadFromFile(bbReload, &unchanged) == bbGuid && !unchanged && boards.GetShared(bbGuid) != bbBefore
                  && boards.GetShared(bbGuid)->keys.size() == 2,
              "BB: 内容が変わった再読込は置き換える");
        check(trees.LoadFromFile(btReload, &unchanged) == btGuid && !unchanged && trees.GetShared(btGuid) != btReplaced,
              "BT: 内容が変わった再読込は置き換える");
        fs::remove_all(dir, ec);
    }

    // ---- 開いているシーンへの外部変更の保留 (M104n) ----
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path dir = fs::temp_directory_path(ec) / L"mye_reloadhub_scene_selftest";
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        const std::wstring scenePath = (dir / L"Open.scene.json").wstring();

        // ディスクの内容は disk を保存して作る。live は「ディスクを読んで、その後に未保存の編集を足した」シーン
        Scene disk;
        disk.CreateGameObjectTracked("Base");
        disk.GetWorld().ApplyStructuralChanges();
        check(SceneSerializer::SaveToFile(disk, scenePath), "(前提) シーンを保存できる");
        auto writeDisk = [&](const char* extraName) {
            disk.CreateGameObjectTracked(extraName);
            disk.GetWorld().ApplyStructuralChanges();
            SceneSerializer::SaveToFile(disk, scenePath);
        };
        auto has = [](Scene& s, const char* name) {
            s.GetWorld().ApplyStructuralChanges(); // 破棄の予約を反映してから探す
            return static_cast<bool>(s.Find(name));
        };
        auto makeLive = [&](Scene& live) {
            SceneSerializer::LoadFromFile(live, scenePath);
            // 保存前に作ったエンティティ。fileId を持つので ApplyDiff なら消える。
            // ディスク側で後から採番される id と衝突しないよう、離れた値にしておく
            GameObject unsaved = live.CreateGameObjectTracked("Unsaved");
            unsaved.GetComponent<FileIdComponent>()->value = 9000;
            live.GetWorld().ApplyStructuralChanges();
        };

        // 1. 口が未設定なら今までどおり適用する (要求 §16.3 の既存挙動の維持)
        {
            Scene live;
            makeLive(live);
            ReloadHub hub;
            ReloadHubTestAccess::Attach(hub, live, scenePath);
            writeDisk("D1");
            ReloadHubTestAccess::Change(hub, scenePath);
            check(has(live, "D1") && !has(live, "Unsaved") && !hub.HasPendingScene(),
                  "gate unset: an outside change is applied as before");
        }

        // 2. 口が偽: ライブのシーンは変わらず、保留が 1 件。同じパスが 2 回変わっても 1 件のまま、適用は最新の内容
        {
            Scene live;
            makeLive(live);
            ReloadHub hub;
            ReloadHubTestAccess::Attach(hub, live, scenePath);
            int asked = 0;
            hub.SetSceneApplyGate([&](const std::wstring&) {
                ++asked;
                return false;
            });
            writeDisk("D2a");
            ReloadHubTestAccess::Change(hub, scenePath);
            check(has(live, "Unsaved") && !has(live, "D2a") && hub.HasPendingScene() && asked == 1,
                  "gate says hold: the live scene is untouched and one change is pending");
            writeDisk("D2b");
            ReloadHubTestAccess::Change(hub, scenePath);
            check(hub.HasPendingScene() && hub.PendingScenePath() == NormalizePathKey(scenePath) && asked == 2,
                  "a second change to the same path keeps a single pending entry");
            hub.RequestApplyPendingScene();
            check(has(live, "Unsaved"), "applying is deferred to Update (nothing happens at request time)");
            hub.Update();
            check(has(live, "D2a") && has(live, "D2b") && !has(live, "Unsaved") && !hub.HasPendingScene()
                      && hub.TakePendingSceneResult() == ReloadHub::PendingSceneResult::Applied,
                  "Load applies the latest disk content, clears the pending entry and reports Applied");
            check(hub.TakePendingSceneResult() == ReloadHub::PendingSceneResult::None, "the result is reported once");
            check(asked == 2, "applying a pending change does not ask the gate again");
        }

        // 3. 保つ: 保留が消えてシーンは変わらない。以後の Update でも何も起きない
        {
            Scene live;
            makeLive(live);
            ReloadHub hub;
            ReloadHubTestAccess::Attach(hub, live, scenePath);
            hub.SetSceneApplyGate([](const std::wstring&) { return false; });
            writeDisk("D3");
            ReloadHubTestAccess::Change(hub, scenePath);
            hub.DiscardPendingScene();
            hub.Update();
            check(!hub.HasPendingScene() && has(live, "Unsaved") && !has(live, "D3"),
                  "Keep: the pending entry is dropped and the live scene is untouched");
        }

        // 4. 口が真 (未保存でない、自分の保存の直後など): 確認なしで適用される
        {
            Scene live;
            makeLive(live);
            ReloadHub hub;
            ReloadHubTestAccess::Attach(hub, live, scenePath);
            hub.SetSceneApplyGate([](const std::wstring&) { return true; });
            writeDisk("D4");
            ReloadHubTestAccess::Change(hub, scenePath);
            check(!hub.HasPendingScene() && has(live, "D4") && !has(live, "Unsaved"),
                  "gate says apply: applied without a pending entry");
        }

        // 5. Play 中を模す (口が偽 → 真へ変わる): 保留を持ち越し、真になってから適用できる
        {
            Scene live;
            makeLive(live);
            ReloadHub hub;
            ReloadHubTestAccess::Attach(hub, live, scenePath);
            bool playing = true;
            hub.SetSceneApplyGate([&](const std::wstring&) { return !playing; });
            writeDisk("D5");
            ReloadHubTestAccess::Change(hub, scenePath);
            hub.Update();
            check(hub.HasPendingScene() && has(live, "Unsaved"), "while playing: the change stays pending across frames");
            playing = false;
            hub.RequestApplyPendingScene();
            hub.Update();
            check(!hub.HasPendingScene() && !has(live, "Unsaved") && has(live, "D5"), "after play stops: the pending change can be applied");
        }

        // 6. 保留中にファイルが消えた: 保留を捨てて Gone を知らせる
        {
            Scene live;
            makeLive(live);
            ReloadHub hub;
            ReloadHubTestAccess::Attach(hub, live, scenePath);
            hub.SetSceneApplyGate([](const std::wstring&) { return false; });
            ReloadHubTestAccess::Change(hub, scenePath);
            const std::wstring moved = scenePath + L".moved";
            fs::rename(fs::path(scenePath), fs::path(moved), ec);
            hub.RequestApplyPendingScene();
            hub.Update();
            check(!hub.HasPendingScene() && has(live, "Unsaved")
                      && hub.TakePendingSceneResult() == ReloadHub::PendingSceneResult::Gone,
                  "file deleted while pending: the pending entry is dropped and Gone is reported");
            fs::rename(fs::path(moved), fs::path(scenePath), ec);
        }

        // 7. 一括適用 (git の操作・復元) は口を呼ばずに適用する
        {
            Scene live;
            makeLive(live);
            ReloadHub hub;
            ReloadHubTestAccess::Attach(hub, live, scenePath);
            int asked = 0;
            hub.SetSceneApplyGate([&](const std::wstring&) {
                ++asked;
                return false;
            });
            writeDisk("D7");
            hub.BeginBatch();
            hub.EndBatch({ { scenePath, BatchChange::Kind::Modified, L"" } });
            check(asked == 0 && has(live, "D7") && !has(live, "Unsaved") && !hub.HasPendingScene(),
                  "batch apply bypasses the gate and applies");
        }

        // 8. シーンを切り替えたら保留は捨てる
        {
            Scene live;
            makeLive(live);
            ReloadHub hub;
            ReloadHubTestAccess::Attach(hub, live, scenePath);
            hub.SetSceneApplyGate([](const std::wstring&) { return false; });
            ReloadHubTestAccess::Change(hub, scenePath);
            hub.SetActiveScenePath((dir / L"Other.scene.json").wstring());
            check(!hub.HasPendingScene(), "switching the open scene drops the pending entry");
        }
        fs::remove_all(dir, ec);
    }

    MYE_LOG_INFO("ReloadHub self test: %s (%d failure(s))", failCount == 0 ? "OK" : "FAILED", failCount);
    return failCount == 0;
}

} // namespace mye
