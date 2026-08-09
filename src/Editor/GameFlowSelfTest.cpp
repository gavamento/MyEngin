#include "Editor/GameFlowSelfTest.h"

#include <cstring>
#include <filesystem>
#include <fstream>

#include "Editor/PlayModeController.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/GameFlow.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/SaveGame.h"
#include "Engine/Engine/Scene.h"

namespace mye {

namespace {

// EngineLoop の tick ループと同じ呼び方 (tick 毎に 1 回) でステップ数を数える
int CountSteps(TimeControl& tc, int ticks)
{
    int n = 0;
    for (int i = 0; i < ticks; ++i) {
        if (tc.Advance()) {
            ++n;
        }
    }
    return n;
}

} // namespace

bool RunGameFlowSelfTest()
{
    MYE_LOG_INFO("==== GameFlow self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- TimeControl: tick ゲートのステップ数 (決定台帳 5 の要検証項目) ----
    {
        TimeControl tc;
        check(CountSteps(tc, 600) == 600 && tc.accum == 0, "scale 100 -> 600/600 steps");
        tc = {};
        tc.scalePercent = 50;
        check(CountSteps(tc, 600) == 300, "scale 50 -> 300/600 steps");
        tc = {};
        tc.scalePercent = 25;
        check(CountSteps(tc, 600) == 150, "scale 25 -> 150/600 steps");
        tc = {};
        tc.scalePercent = 0;
        check(CountSteps(tc, 600) == 0, "scale 0 -> no steps");
        tc = {};
        tc.paused = true;
        check(CountSteps(tc, 600) == 0 && tc.accum == 0, "paused -> no steps, accum frozen");
        tc = {};
        tc.scalePercent = 250;
        check(CountSteps(tc, 600) == 600 && tc.accum == 0,
              "scale >100 clamps to one step per tick");
        tc = {};
        tc.scalePercent = -5;
        check(CountSteps(tc, 600) == 0, "negative scale clamps to 0");
        // ポーズは accum を凍結し、再開後は溜めた続きから位相が進む
        tc = {};
        tc.scalePercent = 50;
        tc.Advance(); // accum 50
        tc.paused = true;
        CountSteps(tc, 10);
        check(tc.accum == 50, "pause preserves accum");
        tc.paused = false;
        check(tc.Advance(), "resume steps on the next tick (50+50)");
    }

    // ---- WorldHash: TimeControl / PersistStore の被覆・挿入順不変 ----
    {
        World w;
        TimeControl tc;
        PersistStore a;
        const uint64_t clean = HashWorld(w, nullptr, &tc, &a);
        tc.paused = true;
        check(HashWorld(w, nullptr, &tc, &a) != clean, "paused is hash-covered");
        tc = {};
        tc.accum = 1;
        check(HashWorld(w, nullptr, &tc, &a) != clean, "accum is hash-covered");
        tc = {};
        tc.scalePercent = 50;
        check(HashWorld(w, nullptr, &tc, &a) != clean, "scalePercent is hash-covered");
        tc = {};
        check(HashWorld(w, nullptr, &tc, &a) == clean, "default TimeControl restores the hash");

        const uint32_t v1 = 123;
        const uint32_t v2 = 456;
        a.Set(HashStr("score"), &v1, sizeof(v1));
        a.Set(HashStr("stage"), &v2, sizeof(v2));
        a.Set(HashStr("flag"), nullptr, 0);
        PersistStore b; // 逆順に挿入しても std::map (キー昇順走査) なのでハッシュ同一
        b.Set(HashStr("flag"), nullptr, 0);
        b.Set(HashStr("stage"), &v2, sizeof(v2));
        b.Set(HashStr("score"), &v1, sizeof(v1));
        check(HashWorld(w, nullptr, &tc, &a) == HashWorld(w, nullptr, &tc, &b),
              "persist hash is insertion-order independent");
        check(HashWorld(w, nullptr, &tc, &a) != clean, "persist entries are hash-covered");
        const uint32_t v3 = 124;
        b.Set(HashStr("score"), &v3, sizeof(v3));
        check(HashWorld(w, nullptr, &tc, &a) != HashWorld(w, nullptr, &tc, &b),
              "persist value change is hash-covered");
        b.Set(HashStr("score"), &v1, sizeof(v1));
        b.Erase(HashStr("flag"));
        check(HashWorld(w, nullptr, &tc, &a) != HashWorld(w, nullptr, &tc, &b),
              "persist key removal is hash-covered");
    }

    // ---- PersistStore: API 意味論 ----
    {
        PersistStore s;
        const uint32_t v = 7;
        s.Set(1, &v, sizeof(v));
        const std::vector<uint8_t>* p = s.Find(1);
        check(p && p->size() == sizeof(v) && std::memcmp(p->data(), &v, sizeof(v)) == 0,
              "Set/Find round trips bytes");
        s.Set(2, nullptr, 0);
        check(s.Find(2) && s.Find(2)->empty(), "empty blob is present (distinct from missing)");
        check(s.Find(3) == nullptr, "missing key -> nullptr");
        s.Set(1, &v, 1); // 上書きは縮む方向も正しく反映される
        check(s.Find(1)->size() == 1, "Set overwrites (shrinking) in place");
        check(s.Erase(1) && !s.Find(1) && !s.Erase(1), "Erase removes exactly once");
    }

    // ---- SaveGameFile: 往復 + 破損耐性 ----
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path tempRoot = fs::temp_directory_path(ec) / L"mye_gameflow_selftest";
        fs::remove_all(tempRoot, ec);
        const std::wstring saveDir = tempRoot.wstring();
        const std::wstring path = SaveGameFile::PathForSlot(saveDir, 0);

        PersistStore s;
        std::vector<uint8_t> blob(256);
        for (int i = 0; i < 256; ++i) {
            blob[static_cast<size_t>(i)] = static_cast<uint8_t>(i); // 全バイト値で hex 往復
        }
        s.Set(HashStr("bytes"), blob.data(), blob.size());
        const uint32_t score = 4200;
        s.Set(HashStr("score"), &score, sizeof(score));
        s.Set(HashStr("empty"), nullptr, 0);

        check(SaveGameFile::Write(path, L"scenes\\stage1.scene.json", s),
              "write creates the save (and its directory)");
        SaveGameData data;
        check(SaveGameFile::Read(path, data), "read succeeds");
        check(data.scenePath == L"scenes\\stage1.scene.json", "scene path round trips");
        check(data.persist == s.Entries(), "persist map round trips bit-exact");

        // シーン無しセーブ (メモリ構築シーン) は空パスで往復する
        check(SaveGameFile::Write(path, L"", s) && SaveGameFile::Read(path, data)
                  && data.scenePath.empty(),
              "empty scene path round trips");

        SaveGameData d2;
        check(!SaveGameFile::Read(SaveGameFile::PathForSlot(saveDir, 9), d2),
              "missing slot -> false");
        {
            std::ofstream f(fs::path(path), std::ios::binary);
            f << "{ broken";
        }
        check(!SaveGameFile::Read(path, d2), "corrupt json -> false");
        {
            std::ofstream f(fs::path(path), std::ios::binary);
            f << R"({ "version": 1, "scene": "", "persist": { "zz": "00" } })";
        }
        check(!SaveGameFile::Read(path, d2), "bad hex key -> false (no partial load)");
        {
            std::ofstream f(fs::path(path), std::ios::binary);
            f << R"({ "version": 1, "scene": "", "persist": { "0000000000000001": "0" } })";
        }
        check(!SaveGameFile::Read(path, d2), "odd-length hex value -> false");
        {
            std::ofstream f(fs::path(path), std::ios::binary);
            f << R"({ "version": 99, "scene": "", "persist": {} })";
        }
        check(!SaveGameFile::Read(path, d2), "unknown version -> false");
        fs::remove_all(tempRoot, ec);
    }

    // ---- PlayModeController: Play/Stop で TimeControl / PersistStore が漏れない ----
    // (本サブの罠筆頭 — シーン文書スナップショットの外にある sim 状態の復元)
    {
        Scene scene;
        scene.CreateGameObjectTracked("thing");
        scene.GetWorld().ApplyStructuralChanges();
        const uint32_t keep = 1;
        scene.Persist().Set(HashStr("editor-side"), &keep, sizeof(keep));

        PlayModeController pmc;
        pmc.Play(scene);
        const uint32_t leak = 99;
        scene.Persist().Set(HashStr("in-play"), &leak, sizeof(leak));
        scene.Time().paused = true;
        scene.Time().scalePercent = 50;
        scene.Time().accum = 30;
        pmc.Stop(scene);
        check(!scene.Time().paused && scene.Time().scalePercent == 100
                  && scene.Time().accum == 0,
              "stop restores TimeControl");
        check(scene.Persist().Find(HashStr("in-play")) == nullptr,
              "stop drops persist values written during play");
        check(scene.Persist().Find(HashStr("editor-side")) != nullptr,
              "stop restores pre-play persist values");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("GameFlow self test: ALL PASS");
        return true;
    }
    MYE_LOG_ERROR("GameFlow self test: %d FAILED", failCount);
    return false;
}

} // namespace mye
