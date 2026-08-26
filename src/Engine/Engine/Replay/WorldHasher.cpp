#include "Engine/Engine/Replay/WorldHasher.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>

#include "Engine/Core/Components.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/GameFlow.h"
#include "Engine/Engine/Particles/CpuParticleBackend.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace {

constexpr char kHexDigits[] = "0123456789ABCDEF";

void AppendHexBytes(std::string& s, const void* data, size_t size)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        s.push_back(kHexDigits[p[i] >> 4]);
        s.push_back(kHexDigits[p[i] & 0xF]);
    }
}

// 16 桁固定 (上位から)。畳み込み hash と「u64 として畳まれた値」の両方に使う
void AppendHexU64(std::string& s, uint64_t v)
{
    for (int i = 15; i >= 0; --i) {
        s.push_back(kHexDigits[(v >> (i * 4)) & 0xF]);
    }
}

void AppendDecU64(std::string& s, uint64_t v)
{
    char buf[24];
    int n = 0;
    do {
        buf[n++] = static_cast<char>('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    while (n > 0) {
        s.push_back(buf[--n]);
    }
}

// 名前は NameComponent の生バイト = タブや制御文字が入りうる。
// 列区切りを壊すと差分ツールが構造差と誤認するので、ここで潰す
void AppendSanitized(std::string& s, const char* text)
{
    if (!text || *text == '\0') {
        s.push_back('-');
        return;
    }
    for (const char* p = text; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        s.push_back(c < 0x20 || c == 0x7F ? '?' : *p);
    }
}

// ダンプの出力先。null (= HashWorld / HashWorldDetailed の経路) なら
// 走査中に 1 バイトも組み立てない
struct DumpCtx {
    std::vector<std::string>* lines = nullptr;
    uint64_t tick = 0;
    std::string entityCol = "-"; // 現在走査中のエンティティ ("index:generation")
    std::string nameCol = "-";
};

// 1 行 = ハッシュに畳み込まれた 1 単位。value は呼び出し側が組んだ hex 文字列
void Emit(DumpCtx* d, const char* component, const char* field, const std::string& value,
          uint64_t fold)
{
    if (!d || !d->lines) {
        return;
    }
    std::string s;
    s.reserve(64 + value.size());
    AppendDecU64(s, d->tick);
    s.push_back('\t');
    s += d->entityCol;
    s.push_back('\t');
    s += d->nameCol;
    s.push_back('\t');
    s += component;
    s.push_back('\t');
    s += field;
    s.push_back('\t');
    s += value;
    s.push_back('\t');
    AppendHexU64(s, fold);
    d->lines->push_back(std::move(s));
}

// u64 として畳み込まれた値の行 (HashCombine 相当)
void EmitU64(DumpCtx* d, const char* component, const char* field, uint64_t value, uint64_t fold)
{
    if (!d || !d->lines) {
        return;
    }
    std::string v;
    v.reserve(16);
    AppendHexU64(v, value);
    Emit(d, component, field, v, fold);
}

// 生バイト列として畳み込まれた値の行 (HashBytes 相当)
void EmitBytes(DumpCtx* d, const char* component, const char* field, const void* data, size_t size,
               uint64_t fold)
{
    if (!d || !d->lines) {
        return;
    }
    std::string v;
    v.reserve(size * 2);
    AppendHexBytes(v, data, size);
    Emit(d, component, field, v, fold);
}

// パーティクルの SoA 配列は要素数が数千になりうる。生 hex を出すとダンプが破裂するので
// 「要素数 + 配列単体のサブハッシュ」に畳んで 1 行にする (割れたエミッタと配列までは特定できる)
void EmitArray(DumpCtx* d, const char* field, const float* data, uint32_t count, uint64_t fold)
{
    if (!d || !d->lines) {
        return;
    }
    std::string v;
    v.reserve(32);
    v += "n=";
    AppendDecU64(v, count);
    v.push_back('#');
    AppendHexU64(v, HashBytes(data, count * sizeof(float)));
    Emit(d, "Particles", field, v, fold);
}

// 1 エンティティ分: 親リンク + シリアライズ対象コンポーネントの登録フィールド
uint64_t HashEntity(World& world, EntityID e, DumpCtx* d)
{
    const ComponentRegistry& reg = ComponentRegistry::Get();
    uint64_t h = kFnvOffset;
    h = HashCombine(h, e.index);
    EmitU64(d, "-", "index", e.index, h);
    h = HashCombine(h, e.generation);
    EmitU64(d, "-", "generation", e.generation, h);

    const EntityID parent = world.GetParent(e);
    h = HashCombine(h, parent.index);
    EmitU64(d, "-", "parentIndex", parent.index, h);
    h = HashCombine(h, parent.generation);
    EmitU64(d, "-", "parentGeneration", parent.generation, h);

    const Archetype* arch = world.GetArchetype(e);
    if (!arch) {
        return h;
    }
    for (ComponentTypeId t : arch->Types()) { // TypeId 昇順 = 決定論
        const ComponentDesc& desc = reg.Desc(t);
        if (desc.flags & (kComponentNoSerialize | kComponentNoHash)) {
            continue; // WorldMatrix (派生値) / Hierarchy / FileId / C# スクリプト状態 (別レーン)
        }
        const void* comp = world.GetComponentRaw(e, t);
        if (!comp) {
            continue;
        }
        h = HashCombine(h, desc.nameHash);
        EmitU64(d, desc.name, "#nameHash", desc.nameHash, h);
        for (const FieldDesc& f : desc.fields) {
            if (f.flags & kFieldNoSerialize) {
                continue;
            }
            // ビットパターンをそのままハッシュ (float 演算で比較しない — spec 11.3)
            const uint8_t* bytes = static_cast<const uint8_t*>(comp) + f.offset;
            const uint32_t size = FieldTypeSize(f.type);
            h = HashBytes(bytes, size, h);
            EmitBytes(d, desc.name, f.name, bytes, size, h);
        }
    }
    return h;
}

uint64_t HashCpuParticles(const CpuParticleBackend& cpu, DumpCtx* d)
{
    uint64_t h = kFnvOffset;
    for (const CpuParticleBackend::EmitterPool& pool : cpu.Pools()) { // owner.index 昇順
        if (d && d->lines) {
            d->entityCol.clear();
            AppendDecU64(d->entityCol, pool.owner.index);
            d->entityCol.push_back(':');
            AppendDecU64(d->entityCol, pool.owner.generation);
        }
        h = HashCombine(h, pool.owner.index);
        EmitU64(d, "Particles", "owner.index", pool.owner.index, h);
        h = HashCombine(h, pool.owner.generation);
        EmitU64(d, "Particles", "owner.generation", pool.owner.generation, h);
        h = HashCombine(h, pool.alive);
        EmitU64(d, "Particles", "alive", pool.alive, h);
        h = HashBytes(&pool.emitAccum, sizeof(pool.emitAccum), h);
        EmitBytes(d, "Particles", "emitAccum", &pool.emitAccum, sizeof(pool.emitAccum), h);
        h = HashCombine(h, static_cast<uint32_t>(pool.ageTicks)); // M32a: 放出ウィンドウ状態
        EmitU64(d, "Particles", "ageTicks", static_cast<uint32_t>(pool.ageTicks), h);
        const uint64_t rngState = pool.rng.State();
        const uint64_t rngInc = pool.rng.Inc();
        h = HashCombine(h, rngState);
        EmitU64(d, "Particles", "rngState", rngState, h);
        h = HashCombine(h, rngInc);
        EmitU64(d, "Particles", "rngInc", rngInc, h);
        const uint32_t n = pool.alive;
        if (n > 0) {
            h = HashBytes(pool.px.data(), n * sizeof(float), h);
            EmitArray(d, "px", pool.px.data(), n, h);
            h = HashBytes(pool.py.data(), n * sizeof(float), h);
            EmitArray(d, "py", pool.py.data(), n, h);
            h = HashBytes(pool.pz.data(), n * sizeof(float), h);
            EmitArray(d, "pz", pool.pz.data(), n, h);
            h = HashBytes(pool.vx.data(), n * sizeof(float), h);
            EmitArray(d, "vx", pool.vx.data(), n, h);
            h = HashBytes(pool.vy.data(), n * sizeof(float), h);
            EmitArray(d, "vy", pool.vy.data(), n, h);
            h = HashBytes(pool.vz.data(), n * sizeof(float), h);
            EmitArray(d, "vz", pool.vz.data(), n, h);
            h = HashBytes(pool.life.data(), n * sizeof(float), h);
            EmitArray(d, "life", pool.life.data(), n, h);
            h = HashBytes(pool.invLife.data(), n * sizeof(float), h);
            EmitArray(d, "invLife", pool.invLife.data(), n, h);
            h = HashBytes(pool.size0.data(), n * sizeof(float), h);
            EmitArray(d, "size0", pool.size0.data(), n, h);
        }
    }
    if (d && d->lines) {
        d->entityCol = "-";
    }
    return h;
}

// M51g: ゲームフロー状態 (決定台帳 5)。RNG の直後・パーティクルの前に畳み込む。
// PersistStore は std::map = キー昇順走査 (挿入順に依存しない — selftest が固定)
uint64_t HashGameFlow(uint64_t h, const TimeControl* time, const PersistStore* persist, DumpCtx* d)
{
    if (time) {
        h = HashCombine(h, time->paused ? 1u : 0u);
        EmitU64(d, "TimeControl", "paused", time->paused ? 1u : 0u, h);
        h = HashCombine(h, static_cast<uint32_t>(time->scalePercent));
        EmitU64(d, "TimeControl", "scalePercent", static_cast<uint32_t>(time->scalePercent), h);
        h = HashCombine(h, static_cast<uint32_t>(time->accum));
        EmitU64(d, "TimeControl", "accum", static_cast<uint32_t>(time->accum), h);
    }
    if (persist) {
        for (const auto& [key, blob] : persist->Entries()) {
            if (d && d->lines) { // キーは名前ハッシュなので名前列に出しておく
                d->nameCol.clear();
                AppendHexU64(d->nameCol, key);
            }
            h = HashCombine(h, key);
            EmitU64(d, "Persist", "key", key, h);
            h = HashCombine(h, static_cast<uint64_t>(blob.size()));
            EmitU64(d, "Persist", "size", static_cast<uint64_t>(blob.size()), h);
            if (!blob.empty()) {
                h = HashBytes(blob.data(), blob.size(), h);
                EmitBytes(d, "Persist", "blob", blob.data(), blob.size(), h);
            }
        }
        if (d && d->lines) {
            d->nameCol = "-";
        }
    }
    return h;
}

void CollectEntitiesSorted(World& world, std::vector<EntityID>& out)
{
    out.clear();
    const ComponentTypeId req[] = { NameComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            out.push_back(arch.EntityAt(row));
        }
    });
    std::sort(out.begin(), out.end(),
              [](EntityID a, EntityID b) { return a.index < b.index; }); // 明示キー (規則 7)
}

