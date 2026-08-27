#include "Engine/Engine/Replay/SimSnapshot.h"

#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "Engine/Core/ByteIo.h"
#include "Engine/Core/Check.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/CollisionSystem.h"
#include "Engine/Engine/Particles/CpuParticleBackend.h"
#include "Engine/Engine/Physics/XpbdBackend.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/Script/ScriptHost.h"

namespace mye {
namespace {

// 節ごとの目印。壊れた/別形式の blob を「途中まで復元」する前に止めるため
constexpr uint32_t kMagic = 0x504E534Du;      // 'MSNP'
constexpr uint32_t kSceneMagic = 0x314E4353u; // 'SCN1'
constexpr uint32_t kPtclMagic = 0x31435450u;  // 'PTC1'
constexpr uint32_t kCollMagic = 0x314C4F43u;  // 'COL1'
constexpr uint32_t kScrMagic = 0x31524353u;   // 'SCR1'
constexpr uint32_t kLoopMagic = 0x31504F4Cu;  // 'LOP1'
constexpr uint32_t kXpbdMagic = 0x31425058u;  // 'XPB1' (M60'b)

constexpr size_t kHeaderBytes = 4 * sizeof(uint32_t) + sizeof(uint64_t);

// ---- Scene (TimeControl / PersistStore / nextFileId / sourcePath / override 表) ----
//
// override 表は unordered_map なので、そのまま走査すると blob のバイト列が実行ごとに
// 変わる (規則 7)。fileId 昇順に整列してから書く — 「同じ状態なら同じ blob」は
// 撮り直しの比較・desync 診断・selftest の全てが依存する不変条件
void WriteScene(ByteWriter& w, const Scene& scene)
{
    w.U32(kSceneMagic);
    const TimeControl& t = scene.Time();
    w.U8(t.paused ? 1u : 0u);
    w.I32(t.scalePercent);
    w.I32(t.accum);

    const PersistStore::Map& persist = scene.Persist().Entries(); // std::map = キー昇順
    w.Count(persist.size());
    for (const auto& [key, blob] : persist) {
        w.U64(key);
        w.Blob(blob.data(), blob.size());
    }

    w.U64(scene.PeekNextFileId());
    w.WStr(scene.SourcePath());

    const std::unordered_map<uint64_t, Scene::OverrideSet>& ov = scene.OverridesTable();
    std::vector<uint64_t> ids;
    ids.reserve(ov.size());
    for (const auto& kv : ov) {
        ids.push_back(kv.first);
    }
    std::sort(ids.begin(), ids.end());
    w.Count(ids.size());
    for (uint64_t id : ids) {
        w.U64(id);
        const Scene::OverrideSet& keys = ov.find(id)->second; // std::set = 昇順
        w.Count(keys.size());
        for (const std::string& k : keys) {
            w.Str(k);
        }
    }
}

struct SceneState {
    TimeControl time;
    PersistStore::Map persist;
    uint64_t nextFileId = 1;
    std::wstring sourcePath;
    std::unordered_map<uint64_t, Scene::OverrideSet> overrides;
};

bool ReadScene(ByteReader& r, SceneState& out)
{
    if (r.U32() != kSceneMagic) {
        MYE_LOG_ERROR("[snapshot] scene section magic mismatch");
        return false;
    }
    out.time.paused = r.U8() != 0;
    out.time.scalePercent = r.I32();
    out.time.accum = r.I32();

    const size_t persistCount = r.Count(sizeof(uint64_t) * 2);
    for (size_t i = 0; i < persistCount && r.Ok(); ++i) {
        const uint64_t key = r.U64();
        const size_t n = r.Count(sizeof(uint8_t));
        std::vector<uint8_t> blob(n);
        r.Raw(blob.data(), n);
        out.persist.emplace(key, std::move(blob));
    }

    out.nextFileId = r.U64();
    out.sourcePath = r.WStr();

    const size_t ovCount = r.Count(sizeof(uint64_t) * 2);
    for (size_t i = 0; i < ovCount && r.Ok(); ++i) {
        const uint64_t id = r.U64();
        const size_t keyCount = r.Count(sizeof(uint64_t));
        Scene::OverrideSet keys;
        for (size_t k = 0; k < keyCount && r.Ok(); ++k) {
            keys.insert(r.Str());
        }
        out.overrides.emplace(id, std::move(keys));
    }
    return r.Ok();
}

// ---- CPU パーティクル池 ----
// SoA 配列は要素数が数千になりうるので生バイトで一括 (要素毎ループは撮影コストに直結する)
void WriteParticles(ByteWriter& w, const CpuParticleBackend* particles)
{
    w.U32(kPtclMagic);
    w.U32(static_cast<uint32_t>(sizeof(ParticleEmitterComponent)));
    const std::vector<CpuParticleBackend::EmitterPool> empty;
    const std::vector<CpuParticleBackend::EmitterPool>& pools =
        particles != nullptr ? particles->Pools() : empty;
    w.Count(pools.size());
    for (const CpuParticleBackend::EmitterPool& p : pools) {
        w.U32(p.owner.index);
        w.U32(p.owner.generation);
        w.U32(p.alive);
        w.F32(p.emitAccum);
        w.I32(p.ageTicks);
        w.U64(p.rng.State());
        w.U64(p.rng.Inc());
        w.F32(p.prevOrigin.x); // M61a (v5)
        w.F32(p.prevOrigin.y);
        w.F32(p.prevOrigin.z);
        w.U32(p.prevOriginValid);
        w.U32(p.prewarmed);
        w.Raw(&p.descCache, sizeof(ParticleEmitterComponent));
        w.PodVector(p.px);
        w.PodVector(p.py);
        w.PodVector(p.pz);
        w.PodVector(p.vx);
        w.PodVector(p.vy);
        w.PodVector(p.vz);
        w.PodVector(p.life);
        w.PodVector(p.invLife);
        w.PodVector(p.size0);
    }
}

bool ReadParticles(ByteReader& r, std::vector<CpuParticleBackend::EmitterPool>& out)
{
    if (r.U32() != kPtclMagic) {
        MYE_LOG_ERROR("[snapshot] particle section magic mismatch");
        return false;
    }
    const uint32_t descSize = r.U32();
    if (descSize != sizeof(ParticleEmitterComponent)) {
        MYE_LOG_ERROR("[snapshot] emitter layout changed (%u vs %zu bytes)", descSize,
                      sizeof(ParticleEmitterComponent));
        return false;
    }
    const size_t count = r.Count(sizeof(uint32_t) * 4);
    out.resize(count);
    for (size_t i = 0; i < count && r.Ok(); ++i) {
        CpuParticleBackend::EmitterPool& p = out[i];
        p.owner.index = r.U32();
        p.owner.generation = r.U32();
        p.alive = r.U32();
        p.emitAccum = r.F32();
        p.ageTicks = r.I32();
        const uint64_t state = r.U64();
        const uint64_t inc = r.U64();
        p.rng.Restore(state, inc);
        p.prevOrigin.x = r.F32(); // M61a (v5)
        p.prevOrigin.y = r.F32();
        p.prevOrigin.z = r.F32();
        p.prevOriginValid = r.U32();
        p.prewarmed = r.U32();
        r.Raw(&p.descCache, sizeof(ParticleEmitterComponent));
        p.px = r.PodVector<float>();
        p.py = r.PodVector<float>();
        p.pz = r.PodVector<float>();
        p.vx = r.PodVector<float>();
        p.vy = r.PodVector<float>();
        p.vz = r.PodVector<float>();
        p.life = r.PodVector<float>();
        p.invLife = r.PodVector<float>();
        p.size0 = r.PodVector<float>();
    }
    return r.Ok();
}

// ---- CollisionSystem の前 tick ペア ----
void WriteCollision(ByteWriter& w, CollisionSystem* collision)
{
    w.U32(kCollMagic);
    const std::vector<uint64_t> empty;
    w.PodVector(collision != nullptr ? collision->PrevPairsForSnapshot() : empty);
    w.PodVector(collision != nullptr ? collision->PrevSolidPairsForSnapshot() : empty);
}

// ---- ScriptHost の Start 済み記録 ----
// unordered_set なので昇順へ整列してから書く (override 表と同じ理由)
void WriteScripts(ByteWriter& w, ScriptHost* scripts)
{
    w.U32(kScrMagic);
    std::vector<uint64_t> keys;
    if (scripts != nullptr) {
        const std::unordered_set<uint64_t>& started = scripts->StartedForSnapshot();
        keys.assign(started.begin(), started.end());
        std::sort(keys.begin(), keys.end());
    }
    w.PodVector(keys);
}

// ---- XPBD 変形体の池 (M60'b) ----
// パーティクル節と同じ流儀: null なら空の節を書く = blob レイアウトは refs の構成に依らず
// 常に同じ。SoA は PodVector で一括 (要素毎ループは撮影コストに直結する)
void WriteXpbd(ByteWriter& w, const XpbdBackend* xpbd)
{
    w.U32(kXpbdMagic);
    const std::vector<XpbdBackend::Pool> empty;
    const std::vector<XpbdBackend::Pool>& pools = xpbd != nullptr ? xpbd->Pools() : empty;
    w.Count(pools.size());
    for (const XpbdBackend::Pool& p : pools) {
        w.U32(p.owner.index);
        w.U32(p.owner.generation);
        w.U32(p.kind);
        w.PodVector(p.px);
        w.PodVector(p.py);
        w.PodVector(p.pz);
        w.PodVector(p.vx);
        w.PodVector(p.vy);
        w.PodVector(p.vz);
        w.PodVector(p.prevX);
        w.PodVector(p.prevY);
        w.PodVector(p.prevZ);
        w.PodVector(p.invMass);
        w.PodVector(p.ca);
        w.PodVector(p.cb);
        w.PodVector(p.rest);
    }
}

bool ReadXpbd(ByteReader& r, std::vector<XpbdBackend::Pool>& out)
{
    if (r.U32() != kXpbdMagic) {
        MYE_LOG_ERROR("[snapshot] xpbd section magic mismatch");
        return false;
    }
    const size_t count = r.Count(sizeof(uint32_t) * 3);
    out.resize(count);
    for (size_t i = 0; i < count && r.Ok(); ++i) {
        XpbdBackend::Pool& p = out[i];
        p.owner.index = r.U32();
        p.owner.generation = r.U32();
        p.kind = r.U32();
        p.px = r.PodVector<float>();
        p.py = r.PodVector<float>();
        p.pz = r.PodVector<float>();
        p.vx = r.PodVector<float>();
        p.vy = r.PodVector<float>();
        p.vz = r.PodVector<float>();
        p.prevX = r.PodVector<float>();
        p.prevY = r.PodVector<float>();
        p.prevZ = r.PodVector<float>();
        p.invMass = r.PodVector<float>();
        p.ca = r.PodVector<uint32_t>();
        p.cb = r.PodVector<uint32_t>();
        p.rest = r.PodVector<float>();
    }
    return r.Ok();
}

// ---- EngineLoop の tick 間キャリー (M51d の前 tick 入力 + M52e の音ハンドル採番) ----
void WriteLoop(ByteWriter& w, const InputSnapshot* prevTickInput, const uint64_t* audioHandleSeq)
{
    w.U32(kLoopMagic);
    // M52g: 前 tick 入力は **kMaxPlayers 本のレーン配列**。実効レーン数 (playerCount) では
    // なく常に上限本数を書くのは、blob のレイアウトを起動オプションから独立させるため —
    // --local-players 2 で撮った blob を 1 レーンの実行で復元できないと、
    // クラッシュ .rep がマルチプレイ設定に縛られてしまう
    w.U32(kMaxPlayers);
    const InputSnapshot zero = {};
    for (uint32_t p = 0; p < kMaxPlayers; ++p) {
        w.Raw(prevTickInput != nullptr ? &prevTickInput[p] : &zero, sizeof(InputSnapshot));
    }
    w.U64(audioHandleSeq != nullptr ? *audioHandleSeq : 0);
}

} // namespace

bool CaptureSimSnapshot(const SimRefs& refs, std::vector<std::byte>& out)
{
    if (refs.scene == nullptr) {
        MYE_LOG_ERROR("[snapshot] capture without a scene");
        return false;
    }
    out.clear();
    ByteWriter w(out);
    w.U32(kMagic);
    w.U32(kSimSnapshotVersion);
    w.U32(static_cast<uint32_t>(sizeof(InputSnapshot)));
    w.U32(0); // 予約 (将来の節追加フラグ用)
    w.U64(refs.tickIndex != nullptr ? *refs.tickIndex : 0);

    WriteScene(w, *refs.scene);
    WriteParticles(w, refs.particles);
    WriteCollision(w, refs.collision);
    WriteScripts(w, refs.scripts);
    WriteLoop(w, refs.prevTickInput, refs.audioHandleSeq);
    WriteXpbd(w, refs.xpbd); // M60'b (v4)
    // ★World は**最後**に置く。復元は「小さい節を全部一時領域へ読み切ってから
    //   World::SnapshotRead (それ自体が全読み後に一括差し替え) を呼ぶ」順で走るので、
    //   どこで失敗しても現世界に手が付いていない状態で戻れる
    refs.scene->GetWorld().SnapshotWrite(w);
    return true;
}

bool RestoreSimSnapshot(const SimRefs& refs, const std::byte* data, size_t size)
{
    if (refs.scene == nullptr || data == nullptr) {
        MYE_LOG_ERROR("[snapshot] restore without a scene/blob");
        return false;
    }
    ByteReader r(data, size);
    if (r.U32() != kMagic) {
        MYE_LOG_ERROR("[snapshot] bad blob magic");
        return false;
    }
    const uint32_t version = r.U32();
    const uint32_t inputSize = r.U32();
    r.U32(); // 予約
    const uint64_t tick = r.U64();
    if (version != kSimSnapshotVersion || inputSize != sizeof(InputSnapshot)) {
        MYE_LOG_ERROR("[snapshot] incompatible blob (v%u, input %u bytes)", version, inputSize);
        return false;
    }

    SceneState scene;
    std::vector<CpuParticleBackend::EmitterPool> pools;
    if (!ReadScene(r, scene) || !ReadParticles(r, pools)) {
        return false;
    }
    if (r.U32() != kCollMagic) {
        MYE_LOG_ERROR("[snapshot] collision section magic mismatch");
        return false;
    }
    std::vector<uint64_t> prevPairs = r.PodVector<uint64_t>();
    std::vector<uint64_t> prevSolidPairs = r.PodVector<uint64_t>();
    if (r.U32() != kScrMagic) {
        MYE_LOG_ERROR("[snapshot] script section magic mismatch");
        return false;
    }
    std::vector<uint64_t> started = r.PodVector<uint64_t>();
    if (r.U32() != kLoopMagic) {
        MYE_LOG_ERROR("[snapshot] loop section magic mismatch");
        return false;
    }
    const uint32_t laneCount = r.U32();
    if (laneCount != kMaxPlayers) {
        MYE_LOG_ERROR("[snapshot] lane count mismatch (blob %u, build %u)", laneCount, kMaxPlayers);
        return false;
    }
    InputSnapshot prevTickInput[kMaxPlayers] = {};
    for (uint32_t p = 0; p < kMaxPlayers; ++p) {
        r.Raw(&prevTickInput[p], sizeof(InputSnapshot));
    }
    const uint64_t audioHandleSeq = r.U64();
    std::vector<XpbdBackend::Pool> xpbdPools;
    if (!ReadXpbd(r, xpbdPools)) { // M60'b (v4)。refs.xpbd が無い構成では読み捨てる
        return false;
    }
    if (!r.Ok()) {
        MYE_LOG_ERROR("[snapshot] truncated blob");
        return false;
    }

    // ここまでで現世界には一切触れていない。World も全読み後の一括差し替え
    if (!refs.scene->GetWorld().SnapshotRead(r)) {
        return false;
    }

    refs.scene->Time() = scene.time;
    refs.scene->Persist().Entries() = std::move(scene.persist);
    refs.scene->SetNextFileId(scene.nextFileId);
    refs.scene->SetSourcePath(std::move(scene.sourcePath));
    refs.scene->ReplaceOverridesTable(std::move(scene.overrides));
    refs.scene->InvalidateFileIdCache(); // 派生物 (EntityID が総入れ替えされたので必ず)

    if (refs.particles != nullptr) {
        refs.particles->PoolsForSnapshot() = std::move(pools);
    }
    if (refs.xpbd != nullptr) {
        refs.xpbd->PoolsForSnapshot() = std::move(xpbdPools);
    }
    if (refs.collision != nullptr) {
        refs.collision->PrevPairsForSnapshot() = std::move(prevPairs);
        refs.collision->PrevSolidPairsForSnapshot() = std::move(prevSolidPairs);
    }
    if (refs.scripts != nullptr) {
        std::unordered_set<uint64_t>& dst = refs.scripts->StartedForSnapshot();
        dst.clear();
        dst.insert(started.begin(), started.end());
    }
    if (refs.prevTickInput != nullptr) {
        for (uint32_t p = 0; p < kMaxPlayers; ++p) {
            refs.prevTickInput[p] = prevTickInput[p];
        }
    }
    if (refs.audioHandleSeq != nullptr) {
        *refs.audioHandleSeq = audioHandleSeq;
    }
    if (refs.tickIndex != nullptr) {
        *refs.tickIndex = tick;
    }
    return true;
}

bool PeekSimSnapshotTick(const std::byte* data, size_t size, uint64_t& outTick)
{
    if (data == nullptr || size < kHeaderBytes) {
        return false;
    }
    ByteReader r(data, size);
    if (r.U32() != kMagic || r.U32() != kSimSnapshotVersion) {
        return false;
    }
    r.U32();
    r.U32();
    outTick = r.U64();
    return r.Ok();
}

} // namespace mye
