#include "Engine/Engine/Replay/SimSnapshotSelfTest.h"

#include <cstddef>
#include <string>
#include <vector>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene.h"

namespace mye {
namespace {

// アーキタイプ列の「形」を 1 本の文字列にする (生成順つき)。
// 順序が変わると ForEachArchetype の列挙順 = ハッシュの畳み込み順が変わるので、
// 復元の合否はここまで見ないと分からない
std::string ArchetypeShape(const World& w)
{
    std::string s;
    for (const auto& arch : w.Archetypes()) {
        s += "[";
        for (ComponentTypeId t : arch->Types()) {
            s += std::to_string(t);
            s += ",";
        }
        s += "#" + std::to_string(arch->Count()) + "]";
    }
    return s;
}

// 被験シーンを組む: 親子 + 別々のコンポーネント構成 (= 複数アーキタイプ) +
// 破棄済みスロット (= freeIndices に穴) + ゲームフロー状態
void BuildScene(Scene& scene)
{
    World& w = scene.GetWorld();
    GameObject root = scene.CreateGameObjectTracked("Root");
    GameObject a = scene.CreateGameObjectTracked("Alpha");
    GameObject b = scene.CreateGameObjectTracked("Beta");
    GameObject c = scene.CreateGameObjectTracked("Gamma");
    GameObject d = scene.CreateGameObjectTracked("Delta");
    a.SetParent(root);
    b.SetParent(root);
    // 生成順が異なるアーキタイプを 3 つ作る (順序保存の検査対象)
    a.AddComponent<MeshRendererComponent>();
    b.AddComponent<ColliderComponent>();
    c.AddComponent<MeshRendererComponent>();
    c.AddComponent<ColliderComponent>();
    w.ApplyStructuralChanges();

    if (auto* t = w.GetComponent<LocalTransform>(a.Id())) {
        t->position = { 1.5f, -2.25f, 0.75f };
    }
    if (auto* t = w.GetComponent<LocalTransform>(c.Id())) {
        t->scale = { 2.0f, 3.0f, 4.0f };
    }
    // スロットに穴を開けて freeIndices を LIFO で埋める (復元で順序が崩れると
    // 次に採番される EntityID が変わる = 以降のリプレイが丸ごとずれる)
    w.DestroyEntity(d.Id());
    w.DestroyEntity(b.Id());
    w.ApplyStructuralChanges();

    scene.Time().paused = true;
    scene.Time().scalePercent = 40;
    scene.Time().accum = 30;
    const uint64_t score = 1234;
    scene.Persist().Set(0xABCDull, &score, sizeof(score));
    scene.Persist().Set(0x1111ull, "hi", 2);
    scene.SetSourcePath(L"C:\\proj\\assets\\scenes\\probe.scene.json");
    scene.SetOverrides(3, { "LocalTransform.position", "name" });
    scene.SetOverrides(1, {}); // 空集合でも「記録あり」= レガシー判定と区別される
    w.Rng().NextU32();
    w.Rng().NextU32();
}

} // namespace

bool RunSimSnapshotSelfTest()
{
    MYE_LOG_INFO("==== SimSnapshot (capture / restore) self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    Scene scene;
    BuildScene(scene);
    World& w = scene.GetWorld();

    SimRefs refs;
    refs.scene = &scene;
    // M52g: 前 tick 入力は kMaxPlayers 本のレーン配列。レーンごとに違う目印を入れて
    // 「往復でレーンが混ざらない」ことまで固定する
    InputSnapshot prevInput[kMaxPlayers] = {};
    prevInput[0].mouseX = 321;
    prevInput[0].mouseY = 654;
    prevInput[1].mouseX = 1321;
    prevInput[kMaxPlayers - 1].mouseX = 4321;
    refs.prevTickInput = prevInput;
    uint64_t tick = 4242;
    refs.tickIndex = &tick;

    const uint64_t hash0 = HashWorld(w, nullptr, &scene.Time(), &scene.Persist());
    const std::string shape0 = ArchetypeShape(w);
    const uint64_t rngState0 = w.Rng().State();
    const uint64_t nextFileId0 = scene.PeekNextFileId();

    std::vector<std::byte> blob;
    check(CaptureSimSnapshot(refs, blob), "capture succeeds");
    check(!blob.empty(), "blob is not empty");
    uint64_t peeked = 0;
    check(PeekSimSnapshotTick(blob.data(), blob.size(), peeked) && peeked == 4242,
          "header carries the tick index");

    // ---- 撮影後に「次に採番される EntityID」を確かめる (LIFO 順の期待値) ----
    const EntityID expectNext = w.CreateEntity("AfterCapture");
    w.ApplyStructuralChanges();

    // ---- 世界を徹底的に壊す ----
    GameObject junk = scene.CreateGameObjectTracked("Junk");
    junk.AddComponent<LightComponent>(); // 撮影時に存在しないアーキタイプを増やす
    w.ApplyStructuralChanges();
    if (auto* t = w.GetComponent<LocalTransform>(junk.Id())) {
        t->position = { 99.0f, 99.0f, 99.0f };
    }
    w.Rng().NextU32();
    scene.Time().paused = false;
    scene.Time().scalePercent = 100;
    scene.Persist().Clear();
    scene.SetSourcePath(L"");
    scene.ReplaceOverridesTable({});
    scene.SetNextFileId(9999);
    for (uint32_t p = 0; p < kMaxPlayers; ++p) {
        prevInput[p] = {};
    }
    tick = 0;
    check(HashWorld(w, nullptr, &scene.Time(), &scene.Persist()) != hash0,
          "the world really diverged before restore");

    // ---- 戻す ----
    check(RestoreSimSnapshot(refs, blob.data(), blob.size()), "restore succeeds");
    check(HashWorld(w, nullptr, &scene.Time(), &scene.Persist()) == hash0,
          "world hash is bit-identical after restore");
    check(ArchetypeShape(w) == shape0, "archetype creation order and row counts are preserved");
    check(w.Rng().State() == rngState0, "world RNG state is restored");
    check(scene.PeekNextFileId() == nextFileId0, "scene nextFileId is restored");
    check(scene.SourcePath() == L"C:\\proj\\assets\\scenes\\probe.scene.json",
          "scene source path is restored");
    check(scene.Persist().Find(0xABCDull) != nullptr
              && scene.Persist().Find(0xABCDull)->size() == sizeof(uint64_t),
          "persist entries are restored");
    check(scene.Time().paused && scene.Time().scalePercent == 40 && scene.Time().accum == 30,
          "time control is restored");
    check(scene.HasOverrideRecord(1) && scene.GetOverrides(3) != nullptr
              && scene.GetOverrides(3)->size() == 2,
          "prefab override records are restored");
    check(prevInput[0].mouseX == 321 && prevInput[0].mouseY == 654,
          "prev tick input is restored");
    check(prevInput[1].mouseX == 1321 && prevInput[kMaxPlayers - 1].mouseX == 4321,
          "...for every input lane (M52g)");
    check(tick == 4242, "tick index is restored");

    // ---- ハッシュに出ない状態: EntityID 世代と freeIndices の LIFO 順 ----
    const EntityID actualNext = w.CreateEntity("AfterRestore");
    w.ApplyStructuralChanges();
    check(actualNext.index == expectNext.index && actualNext.generation == expectNext.generation,
          "next EntityID (free-list LIFO order + generation) matches the pre-restore world");

    // ---- 撮り直しがバイト同一になる (= 同じ状態なら同じ blob) ----
    // ここが崩れると desync 診断も M52i のロールバック比較も足場を失う。
    // 直前の CreateEntity を戻してから撮り直す
    check(RestoreSimSnapshot(refs, blob.data(), blob.size()), "restore (second time) succeeds");
    std::vector<std::byte> blob2;
    check(CaptureSimSnapshot(refs, blob2), "re-capture succeeds");
    check(blob2 == blob, "re-captured blob is byte-identical");

    // ---- 壊れた blob は「何も書き換えずに」失敗すること ----
    const uint64_t hashBefore = HashWorld(w, nullptr, &scene.Time(), &scene.Persist());
    std::vector<std::byte> truncated(blob.begin(), blob.begin() + blob.size() / 2);
    check(!RestoreSimSnapshot(refs, truncated.data(), truncated.size()),
          "truncated blob is rejected");
    check(HashWorld(w, nullptr, &scene.Time(), &scene.Persist()) == hashBefore,
          "a rejected restore leaves the world untouched");
    std::vector<std::byte> garbage = blob;
    garbage[0] = static_cast<std::byte>(0x00);
    check(!RestoreSimSnapshot(refs, garbage.data(), garbage.size()), "bad magic is rejected");

    if (failCount == 0) {
        MYE_LOG_INFO("==== SimSnapshot self test: ALL PASS (blob %zu bytes) ====", blob.size());
    } else {
        MYE_LOG_ERROR("==== SimSnapshot self test: %d FAILED ====", failCount);
    }
    return failCount == 0;
}

} // namespace mye