// 3 出口 (HashWorld / HashWorldDetailed / HashWorldDump) の唯一の実装。
// outEntities / dump は要らない出口では null — その場合は分岐 1 回分しか払わない
uint64_t HashWorldImpl(World& world, const SimSources& src,
                       std::vector<EntityHash>* outEntities, DumpCtx* d)
{
    std::vector<EntityID> entities;
    CollectEntitiesSorted(world, entities);

    if (outEntities) {
        outEntities->clear();
        outEntities->reserve(entities.size());
    }
    uint64_t total = kFnvOffset;
    for (EntityID e : entities) {
        if (d && d->lines) {
            d->entityCol.clear();
            AppendDecU64(d->entityCol, e.index);
            d->entityCol.push_back(':');
            AppendDecU64(d->entityCol, e.generation);
            d->nameCol.clear();
            AppendSanitized(d->nameCol, world.GetName(e));
        }
        const uint64_t eh = HashEntity(world, e, d);
        if (outEntities) {
            outEntities->push_back({ e, eh });
        }
        total = HashCombine(total, eh);
        EmitU64(d, "-", "#entity", eh, total);
    }
    if (d && d->lines) {
        d->entityCol = "-";
        d->nameCol = "-";
    }
    // ワールド RNG ストリーム
    total = HashCombine(total, world.Rng().State());
    EmitU64(d, "World", "rngState", world.Rng().State(), total);
    total = HashCombine(total, world.Rng().Inc());
    EmitU64(d, "World", "rngInc", world.Rng().Inc(), total);
    // ゲームフロー状態 (M51g: RNG の直後)
    total = HashGameFlow(total, src.time, src.persist, d);
    // CPU パーティクル (spec 11.3: ハッシュ対象)
    if (src.particles) {
        const uint64_t ph = HashCpuParticles(*src.particles, d);
        total = HashCombine(total, ph);
        EmitU64(d, "Particles", "#total", ph, total);
    }
    return total;
}

