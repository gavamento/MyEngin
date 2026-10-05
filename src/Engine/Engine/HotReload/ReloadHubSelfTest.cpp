#include "Engine/Engine/HotReload/ReloadHubSelfTest.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/AI/BehaviorTreeLibrary.h"
#include "Engine/Engine/AI/BlackboardLibrary.h"
#include "Engine/Engine/HotReload/ReloadHub.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {

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
        { L"c:\\p\\assets\\ai\\guard.bt.json", ReloadKind::BehaviorTree, 6 },
        { L"c:\\p\\assets\\ai\\guard.bb.json", ReloadKind::Blackboard, 6 },
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

    MYE_LOG_INFO("ReloadHub self test: %s (%d failure(s))", failCount == 0 ? "OK" : "FAILED", failCount);
    return failCount == 0;
}

} // namespace mye
