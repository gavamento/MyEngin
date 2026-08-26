#include "Engine/Engine/Replay/WorldHasherSelfTest.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "Engine/Core/Components.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene.h"

namespace mye {
namespace {

// 最終行の畳み込み列 (7 列目) を取り出す
uint64_t LastFold(const HashDump& d)
{
    if (d.lines.empty()) {
        return 0;
    }
    const std::string& s = d.lines.back();
    const size_t tab = s.rfind('\t');
    return (tab == std::string::npos) ? 0 : std::strtoull(s.c_str() + tab + 1, nullptr, 16);
}

// 部分文字列を全部含む最初の行の位置 (見つからなければ npos)
size_t FindLine(const HashDump& d, const char* a, const char* b)
{
    for (size_t i = 0; i < d.lines.size(); ++i) {
        if (d.lines[i].find(a) != std::string::npos && d.lines[i].find(b) != std::string::npos) {
            return i;
        }
    }
    return std::string::npos;
}

size_t CountLines(const HashDump& d, const char* needle)
{
    size_t n = 0;
    for (const std::string& l : d.lines) {
        if (l.find(needle) != std::string::npos) {
            ++n;
        }
    }
    return n;
}

} // namespace

bool RunWorldHasherSelfTest()
{
    MYE_LOG_INFO("==== WorldHasher (field dump / diff) self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- 被験ワールド: 親子 2 体 + ゲームフロー状態 ----
    Scene scene;
    GameObject alpha = scene.CreateGameObjectTracked("Alpha");
    GameObject beta = scene.CreateGameObjectTracked("Beta");
    beta.SetParent(alpha);
    World& w = scene.GetWorld();
    w.ApplyStructuralChanges();

    if (auto* t = w.GetComponent<LocalTransform>(beta.Id())) {
        t->position = { 1.5f, -2.25f, 0.75f };
    }
    scene.Time().paused = true;
    scene.Time().scalePercent = 50;
    const uint64_t goldKey = HashStr("gold");
    uint32_t gold = 7;
    scene.Persist().Set(goldKey, &gold, sizeof(gold));

    TimeControl& time = scene.Time();
    PersistStore& persist = scene.Persist();

    // ---- 3 出口の total 一致 ----
    // 走査が 3 実装に分かれると、診断だけ古い規則で歩いて嘘の行を指すようになる。
    // 「同一実装の 3 出口」であることをここで毎回固定する
    const uint64_t hPlain = HashWorld(w, {nullptr, &time, &persist});
    std::vector<EntityHash> ents;
    uint64_t hDetailed = 0;
    HashWorldDetailed(w, {nullptr, &time, &persist}, ents, hDetailed);
    HashDump base;
    HashWorldDump(w, {nullptr, &time, &persist}, /*tick=*/42, base);
    check(hPlain == hDetailed && hPlain == base.total,
          "3 exits (HashWorld / Detailed / Dump) agree on the total");
    check(!base.lines.empty() && LastFold(base) == base.total,
          "the fold column of the last line equals the total");
    check(CountLines(base, "\t#entity\t") == ents.size(), "one #entity row per hashed entity");
    check(FindLine(base, "\tWorld\trngState\t", "\t-\t") != std::string::npos
              && FindLine(base, "\tTimeControl\tscalePercent\t", "\t-\t") != std::string::npos
              && FindLine(base, "\tPersist\tblob\t", "\t-\t") != std::string::npos,
          "the dump covers the world RNG, TimeControl and PersistStore rows");

    // ---- 1 フィールドの変異は 1 行だけ報告される ----
    // 畳み込み列は 1 つ割れると以降が全部ずれるので、件数は値列で数える設計。
    // ここが崩れると「割れた場所」が数百行のノイズに埋もれて診断の意味が消える
    {
        auto* t = w.GetComponent<LocalTransform>(beta.Id());
        check(t != nullptr, "LocalTransform is a base component of every entity");
        if (t) {
            t->position.x += 1.0f;
        }
        HashDump moved;
        HashWorldDump(w, {nullptr, &time, &persist}, 42, moved);
        const HashDumpDiff d = DiffHashDumps(base, moved);
        const size_t posLine = FindLine(base, "\tLocalTransform\tposition\t", "\tBeta\t");
        check(d.valueDiffs == 1 && d.rollupDiffs == 1 && !d.structureDiffers && d.totalDiffers,
              "a single changed field yields exactly one differing leaf row (+ its rollup)");
        check(posLine != std::string::npos && d.firstFoldLine == posLine,
              "the diff points at the mutated field's row");
        if (t) {
            t->position.x -= 1.0f;
        }
        HashDump restored;
        HashWorldDump(w, {nullptr, &time, &persist}, 42, restored);
        check(DiffHashDumps(base, restored).Same(), "restoring the field makes the diff clean");
    }

    // ---- String64 の終端以降 (M48i の罠) ----
    // ハッシュは FieldTypeSize 分まるごと読むので、終端の先の残骸でも割れる。
    // ダンプが同じバイト範囲を出していないとこの差は診断から消える
    {
        // Name も LocalTransform と同じ基本アーキタイプの一部 (World のコンストラクタ)
        auto* nc = w.GetComponent<NameComponent>(beta.Id());
        const size_t n = nc ? std::strlen(nc->value) : 0;
        if (nc) {
            nc->value[n + 1] = 'X'; // 文字列としては "Beta" のまま (名前列は変わらない)
        }
        HashDump dirty;
        HashWorldDump(w, {nullptr, &time, &persist}, 42, dirty);
        const HashDumpDiff d = DiffHashDumps(base, dirty);
        check(nc && std::strlen(nc->value) == n && d.valueDiffs == 1 && d.rollupDiffs == 1
                  && !d.structureDiffers,
              "bytes past the String64 terminator show up as a field difference");
        check(d.firstFoldLine == FindLine(base, "\tName\tvalue\t", "\tBeta\t"),
              "the String64 difference is attributed to Name.value");
        if (nc) {
            std::memset(nc->value + n + 1, 0, sizeof(nc->value) - n - 1);
        }
        HashDump clean;
        HashWorldDump(w, {nullptr, &time, &persist}, 42, clean);
        check(DiffHashDumps(base, clean).Same(), "zeroing the tail restores the dump");
    }

    // ---- PersistStore の値も 1 行として見える ----
    {
        gold = 8;
        persist.Set(goldKey, &gold, sizeof(gold));
        HashDump rich;
        HashWorldDump(w, {nullptr, &time, &persist}, 42, rich);
        const HashDumpDiff d = DiffHashDumps(base, rich);
        check(d.valueDiffs == 1 && d.rollupDiffs == 0 && !d.structureDiffers
                  && d.firstFoldLine == FindLine(rich, "\tPersist\tblob\t", "\t-\t"),
              "a changed persist blob is a single differing row (no entity rollup involved)");
        gold = 7;
        persist.Set(goldKey, &gold, sizeof(gold));
    }

    // ---- ファイル往復 ----
    {
        std::error_code ec;
        const std::filesystem::path tmp =
            std::filesystem::temp_directory_path(ec) / "mye_hashdump_selftest.dump";
        check(WriteHashDump(tmp.wstring(), base), "WriteHashDump succeeds");
        HashDump loaded;
        check(ReadHashDump(tmp.wstring(), loaded), "ReadHashDump succeeds");
        check(loaded.tick == base.tick && loaded.total == base.total && loaded.lines == base.lines,
              "the dump survives a file round trip byte for byte");
        check(DiffHashDumps(base, loaded).Same(), "a round-tripped dump diffs clean");
        std::filesystem::remove(tmp, ec);
    }

    // ---- 行がずれる差 (エンティティ増減) は構造差として報告する ----
    // 値列だけ比べていると、途中で 1 体増えた瞬間に以降の全行が「別フィールド同士」の
    // 比較になり、差分が意味のない大量出力に化ける。突き合わせ不能はそう言う
    {
        GameObject gamma = scene.CreateGameObjectTracked("Gamma");
        (void)gamma;
        w.ApplyStructuralChanges();
        HashDump grown;
        HashWorldDump(w, {nullptr, &time, &persist}, 42, grown);
        const HashDumpDiff d = DiffHashDumps(base, grown);
        check(d.structureDiffers && d.totalDiffers,
              "adding an entity is reported as a structure difference");
    }

    MYE_LOG_INFO("==== WorldHasher self test: %s ====", failCount == 0 ? "PASS" : "FAIL");
    return failCount == 0;
}

} // namespace mye