// ---- ダンプ差分 ----

// 7 列に分解する。区切りが足りない行は不正 (先頭の 5 列 = キー)
struct DumpCols {
    std::string_view col[7];
    bool ok = false;
};

DumpCols SplitCols(const std::string& line)
{
    DumpCols c;
    size_t start = 0;
    for (int i = 0; i < 7; ++i) {
        if (i == 6) {
            c.col[i] = std::string_view(line).substr(start);
            c.ok = true;
            break;
        }
        const size_t tab = line.find('\t', start);
        if (tab == std::string::npos) {
            return c;
        }
        c.col[i] = std::string_view(line).substr(start, tab - start);
        start = tab + 1;
    }
    return c;
}

// キー = tick 以外の 4 列 (entity / 名前 / コンポーネント / フィールド)。
// tick を外すのは「同じ状態を別 tick で撮ったダンプ」も突き合わせられるようにするため
bool SameKey(const DumpCols& a, const DumpCols& b)
{
    return a.col[1] == b.col[1] && a.col[2] == b.col[2] && a.col[3] == b.col[3]
           && a.col[4] == b.col[4];
}

// ログ 1 行は 240 バイトで切られる。値 hex は String256 で 512 文字になりうるので縮める
std::string Shorten(std::string_view v, size_t maxLen = 160)
{
    if (v.size() <= maxLen) {
        return std::string(v);
    }
    std::string s(v.substr(0, maxLen));
    s += "...(";
    AppendDecU64(s, v.size());
    s += " chars)";
    return s;
}

} // namespace

