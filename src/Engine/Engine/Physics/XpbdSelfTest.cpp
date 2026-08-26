//====================================================================================
//                          XpbdSelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      08/27/2026
//                                          XpbdBackend の内容ゲート/被覆/snapshot 往復検査
//====================================================================================
#include "Engine/Engine/Physics/XpbdSelfTest.h"

#include <cstddef>
#include <vector>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Physics/XpbdBackend.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene.h"

namespace mye {
namespace {

// 手で組んだ検査用の池 (2 粒子 + 1 距離拘束)。ソルバはまだ無いので値は任意だが、
// 「全フィールドが被覆に入っているか」を 1 フィールドずつ変異で確かめるため、
// 各配列が互いに違う値を持つようにしてある (同値だと px と py の取り違えを検出できない)
XpbdBackend::Pool MakeProbePool(EntityID owner)
{
    XpbdBackend::Pool p;
    p.owner = owner;
    p.kind = static_cast<uint32_t>(XpbdBackend::PoolKind::Rope);
    p.px = { 0.5f, 1.5f };
    p.py = { 2.5f, 3.5f };
    p.pz = { -0.25f, 0.25f };
    p.vx = { 0.125f, -0.125f };
    p.vy = { 4.0f, 5.0f };
    p.vz = { -6.0f, 7.0f };
    p.prevX = { 0.4375f, 1.4375f };
    p.prevY = { 2.4375f, 3.4375f };
    p.prevZ = { -0.3125f, 0.1875f };
    p.invMass = { 0.0f, 2.0f }; // 先頭はピン留め
    p.ca = { 0u };
    p.cb = { 1u };
    p.rest = { 1.0f };
    return p;
}

} // namespace

bool RunXpbdSelfTest()
{
    MYE_LOG_INFO("==== Xpbd backend (M60'b) self test ====");
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
    GameObject owner = scene.CreateGameObjectTracked("XpbdOwner");
    World& w = scene.GetWorld();
    w.ApplyStructuralChanges();

    // ---- 1. 内容ゲート: 空の池は配線してもハッシュを 1 ビットも動かさない ----
    XpbdBackend backend;
    SimSources srcXpbd;
    srcXpbd.xpbd = &backend;
    const uint64_t hashPlain = HashWorld(w);
    check(HashWorld(w, srcXpbd) == hashPlain,
          "an empty backend folds nothing (content gate keeps the hash intact)");

    // ---- 2. 池の全フィールドが被覆に入っている ----
    backend.PoolsForSnapshot().push_back(MakeProbePool(owner.Id()));
    const uint64_t hashPool = HashWorld(w, srcXpbd);
    check(hashPool != hashPlain, "a populated pool changes the hash");

    // 1 フィールドずつ変異させて必ず割れることを確かめる。戻し忘れ防止のため
    // 「変異 → 比較 → 復元 → 復元確認」を 1 か所で回す
    auto mutateCheck = [&](const char* what, auto&& mutate, auto&& restore) {
        mutate();
        const bool moved = HashWorld(w, srcXpbd) != hashPool;
        restore();
        const bool restored = HashWorld(w, srcXpbd) == hashPool;
        check(moved && restored, what);
    };
    XpbdBackend::Pool& pool = backend.PoolsForSnapshot()[0];
    mutateCheck("px is hash-covered", [&] { pool.px[0] += 1.0f; }, [&] { pool.px[0] -= 1.0f; });
    mutateCheck("py is hash-covered", [&] { pool.py[1] += 1.0f; }, [&] { pool.py[1] -= 1.0f; });
    mutateCheck("pz is hash-covered", [&] { pool.pz[0] += 1.0f; }, [&] { pool.pz[0] -= 1.0f; });
    mutateCheck("vx is hash-covered", [&] { pool.vx[0] += 1.0f; }, [&] { pool.vx[0] -= 1.0f; });
    mutateCheck("vy is hash-covered", [&] { pool.vy[1] += 1.0f; }, [&] { pool.vy[1] -= 1.0f; });
    mutateCheck("vz is hash-covered", [&] { pool.vz[0] += 1.0f; }, [&] { pool.vz[0] -= 1.0f; });
    mutateCheck("prevX is hash-covered", [&] { pool.prevX[0] += 1.0f; },
                [&] { pool.prevX[0] -= 1.0f; });
    mutateCheck("prevY is hash-covered", [&] { pool.prevY[1] += 1.0f; },
                [&] { pool.prevY[1] -= 1.0f; });
    mutateCheck("prevZ is hash-covered", [&] { pool.prevZ[0] += 1.0f; },
                [&] { pool.prevZ[0] -= 1.0f; });
    mutateCheck("invMass is hash-covered", [&] { pool.invMass[1] += 1.0f; },
                [&] { pool.invMass[1] -= 1.0f; });
    mutateCheck("ca is hash-covered", [&] { pool.ca[0] = 1u; }, [&] { pool.ca[0] = 0u; });
    mutateCheck("cb is hash-covered", [&] { pool.cb[0] = 0u; }, [&] { pool.cb[0] = 1u; });
    mutateCheck("rest is hash-covered", [&] { pool.rest[0] += 0.5f; },
                [&] { pool.rest[0] -= 0.5f; });
    mutateCheck("kind is hash-covered",
                [&] { pool.kind = static_cast<uint32_t>(XpbdBackend::PoolKind::Cloth); },
                [&] { pool.kind = static_cast<uint32_t>(XpbdBackend::PoolKind::Rope); });
    mutateCheck("owner is hash-covered", [&] { ++pool.owner.generation; },
                [&] { --pool.owner.generation; });

    // ---- 3. SimSnapshot v4 の往復 ----
    SimRefs refs;
    refs.scene = &scene;
    refs.xpbd = &backend;
    uint64_t tick = 60;
    refs.tickIndex = &tick;

    std::vector<std::byte> blob;
    check(CaptureSimSnapshot(refs, blob), "capture succeeds with a populated pool");

    // 池を徹底的に壊してから戻す (要素数まで変える = resize 経路も通す)
    backend.PoolsForSnapshot().clear();
    backend.PoolsForSnapshot().push_back(MakeProbePool(owner.Id()));
    backend.PoolsForSnapshot()[0].px = { 9.0f };
    backend.PoolsForSnapshot()[0].kind = static_cast<uint32_t>(XpbdBackend::PoolKind::SoftBody);
    backend.PoolsForSnapshot().push_back(MakeProbePool(owner.Id()));
    check(HashWorld(w, srcXpbd) != hashPool, "the pools really diverged before restore");

    check(RestoreSimSnapshot(refs, blob.data(), blob.size()), "restore succeeds");
    check(HashWorld(w, srcXpbd) == hashPool,
          "restore is bit-identical (hash returns to the captured value)");
    check(backend.Pools().size() == 1 && backend.Pools()[0].px.size() == 2,
          "restore rebuilds the pool shape (count and array sizes)");

    // ---- 4. blob レイアウトは refs の構成に依らない ----
    // null と「空 backend」で同じ blob になること = 「節ごとの参照が null なら空の節」の契約。
    // これが崩れると、クラッシュ .rep が「どの構成で撮ったか」に縛られてしまう
    backend.Reset();
    std::vector<std::byte> blobWithEmpty;
    check(CaptureSimSnapshot(refs, blobWithEmpty), "capture succeeds with an empty backend");
    SimRefs refsNoXpbd = refs;
    refsNoXpbd.xpbd = nullptr;
    std::vector<std::byte> blobNoXpbd;
    check(CaptureSimSnapshot(refsNoXpbd, blobNoXpbd), "capture succeeds without a backend");
    check(blobWithEmpty == blobNoXpbd,
          "a null backend and an empty backend write the same blob (layout independence)");

    // ---- 5. refs に池が無い構成は節を読み捨てる ----
    // (blob には池入りの節があるが、受け手が居なければ黙って捨てて他の節は復元する)
    backend.PoolsForSnapshot().push_back(MakeProbePool(owner.Id()));
    std::vector<std::byte> blobWithPool;
    check(CaptureSimSnapshot(refs, blobWithPool), "capture succeeds for the discard probe");
    backend.PoolsForSnapshot().clear();
    check(RestoreSimSnapshot(refsNoXpbd, blobWithPool.data(), blobWithPool.size()),
          "restore succeeds when the refs have no backend");
    check(backend.Pools().empty(), "the xpbd section is discarded, not force-applied");

    if (failCount == 0) {
        MYE_LOG_INFO("==== Xpbd backend self test: ALL PASS ====");
    } else {
        MYE_LOG_ERROR("==== Xpbd backend self test: %d FAILED ====", failCount);
    }
    return failCount == 0;
}

} // namespace mye
