//====================================================================================
//                          ComputeStateSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          ForEach の State バッファ (スナップショット・ハッシュ・寿命) の回帰テスト
//====================================================================================
#include "Engine/Engine/Compute/ComputeStateSelfTest.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Util/ByteIo.h"
#include "Engine/Engine/Compute/ComputeBufferStore.h"
#include "Engine/Engine/Compute/ComputeService.h"
#include "Engine/Engine/Compute/KernelHash.h"
#include "Engine/Engine/Compute/KernelRange.h"
#include "Engine/Engine/Compute/KernelRunner.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Platform/PathUtil.h"
#include "Shared/KernelCompat.h"

// ComputeServiceSelfTest.cpp とは別の名前空間で同じカーネルを取り込む
namespace mye::kernel::cpu::StateSaxpy {
#include "../../../../assets/shaders/kernels/SelfTestSaxpy.kernel.hlsl"
}

namespace mye {
namespace {

using namespace compute;
namespace fs = std::filesystem;
namespace sx = kernel::cpu::StateSaxpy;

int g_fails = 0;

void Check(bool cond, const char* msg)
{
    if (cond) {
        MYE_LOG_INFO("  PASS: %s", msg);
    } else {
        MYE_LOG_ERROR("  FAIL: %s", msg);
        ++g_fails;
    }
}

constexpr uint32_t kCount = 257; // 8 の倍数にしない (端数のレンジも通す)
constexpr sx::SelfTestSaxpyParams kParams = { 0.75f };
constexpr const char* kScript = "StateScript";
constexpr const char* kSaxpy = "SelfTestSaxpy";

std::vector<float> MakeFloats(uint32_t n, uint32_t salt)
{
    std::vector<float> v(n);
    for (uint32_t i = 0; i < n; ++i) {
        v[i] = kernel::HashToUnitFloat(kernel::HashU32(i * 2654435761u + salt)) * 8.0f - 4.0f;
    }
    return v;
}

// カーネルと宣言を登録する。サービスごとに 1 回
bool RegisterSaxpy(ComputeService& svc)
{
    std::ifstream in(fs::path(FindEngineShaderDir()) / L"kernels" / L"SelfTestSaxpy.kernel.hlsl", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.empty()) {
        return false;
    }
    const uint64_t hash = kernel::HashKernelSource(text);
    ComputeKernelDesc desc;
    desc.name = kSaxpy;
    desc.paramsSize = sizeof(sx::SelfTestSaxpyParams);
    desc.args = { { "y", true, sizeof(float) }, { "x", false, sizeof(float) } };
    desc.cpp = { kernel::MakeRangeFn<&sx::SelfTestSaxpy_Cpu>(), hash };
    if (!svc.RegisterKernel(desc)) {
        return false;
    }
    svc.UpdateKernelSource("SelfTestSaxpy.kernel.hlsl", text);
    svc.ReplaceDeclarations({ { kScript, { kSaxpy } } });
    return true;
}

ForEachArgs SaxpyArgs(const uint64_t* bufs)
{
    ForEachArgs a;
    a.caller = { CallerKind::Cpp, kScript };
    a.kernel = kSaxpy;
    a.execution = Execution::Auto;
    a.params = &kParams;
    a.paramsSize = sizeof(kParams);
    a.buffers = bufs;
    a.bufferCount = 2;
    return a;
}

// 参照: 型つきの C++ 版を直接 1 回回す
std::vector<float> ReferenceStep(jobs::JobSystem& js, std::vector<float> y, const std::vector<float>& x)
{
    kernel::RunKernelCpu(js, &sx::SelfTestSaxpy_Cpu, kParams, kernel::RunOptions{},
                         kernel::RWView<float>(y.data(), static_cast<uint32_t>(y.size())),
                         kernel::ROView<float>(x.data(), static_cast<uint32_t>(x.size())));
    return y;
}

std::vector<float> ReadAll(ComputeService& svc, CallerKind lane, uint64_t h, uint32_t n)
{
    std::vector<float> v(n, -12345.0f);
    svc.ReadBuffer(lane, h, v.data(), n);
    return v;
}

std::vector<std::byte> Snapshot(const ComputeBufferStore& store)
{
    std::vector<std::byte> blob;
    ByteWriter w(blob);
    store.WriteSnapshot(w);
    return blob;
}

// ---------------------------------------------------------------------------
// ストア単体
// ---------------------------------------------------------------------------

void TestStoreSnapshot()
{
    MYE_LOG_INFO("-- ストアの保存/復元 --");
    const EntityID e1{ 3, 1 };
    const EntityID e2{ 9, 4 };
    ComputeBufferStore a(StoreLane::Game);
    const uint64_t s0 = a.Create(4, 4, BufferKind::State, e1);
    const uint64_t fx = a.Create(8, 4, BufferKind::Fx, e1);
    const uint64_t s2 = a.Create(3, 8, BufferKind::State, e2);
    const uint64_t s3 = a.Create(2, 4, BufferKind::State);
    const std::vector<float> f0 = MakeFloats(4, 11);
    const std::vector<float> f2 = MakeFloats(6, 12);
    a.Write(s0, f0.data(), 4);
    a.Write(s2, f2.data(), 3);
    a.Release(s3); // 世代が進んだ空きスロットを作る
    const std::vector<std::byte> blob = Snapshot(a);

    // 撮った後に変える: State を書き換え、解放し、Fx を足す
    const uint64_t lateFx = a.Create(5, 4, BufferKind::Fx);
    const std::vector<float> zero(4, 0.0f);
    a.Write(s0, zero.data(), 4);
    a.Release(s2);

    ByteReader r(blob.data(), blob.size());
    std::vector<ComputeBufferStore::Slot> slots;
    Check(a.ReadSnapshot(r, slots) && r.Ok() && r.Remaining() == 0, "撮った blob を読める (読み残しなし)");
    a.ApplySnapshot(std::move(slots));
    std::vector<float> back(4);
    Check(a.IsValid(s0) && a.Read(s0, back.data(), 4) && back == f0, "State の中身が戻る");
    std::vector<float> back2(6);
    const ComputeBufferStore::Slot* sl2 = a.Find(s2);
    Check(sl2 != nullptr && a.Read(s2, back2.data(), 3) && back2 == f2 && sl2->owner.index == e2.index
              && sl2->owner.generation == e2.generation && sl2->count == 3 && sl2->stride == 8,
          "解放していた State がハンドルごと戻り、持ち主・形も戻る");
    const ComputeBufferStore::Slot* sl0 = a.Find(s0);
    Check(sl0 != nullptr && sl0->owner.index == e1.index && sl0->owner.generation == e1.generation,
          "持ち主 (EntityID) が戻る");
    Check(a.IsValid(fx) && a.IsValid(lateFx), "(iii) 復元の前後で手元の Fx のハンドルはそのまま有効 (Fx の表には触らない)");
    Check(!a.IsValid(s3), "撮影時に解放済みだったものは空きのまま");
    Check(a.LiveCount() == 4 && a.StateBytes() == (4u * 4u + 3u * 8u), "生存数と State のバイト数が合う");

    // 同じ操作から同じハンドル: 撮影時点の続きと一致する
    ComputeBufferStore ref(StoreLane::Game);
    ref.Create(4, 4, BufferKind::State, e1);
    ref.Create(8, 4, BufferKind::Fx, e1);
    ref.Create(3, 8, BufferKind::State, e2);
    const uint64_t refS3 = ref.Create(2, 4, BufferKind::State);
    ref.Release(refS3);
    const uint64_t refNext = ref.Create(2, 4, BufferKind::State);
    const uint64_t gotNext = a.Create(2, 4, BufferKind::State);
    Check(gotNext == refNext && refNext != s3, "復元後に作ると、撮影時点の続きと同じハンドルが出る (世代も戻る)");

    // 別の (空の) ストアへ復元: State と採番は戻る。(i) 復元後の Create が連続実行と一致する
    ComputeBufferStore fresh(StoreLane::Game);
    ByteReader r2(blob.data(), blob.size());
    std::vector<ComputeBufferStore::Slot> slots2;
    Check(fresh.ReadSnapshot(r2, slots2), "空のストアでも読める");
    fresh.ApplySnapshot(std::move(slots2));
    Check(fresh.IsValid(s0) && fresh.IsValid(s2) && !fresh.IsValid(fx), "空のストアへは State だけが戻る (Fx は別物)");
    Check(fresh.Create(2, 4, BufferKind::State) == refNext, "(i)(v) 空のストアへ復元してから作ったハンドルが、連続実行と一致する");

    // (ii) 復元の後に Fx を作ってから State を作っても、連続実行と一致する (Fx の作り直しが State の採番を動かさない)
    ComputeBufferStore fresh2(StoreLane::Game);
    ByteReader r3(blob.data(), blob.size());
    std::vector<ComputeBufferStore::Slot> slots3;
    Check(fresh2.ReadSnapshot(r3, slots3), "(ii) 読める");
    fresh2.ApplySnapshot(std::move(slots3));
    fresh2.Create(8, 4, BufferKind::Fx);
    fresh2.Create(8, 4, BufferKind::Fx);
    Check(fresh2.Create(2, 4, BufferKind::State) == refNext, "(ii) 復元後に Fx を作っても、続く State のハンドルは連続実行と一致する");

    // (iv) State と Fx を交互に作成・解放しても、State のハンドル列は Fx の有無に依存しない
    ComputeBufferStore withFx(StoreLane::Game);
    ComputeBufferStore noFx(StoreLane::Game);
    bool sameSeq = true;
    uint64_t prevWith = 0;
    uint64_t prevNo = 0;
    for (int i = 0; i < 12; ++i) {
        const uint64_t f1 = withFx.Create(4, 4, BufferKind::Fx);
        const uint64_t hw = withFx.Create(2, 4, BufferKind::State);
        const uint64_t hn = noFx.Create(2, 4, BufferKind::State);
        sameSeq = sameSeq && hw == hn;
        if (i % 3 == 2) {
            withFx.Release(prevWith);
            noFx.Release(prevNo);
        }
        if (i % 2 == 1) {
            withFx.Release(f1);
        }
        prevWith = hw;
        prevNo = hn;
    }
    Check(sameSeq, "(iv) State と Fx を交互に作成・解放しても、State のハンドル列は Fx の有無に依存しない");
    Check(a.Find(fx) != nullptr && a.Find(fx)->kind == BufferKind::Fx && a.Find(s0)->kind == BufferKind::State
              && ((fx >> 32) & (1u << 29)) != 0 && ((s0 >> 32) & (1u << 29)) == 0,
          "ハンドルに種別の印 (世代欄の bit 29) が入る");

    // 壊れた blob
    bool allRejected = true;
    for (size_t cut : { size_t(0), size_t(3), size_t(9), blob.size() / 2, blob.size() - 1 }) {
        ByteReader rr(blob.data(), cut);
        std::vector<ComputeBufferStore::Slot> tmp;
        allRejected = allRejected && !fresh.ReadSnapshot(rr, tmp);
    }
    Check(allRejected, "途中で切れた blob は全部拒む");
    std::vector<std::byte> bad = blob;
    // 先頭 8 バイトはスロット数。巨大にして「残りバイト数に収まらない」を踏ませる
    bad[7] = std::byte{ 0x7F };
    ByteReader rb(bad.data(), bad.size());
    std::vector<ComputeBufferStore::Slot> tmp2;
    Check(!fresh.ReadSnapshot(rb, tmp2), "スロット数が残りに収まらない blob は拒む");
    std::vector<std::byte> badStride = blob;
    // 最初のスロット: [count(8)] [gen(4) mark(1) count(4) stride(4)...]。stride の下位バイトを 4 の倍数でなくする
    badStride[8 + 4 + 1 + 4] = std::byte{ 6 };
    ByteReader rs(badStride.data(), badStride.size());
    Check(!fresh.ReadSnapshot(rs, tmp2), "stride が 4 の倍数でない blob は拒む");
}

// ---------------------------------------------------------------------------
// ハッシュ
// ---------------------------------------------------------------------------

void TestHash()
{
    MYE_LOG_INFO("-- ワールドハッシュ --");
    Scene scene;
    World& w = scene.GetWorld();
    GameObject g = scene.CreateGameObjectTracked("Owner");
    w.ApplyStructuralChanges();
    ComputeBufferStore store(StoreLane::Game);

    SimSources none;
    none.time = &scene.Time();
    SimSources withStore = none;
    withStore.computeState = &store;
    const uint64_t baseHash = HashWorld(w, none);
    Check(HashWorld(w, withStore) == baseHash, "State が 1 本も無ければ、ストアを渡してもハッシュは動かない (内容ゲート)");

    const uint64_t s = store.Create(4, 4, BufferKind::State, g.Id());
    const std::vector<float> f = MakeFloats(4, 21);
    store.Write(s, f.data(), 4);
    const uint64_t h1 = HashWorld(w, withStore);
    Check(h1 != baseHash, "State があるとハッシュに入る");
    Check(HashWorld(w, withStore) == h1, "同じ状態なら同じハッシュ");

    std::vector<float> g2 = f;
    g2[3] += 1.0f;
    store.Write(s, g2.data(), 4);
    const uint64_t h2 = HashWorld(w, withStore);
    Check(h2 != h1, "中身が 1 要素変わると割れる");
    store.Write(s, f.data(), 4);
    Check(HashWorld(w, withStore) == h1, "戻すと元のハッシュに戻る");

    const uint64_t fxHandle = store.Create(4, 4, BufferKind::Fx);
    store.Write(fxHandle, g2.data(), 4);
    Check(HashWorld(w, withStore) == h1, "Fx はハッシュに入らない");

    ComputeBufferStore other(StoreLane::Game);
    const uint64_t s2 = other.Create(4, 4, BufferKind::State, EntityID{ 5, 5 });
    other.Write(s2, f.data(), 4);
    SimSources withOther = none;
    withOther.computeState = &other;
    Check(HashWorld(w, withOther) != h1, "持ち主だけが違っても割れる");

    HashDump dump;
    HashWorldDump(w, withStore, 7, dump);
    bool sawData = false;
    bool sawTotal = false;
    for (const std::string& line : dump.lines) {
        sawData = sawData || line.find("\tCompute\tdata\t") != std::string::npos;
        sawTotal = sawTotal || line.find("\tCompute\t#total\t") != std::string::npos;
    }
    Check(sawData && sawTotal && dump.total == h1, "--hash-dump に Compute の行が出て、total が HashWorld と一致する");
    std::vector<EntityHash> ents;
    uint64_t detailedTotal = 0;
    HashWorldDetailed(w, withStore, ents, detailedTotal);
    Check(detailedTotal == h1, "HashWorldDetailed も同じ total");
}

// State の総量ごとの、1 回のワールドハッシュにかかる時間 (ms)。1 tick (16.6ms) に毎 tick 1 回かかるコストの目安で、
// 既定の上限 (8MiB) が tick の 10% 以内かを人が読む用に記録する (比較のため 16MiB まで測る)。時間は合否に使わない (決定論の邪魔をしない)
void TestHashCost()
{
    MYE_LOG_INFO("-- State のハッシュのコスト --");
    constexpr uint32_t kBufferBytes = 1u << 20;
    constexpr uint32_t kStride = 16;
    constexpr uint32_t kRepeats = 8;
    for (const uint32_t megabytes : { 1u, 2u, 4u, 8u, 16u }) {
        Scene scene;
        World& w = scene.GetWorld();
        GameObject g = scene.CreateGameObjectTracked("Owner");
        w.ApplyStructuralChanges();
        ComputeStoreLimits limits;
        limits.stateTotalBytes = 16ull << 20; // 既定より大きい総量も測る
        ComputeBufferStore store(StoreLane::Game, limits);
        SimSources src;
        src.time = &scene.Time();
        src.computeState = &store;
        std::vector<uint8_t> fill(kBufferBytes);
        for (uint32_t i = 0; i < kBufferBytes; ++i) {
            fill[i] = static_cast<uint8_t>(kernel::HashU32(i + 7u) >> 24);
        }
        bool created = true;
        for (uint32_t b = 0; b < megabytes; ++b) {
            const uint64_t h = store.Create(kBufferBytes / kStride, kStride, BufferKind::State, g.Id());
            created = created && h != 0 && store.Write(h, fill.data(), kBufferBytes / kStride);
        }
        Check(created && store.StateBytes() == static_cast<uint64_t>(megabytes) * kBufferBytes, "State を総量ぶん作れる");
        const uint64_t first = HashWorld(w, src);
        bool stable = true;
        const auto t0 = std::chrono::steady_clock::now();
        for (uint32_t r = 0; r < kRepeats; ++r) {
            stable = stable && HashWorld(w, src) == first;
        }
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / kRepeats;
        MYE_LOG_INFO("    State %u MiB: ワールドハッシュ 1 回 %.3f ms (1 tick = 16.667 ms の %.1f%%)", megabytes, ms,
                     ms * 100.0 / (1000.0 / 60.0));
        Check(stable, "同じ状態のハッシュは繰り返しても同じ");
    }
}

// ---------------------------------------------------------------------------
// SimSnapshot の往復・寿命
// ---------------------------------------------------------------------------

void TestSimSnapshotAndLifetime()
{
    MYE_LOG_INFO("-- SimSnapshot の往復と寿命 --");
    jobs::JobSystem js;
    js.Init();
    {
        Scene scene;
        World& w = scene.GetWorld();
        GameObject e1 = scene.CreateGameObjectTracked("Owner1");
        GameObject e2 = scene.CreateGameObjectTracked("Owner2");
        w.ApplyStructuralChanges();

        ComputeService svc;
        svc.Bind(nullptr, nullptr, &js);
        Check(RegisterSaxpy(svc), "カーネルを登録できる");
        SimRefs refs;
        refs.scene = &scene;
        refs.compute = &svc;
        uint64_t tick = 100;
        refs.tickIndex = &tick;

        const uint32_t n = kCount;
        const std::vector<float> y0 = MakeFloats(n, 31);
        const std::vector<float> x0 = MakeFloats(n, 32);
        uint64_t buf[2];
        buf[0] = svc.CreateBuffer(CallerKind::Cpp, n, sizeof(float), BufferKind::State, e1.Id());
        buf[1] = svc.CreateBuffer(CallerKind::Cpp, n, sizeof(float), BufferKind::State, e1.Id());
        const uint64_t other = svc.CreateBuffer(CallerKind::Cpp, 5, sizeof(float), BufferKind::State, e2.Id());
        const uint64_t fx = svc.CreateBuffer(CallerKind::Cpp, 5, sizeof(float), BufferKind::Fx, e2.Id());
        const uint64_t scriptLane = svc.CreateBuffer(CallerKind::CSharp, 5, sizeof(float), BufferKind::State, e1.Id());
        svc.WriteBuffer(CallerKind::Cpp, buf[0], y0.data(), n);
        svc.WriteBuffer(CallerKind::Cpp, buf[1], x0.data(), n);
        Check(buf[0] != 0 && buf[1] != 0 && other != 0 && fx != 0 && scriptLane != 0, "State / Fx / C# 用のバッファを作れる");

        svc.BeginTick(false);
        const uint64_t job = svc.ForEach(SaxpyArgs(buf));
        const std::vector<float> y1 = ReferenceStep(js, y0, x0);
        Check(job == kJobDoneCpu && ReadAll(svc, CallerKind::Cpp, buf[0], n) == y1, "State の ForEach が CPU で同期に走る");

        const uint64_t hashA = HashWorld(w, refs.HashSources());
        std::vector<std::byte> blob;
        Check(CaptureSimSnapshot(refs, blob), "撮影できる");

        // 撮影後に変える (最初の操作は Create: 復元後の同じ操作と比べる)
        const uint64_t mutCreate = svc.CreateBuffer(CallerKind::Cpp, 3, sizeof(float), BufferKind::State, e2.Id());
        svc.BeginTick(false);
        svc.ForEach(SaxpyArgs(buf));
        svc.ReleaseBuffer(CallerKind::Cpp, other);
        const uint64_t lateFx = svc.CreateBuffer(CallerKind::Cpp, 5, sizeof(float), BufferKind::Fx);
        Check(HashWorld(w, refs.HashSources()) != hashA, "撮影後の変更でハッシュが動く");

        Check(RestoreSimSnapshot(refs, blob.data(), blob.size()), "復元できる");
        Check(HashWorld(w, refs.HashSources()) == hashA, "復元後のハッシュが撮影時と一致する");
        Check(ReadAll(svc, CallerKind::Cpp, buf[0], n) == y1 && svc.Store(CallerKind::Cpp).IsValid(other),
              "FIELD に持っていたハンドルが、復元後も同じバッファ (同じ中身) を指す");
        Check(svc.Store(CallerKind::Cpp).IsValid(fx) && svc.Store(CallerKind::Cpp).IsValid(lateFx),
              "(iii) 復元の前後で、撮影前後どちらの Fx も手元のハンドルのまま有効 (Fx の表には触らない)");
        Check(svc.Store(CallerKind::CSharp).IsValid(scriptLane), "C# 用のストアは復元に触られない");
        const uint64_t again = svc.CreateBuffer(CallerKind::Cpp, 3, sizeof(float), BufferKind::State, e2.Id());
        Check(again == mutCreate, "復元後に同じ操作をすると同じハンドルが出る (連続して走った場合と同じ)");
        svc.ReleaseBuffer(CallerKind::Cpp, again);
        RestoreSimSnapshot(refs, blob.data(), blob.size());
        // (ii) 復元後にスクリプトが Fx を作り直しても、続く State の採番は連続実行と変わらない
        const uint64_t rebuiltFx = svc.CreateBuffer(CallerKind::Cpp, 5, sizeof(float), BufferKind::Fx);
        const uint64_t afterFx = svc.CreateBuffer(CallerKind::Cpp, 3, sizeof(float), BufferKind::State, e2.Id());
        Check(afterFx == mutCreate, "(ii) 復元後に Fx を作ってから State を作っても、連続実行と同じハンドルが出る");
        svc.ReleaseBuffer(CallerKind::Cpp, afterFx);
        svc.ReleaseBuffer(CallerKind::Cpp, rebuiltFx);

        // 復元後の ForEach が、連続して走った場合と同じ結果
        svc.BeginTick(false);
        const bool ran = svc.ForEach(SaxpyArgs(buf)) == kJobDoneCpu;
        Check(ran && ReadAll(svc, CallerKind::Cpp, buf[0], n) == ReferenceStep(js, y1, x0), "復元後の続きの ForEach が同じ結果になる");
        RestoreSimSnapshot(refs, blob.data(), blob.size());

        // 壊れた blob: ストアに手が付かない
        std::vector<std::byte> truncated(blob.begin(), blob.begin() + static_cast<std::ptrdiff_t>(blob.size() - 40));
        Check(!RestoreSimSnapshot(refs, truncated.data(), truncated.size())
                  && HashWorld(w, refs.HashSources()) == hashA,
              "壊れた blob は拒み、State を書き換えない");

        // compute を持たない構成へも戻せる (節は読み捨て)
        SimRefs bare = refs;
        bare.compute = nullptr;
        Check(RestoreSimSnapshot(bare, blob.data(), blob.size()), "compute の無い構成でも blob を読める");
        RestoreSimSnapshot(refs, blob.data(), blob.size());

        // 所有エンティティの破棄 → tick 末に解放
        w.DestroyEntity(e1.Id());
        w.ApplyStructuralChanges();
        const uint32_t released = svc.ReleaseDeadOwners(w);
        Check(released == 3 && !svc.Store(CallerKind::Cpp).IsValid(buf[0]) && !svc.Store(CallerKind::Cpp).IsValid(buf[1])
                  && !svc.Store(CallerKind::CSharp).IsValid(scriptLane),
              "持ち主が破棄されると、そのエンティティの State が (C# 用も) 解放される");
        Check(svc.Store(CallerKind::Cpp).IsValid(other) && svc.Store(CallerKind::Cpp).IsValid(fx),
              "別のエンティティのバッファは残る");
        Check(svc.ReleaseDeadOwners(w) == 0, "もう一度呼んでも何も起きない");
        w.DestroyEntity(e2.Id());
        w.ApplyStructuralChanges();
        Check(svc.ReleaseDeadOwners(w) == 2 && svc.Store(CallerKind::Cpp).LiveCount() == 1
                  && svc.Store(CallerKind::Cpp).IsValid(lateFx),
              "残りの持ち主を破棄すると持ち主付きは全部解放される (Fx も。持ち主なしの Fx は残る)");

        // 持ち主なしは自動解放しない / 全解放で採番が新規起動と同じに戻る
        const uint64_t unowned = svc.CreateBuffer(CallerKind::Cpp, 2, sizeof(float), BufferKind::State);
        Check(svc.ReleaseDeadOwners(w) == 0 && svc.Store(CallerKind::Cpp).IsValid(unowned), "持ち主の無いバッファは自動解放しない");
        svc.ReleaseAllBuffers();
        ComputeService freshSvc;
        const uint64_t freshFirst = freshSvc.CreateBuffer(CallerKind::Cpp, 2, sizeof(float), BufferKind::State);
        const uint64_t afterAll = svc.CreateBuffer(CallerKind::Cpp, 2, sizeof(float), BufferKind::State);
        Check(svc.Store(CallerKind::Cpp).LiveCount() == 1 && afterAll == freshFirst,
              "全解放 (Play 停止・シーン読込) の後の採番は、新規起動と同じ ハンドルから始まる");
    }
    js.Shutdown();
}

} // namespace

bool RunComputeStateSelfTest()
{
    MYE_LOG_INFO("==== Compute state (M91d) self test ====");
    g_fails = 0;
    TestStoreSnapshot();
    TestHash();
    TestHashCost();
    TestSimSnapshotAndLifetime();
    MYE_LOG_INFO("==== Compute state self test: %s ====", g_fails == 0 ? "PASS" : "FAIL");
    return g_fails == 0;
}

bool CheckComputeStateRunsWithoutDevice(ComputeService& service)
{
    g_fails = 0;
    jobs::JobSystem& js = jobs::System();
    if (!RegisterSaxpy(service)) {
        MYE_LOG_ERROR("  FAIL: the saxpy kernel cannot be registered (run from the repository root)");
        return false;
    }
    const uint32_t n = kCount;
    const std::vector<float> y0 = MakeFloats(n, 41);
    const std::vector<float> x0 = MakeFloats(n, 42);
    uint64_t buf[2];
    buf[0] = service.CreateBuffer(CallerKind::Cpp, n, sizeof(float), BufferKind::State);
    buf[1] = service.CreateBuffer(CallerKind::Cpp, n, sizeof(float), BufferKind::State);
    service.WriteBuffer(CallerKind::Cpp, buf[0], y0.data(), n);
    service.WriteBuffer(CallerKind::Cpp, buf[1], x0.data(), n);
    service.BeginTick(false);
    const uint64_t job = service.ForEach(SaxpyArgs(buf));
    Check(job == kJobDoneCpu && service.Target(job) == JobTarget::Cpu, "デバイス無しでも State の ForEach が CPU で完了する");
    Check(ReadAll(service, CallerKind::Cpp, buf[0], n) == ReferenceStep(js, y0, x0), "結果が EngineLoop 側と同じ参照実行とビット一致する");
    service.ReleaseBuffer(CallerKind::Cpp, buf[0]);
    service.ReleaseBuffer(CallerKind::Cpp, buf[1]);
    return g_fails == 0;
}

} // namespace mye