void HashWorldDetailed(World& world, const SimSources& src, std::vector<EntityHash>& outEntities,
                       uint64_t& outTotal)
{
    outTotal = HashWorldImpl(world, src, &outEntities, nullptr);
}

uint64_t HashWorld(World& world, const SimSources& src)
{
    return HashWorldImpl(world, src, nullptr, nullptr);
}

void HashWorldDump(World& world, const SimSources& src, uint64_t tick, HashDump& out)
{
    out.tick = tick;
    out.lines.clear();
    DumpCtx d;
    d.lines = &out.lines;
    d.tick = tick;
    out.total = HashWorldImpl(world, src, nullptr, &d);
}

bool WriteHashDump(const std::wstring& path, const HashDump& dump)
{
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path(), ec);
    }
    std::ofstream f(p, std::ios::binary);
    if (!f) {
        MYE_LOG_ERROR("[hashdump] cannot write %s", WideToUtf8(path).c_str());
        return false;
    }
    // ヘッダは '#' 始まり。差分は本文行だけを突き合わせる
    f << "#mye-hash-dump v1\n";
    f << "#tick " << dump.tick << "\n";
    std::string totalHex;
    AppendHexU64(totalHex, dump.total);
    f << "#total " << totalHex << "\n";
    f << "#columns tick\tentity\tname\tcomponent\tfield\tvalue\tfold\n";
    for (const std::string& line : dump.lines) {
        f << line << "\n";
    }
    MYE_LOG_INFO("[hashdump] tick %llu: %zu line(s), total %s -> %s",
                 static_cast<unsigned long long>(dump.tick), dump.lines.size(), totalHex.c_str(),
                 WideToUtf8(path).c_str());
    return true;
}

bool ReadHashDump(const std::wstring& path, HashDump& out)
{
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) {
        MYE_LOG_ERROR("[hashdump] cannot open %s", WideToUtf8(path).c_str());
        return false;
    }
    out = {};
    bool sawMagic = false;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        if (line[0] == '#') {
            if (line.rfind("#mye-hash-dump", 0) == 0) {
                sawMagic = true;
            } else if (line.rfind("#tick ", 0) == 0) {
                out.tick = std::strtoull(line.c_str() + 6, nullptr, 10);
            } else if (line.rfind("#total ", 0) == 0) {
                out.total = std::strtoull(line.c_str() + 7, nullptr, 16);
            }
            continue;
        }
        out.lines.push_back(line);
    }
    if (!sawMagic) {
        MYE_LOG_ERROR("[hashdump] not a hash dump: %s", WideToUtf8(path).c_str());
        return false;
    }
    return true;
}

HashDumpDiff DiffHashDumps(const HashDump& a, const HashDump& b, int maxReport)
{
    HashDumpDiff r;
    r.totalDiffers = (a.total != b.total);
    if (a.tick != b.tick) {
        MYE_LOG_WARN("[hashdiff] the two dumps are from different ticks (A=%llu B=%llu)",
                     static_cast<unsigned long long>(a.tick),
                     static_cast<unsigned long long>(b.tick));
    }

    const size_t n = (std::min)(a.lines.size(), b.lines.size());
    int reported = 0;
    for (size_t i = 0; i < n; ++i) {
        const DumpCols ca = SplitCols(a.lines[i]);
        const DumpCols cb = SplitCols(b.lines[i]);
        if (!ca.ok || !cb.ok) {
            r.structureDiffers = true;
            MYE_LOG_ERROR("[hashdiff] line %zu: malformed (expected 7 tab-separated columns)",
                          i + 1);
            break;
        }
        if (!SameKey(ca, cb)) {
            // ここから先は行がずれる = 突き合わせ不能。構造差として打ち切る
            r.structureDiffers = true;
            MYE_LOG_ERROR("[hashdiff] line %zu: structure differs", i + 1);
            MYE_LOG_ERROR("[hashdiff]   A: %s %s %s.%s", std::string(ca.col[1]).c_str(),
                          std::string(ca.col[2]).c_str(), std::string(ca.col[3]).c_str(),
                          std::string(ca.col[4]).c_str());
            MYE_LOG_ERROR("[hashdiff]   B: %s %s %s.%s", std::string(cb.col[1]).c_str(),
                          std::string(cb.col[2]).c_str(), std::string(cb.col[3]).c_str(),
                          std::string(cb.col[4]).c_str());
            break;
        }
        if (ca.col[6] != cb.col[6] && r.firstFoldLine == static_cast<size_t>(-1)) {
            r.firstFoldLine = i;
        }
        if (ca.col[5] != cb.col[5]) {
            // '#' 始まりのフィールドは同じダンプ内の他の行から決まるまとめ値
            if (!ca.col[4].empty() && ca.col[4].front() == '#') {
                ++r.rollupDiffs;
                continue;
            }
            ++r.valueDiffs;
            if (reported < maxReport) {
                ++reported;
                MYE_LOG_ERROR("[hashdiff] line %zu: %s \"%s\" %s.%s", i + 1,
                              std::string(ca.col[1]).c_str(), std::string(ca.col[2]).c_str(),
                              std::string(ca.col[3]).c_str(), std::string(ca.col[4]).c_str());
                MYE_LOG_ERROR("[hashdiff]   A = %s", Shorten(ca.col[5]).c_str());
                MYE_LOG_ERROR("[hashdiff]   B = %s", Shorten(cb.col[5]).c_str());
            }
        }
    }
    if (a.lines.size() != b.lines.size()) {
        r.structureDiffers = true;
        MYE_LOG_ERROR("[hashdiff] line count differs: A=%zu B=%zu", a.lines.size(),
                      b.lines.size());
    }
    const uint64_t reportCap = maxReport > 0 ? static_cast<uint64_t>(maxReport) : 0;
    if (r.valueDiffs > reportCap) {
        MYE_LOG_ERROR("[hashdiff] ... and %llu more differing field(s)",
                      static_cast<unsigned long long>(r.valueDiffs - reportCap));
    }
    if (r.firstFoldLine != static_cast<size_t>(-1)) {
        const DumpCols c = SplitCols(a.lines[r.firstFoldLine]);
        MYE_LOG_ERROR("[hashdiff] divergence starts at line %zu: %s \"%s\" %s.%s",
                      r.firstFoldLine + 1, std::string(c.col[1]).c_str(),
                      std::string(c.col[2]).c_str(), std::string(c.col[3]).c_str(),
                      std::string(c.col[4]).c_str());
    }
    if (r.rollupDiffs > 0 && r.valueDiffs == 0 && !r.structureDiffers) {
        // まとめ値だけが割れている = ダンプがハッシュ対象のどれかを行にしていない。
        // 診断そのもののバグなので目立たせる (WorldHasherSelfTest が本来ここを塞いでいる)
        MYE_LOG_ERROR("[hashdiff] %llu rollup row(s) differ but no leaf field does -"
                      " the dump is not covering everything the hash reads",
                      static_cast<unsigned long long>(r.rollupDiffs));
    }
    if (r.Same()) {
        MYE_LOG_INFO("[hashdiff] identical (%zu lines, total %016llX)", a.lines.size(),
                     static_cast<unsigned long long>(a.total));
    } else {
        MYE_LOG_ERROR("[hashdiff] %llu field(s) differ / total A=%016llX B=%016llX",
                      static_cast<unsigned long long>(r.valueDiffs),
                      static_cast<unsigned long long>(a.total),
                      static_cast<unsigned long long>(b.total));
    }
    return r;
}

} // namespace mye
