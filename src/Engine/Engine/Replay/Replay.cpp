#include "Engine/Engine/Replay/Replay.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace {
constexpr uint32_t kReplayMagic = 0x5045524Du; // 'MREP'
} // namespace

void ReplayRecorder::Start(const std::wstring& path, uint64_t rngState, uint64_t rngInc,
                           uint32_t entityCount, uint32_t playerCount, const std::byte* snapshot,
                           size_t snapshotSize, const SessionConfig& session,
                           const SimProvenance& provenance, const SnapshotMeta& startMeta)
{
    path_ = path;
    header_ = {};
    header_.rngState = rngState;
    header_.rngInc = rngInc;
    header_.entityCount = entityCount;
    header_.playerCount = (playerCount == 0) ? 1u : playerCount;
    header_.session = session;
    header_.provenance = provenance;
    header_.startMeta = startMeta;
    header_.flags = RoleHasSystemInput(session.role) ? kReplayFlagSystemInput : 0u;
    snapshot_.clear();
    if (snapshot != nullptr && snapshotSize > 0) {
        snapshot_.assign(snapshot, snapshot + snapshotSize);
    }
    header_.snapshotSize = snapshot_.size();
    inputs_.clear();
    systemInputs_.clear();
    hashes_.clear();
    active_ = true;
    MYE_LOG_INFO("[replay] recording to %s (players %u, snapshot %zu bytes, system input %s)",
                 WideToUtf8(path).c_str(), header_.playerCount, snapshot_.size(),
                 header_.flags != 0 ? "yes" : "no");
}

void ReplayRecorder::RecordTick(const InputSnapshot* lanes, uint32_t playerCount,
                                uint64_t worldHash, const SystemInputTick* systemInput)
{
    // 宣言と実際が食い違ったら**宣言側に合わせて**書く (足りない分はゼロ値)。
    // ここで黙って可変長にすると、ファイルの tick レコード長が tick ごとに変わって
    // 再生側が一切読めなくなる
    for (uint32_t p = 0; p < header_.playerCount; ++p) {
        inputs_.push_back((lanes != nullptr && p < playerCount) ? lanes[p] : InputSnapshot{});
    }
    if ((header_.flags & kReplayFlagSystemInput) != 0) {
        systemInputs_.push_back(NormalizeSystemInput(systemInput != nullptr ? *systemInput
                                                                          : SystemInputTick{}));
    }
    hashes_.push_back(worldHash);
}

bool ReplayRecorder::Finish()
{
    if (!active_) {
        return false;
    }
    active_ = false;
    header_.tickCount = hashes_.size();

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path_).parent_path(), ec);
    std::ofstream f(std::filesystem::path(path_), std::ios::binary);
    if (!f) {
        MYE_LOG_ERROR("[replay] cannot write %s", WideToUtf8(path_).c_str());
        return false;
    }
    f.write(reinterpret_cast<const char*>(&header_), sizeof(header_));
    if (!snapshot_.empty()) {
        f.write(reinterpret_cast<const char*>(snapshot_.data()),
                static_cast<std::streamsize>(snapshot_.size()));
    }
    // tick レコード = InputSnapshot × playerCount + (flags.bit0 のとき SystemInputTick) + uint64 hash。
    // 入力列とハッシュ列を別々に持っているので、書くときに tick 単位で綴じ直す
    const size_t perTick = header_.playerCount;
    const bool hasSystemInput = (header_.flags & kReplayFlagSystemInput) != 0;
    for (size_t t = 0; t < hashes_.size(); ++t) {
        f.write(reinterpret_cast<const char*>(&inputs_[t * perTick]),
                static_cast<std::streamsize>(perTick * sizeof(InputSnapshot)));
        if (hasSystemInput) {
            f.write(reinterpret_cast<const char*>(&systemInputs_[t]), sizeof(SystemInputTick));
        }
        f.write(reinterpret_cast<const char*>(&hashes_[t]), sizeof(uint64_t));
    }
    MYE_LOG_INFO("[replay] recorded %llu ticks -> %s",
                 static_cast<unsigned long long>(hashes_.size()), WideToUtf8(path_).c_str());
    return true;
}

bool ReplayPlayer::Load(const std::wstring& path)
{
    active_ = false;
    std::error_code ec;
    const uint64_t fileSize = std::filesystem::file_size(std::filesystem::path(path), ec);
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f || ec) {
        MYE_LOG_ERROR("[replay] cannot open %s", WideToUtf8(path).c_str());
        return false;
    }
    // ★ヘッダも中身もまず手元の変数へ読み、全部そろってから差し替える (途中で失敗しても前の内容を壊さない)
    // v8 と v9 は先頭 56 バイトが同じ並び。まず共通部を読んで版を決め、v9 なら残りを読む。
    // v8 の新項目 (flags / SessionConfig / SimProvenance / SnapshotMeta) は「不明」= 0 のまま
    MyeReplayHeaderV8 base = {};
    f.read(reinterpret_cast<char*>(&base), sizeof(base));
    if (!f || base.magic != kReplayMagic) {
        MYE_LOG_ERROR("[replay] bad file magic");
        return false;
    }
    if (base.version < kReplayOldestReadableVersion || base.version > kReplayFileVersion
        || base.inputSize != sizeof(InputSnapshot)) {
        MYE_LOG_ERROR("[replay] incompatible version/layout (v%u, input %u bytes)", base.version,
                      base.inputSize);
        return false;
    }
    MyeReplayHeader header;
    header.magic = base.magic;
    header.version = base.version;
    header.fixedDt = base.fixedDt;
    header.inputSize = base.inputSize;
    header.tickCount = base.tickCount;
    header.rngState = base.rngState;
    header.rngInc = base.rngInc;
    header.entityCount = base.entityCount;
    header.playerCount = base.playerCount;
    header.snapshotSize = base.snapshotSize;
    size_t headerBytes = sizeof(MyeReplayHeaderV8);
    if (base.version >= 9) {
        headerBytes = sizeof(MyeReplayHeader);
        f.read(reinterpret_cast<char*>(&header) + sizeof(MyeReplayHeaderV8),
               static_cast<std::streamsize>(sizeof(MyeReplayHeader) - sizeof(MyeReplayHeaderV8)));
        if (!f) {
            MYE_LOG_ERROR("[replay] truncated file (v9 header)");
            return false;
        }
        if ((header.flags & ~kReplayFlagSystemInput) != 0) {
            MYE_LOG_ERROR("[replay] unknown header flags 0x%x", header.flags);
            return false;
        }
    }
    if (header.playerCount == 0 || header.playerCount > kMaxPlayers) {
        MYE_LOG_ERROR("[replay] playerCount = %u (supported: 1..%u)", header.playerCount, kMaxPlayers);
        return false;
    }
    // ★件数は確保の**前**に実ファイル長と突き合わせる。壊れたヘッダの tickCount / snapshotSize を
    //   そのまま resize すると、数バイトのファイルで何 GB も確保しにいく (tickCount x playerCount の
    //   桁あふれもここで起きなくなる: 1 tick の長さで割ってから比べるので掛け算をしない)
    const uint64_t body = fileSize - headerBytes; // ヘッダは読めた = 実ファイルはヘッダ長以上
    if (header.snapshotSize > body) {
        MYE_LOG_ERROR("[replay] truncated file (snapshot %llu bytes, %llu bytes after the header)",
                      static_cast<unsigned long long>(header.snapshotSize),
                      static_cast<unsigned long long>(body));
        return false;
    }
    const uint64_t perTickBytes = ReplayTickRecordBytes(header.playerCount, header.flags);
    if (header.tickCount > (body - header.snapshotSize) / perTickBytes) {
        MYE_LOG_ERROR("[replay] truncated file (%llu ticks declared, room for %llu)",
                      static_cast<unsigned long long>(header.tickCount),
                      static_cast<unsigned long long>((body - header.snapshotSize) / perTickBytes));
        return false;
    }
    std::vector<std::byte> snapshot(static_cast<size_t>(header.snapshotSize));
    if (!snapshot.empty()) {
        f.read(reinterpret_cast<char*>(snapshot.data()), static_cast<std::streamsize>(snapshot.size()));
    }
    // D9: スナップショットを埋めた v9 は、ヘッダの rngState / rngInc が blob の World RNG と
    // 一致しなければ拒否する (RNG の真値が 2 つあって食い違えると、復元経路ごとに別の世界になる)。
    // blob の版が違う (v8 の旧 blob など) と読み取れないが、それは RestoreSimSnapshot が拒む
    if (header.version >= 9 && !snapshot.empty()) {
        uint64_t blobState = 0;
        uint64_t blobInc = 0;
        if (PeekSimSnapshotWorldRng(snapshot.data(), snapshot.size(), blobState, blobInc)
            && (blobState != header.rngState || blobInc != header.rngInc)) {
            MYE_LOG_ERROR("[replay] header RNG (%016llx/%016llx) differs from the embedded "
                          "snapshot's world RNG (%016llx/%016llx)",
                          static_cast<unsigned long long>(header.rngState),
                          static_cast<unsigned long long>(header.rngInc),
                          static_cast<unsigned long long>(blobState),
                          static_cast<unsigned long long>(blobInc));
            return false;
        }
    }
    const size_t perTick = header.playerCount;
    const bool hasSystemInput = (header.flags & kReplayFlagSystemInput) != 0;
    std::vector<InputSnapshot> inputs(static_cast<size_t>(header.tickCount) * perTick);
    std::vector<SystemInputTick> systemInputs(
        hasSystemInput ? static_cast<size_t>(header.tickCount) : 0);
    std::vector<uint64_t> hashes(static_cast<size_t>(header.tickCount));
    for (size_t t = 0; t < hashes.size(); ++t) {
        f.read(reinterpret_cast<char*>(&inputs[t * perTick]),
               static_cast<std::streamsize>(perTick * sizeof(InputSnapshot)));
        if (hasSystemInput) {
            f.read(reinterpret_cast<char*>(&systemInputs[t]), sizeof(SystemInputTick));
            if (f && systemInputs[t].eventCount > kMaxSystemEventsPerTick) {
                MYE_LOG_ERROR("[replay] tick %zu has %u system events (max %u)", t,
                              systemInputs[t].eventCount, kMaxSystemEventsPerTick);
                return false;
            }
        }
        f.read(reinterpret_cast<char*>(&hashes[t]), sizeof(uint64_t));
    }
    if (!f) {
        MYE_LOG_ERROR("[replay] truncated file");
        return false;
    }
    header_ = header;
    snapshot_ = std::move(snapshot);
    inputs_ = std::move(inputs);
    systemInputs_ = std::move(systemInputs);
    hashes_ = std::move(hashes);
    active_ = true;
    MYE_LOG_INFO("[replay] loaded %llu ticks from %s (v%u, players %u, snapshot %zu bytes, system input %s)",
                 static_cast<unsigned long long>(hashes_.size()), WideToUtf8(path).c_str(),
                 header_.version, header_.playerCount, snapshot_.size(),
                 hasSystemInput ? "yes" : "no");
    return true;
}

namespace {

// InputSnapshot のどのフィールドが最初に食い違ったかを名前で返す (空 = 一致)。
// ★HashEntity と同じく**構造体まるごと**を見る — 明示パディング (pad / pad2) まで
//   比較対象に入れているのは、そこが .rep のバイト列に載る以上「一致していない .rep」は
//   本当に一致していないから (M48i の String64 終端以降と同じ理屈)
std::string FirstDifferentInputField(const InputSnapshot& a, const InputSnapshot& b)
{
    for (int i = 0; i < 32; ++i) {
        if (a.keys[i] != b.keys[i]) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "keys[%d]", i);
            return std::string(buf);
        }
    }
    if (a.mouseX != b.mouseX) return "mouseX";
    if (a.mouseY != b.mouseY) return "mouseY";
    // ★M64a で足した生マウスデルタの比較が抜けていた (M70b で回収)。ここに無いと
    //   視点入力だけが食い違ったときに --rep-diff が「入力は同じ」と嘘をつく
    if (a.mouseDeltaX != b.mouseDeltaX) return "mouseDeltaX";
    if (a.mouseDeltaY != b.mouseDeltaY) return "mouseDeltaY";
    if (a.wheelDelta != b.wheelDelta) return "wheelDelta";
    if (a.mouseButtons != b.mouseButtons) return "mouseButtons";
    if (std::memcmp(a.pad, b.pad, sizeof(a.pad)) != 0) return "pad";
    if (a.padButtons != b.padButtons) return "padButtons";
    if (a.padLeftTrigger != b.padLeftTrigger) return "padLeftTrigger";
    if (a.padRightTrigger != b.padRightTrigger) return "padRightTrigger";
    if (a.padLX != b.padLX) return "padLX";
    if (a.padLY != b.padLY) return "padLY";
    if (a.padRX != b.padRX) return "padRX";
    if (a.padRY != b.padRY) return "padRY";
    if (a.padConnected != b.padConnected) return "padConnected";
    if (std::memcmp(a.pad2, b.pad2, sizeof(a.pad2)) != 0) return "pad2";
    // ゲーム面 (M75b、M70b のキャンバス 4 値の後継)。float だが「同じ入力なら同じビット列」の
    // 照合なので == でよい
    if (a.mouseSurfX != b.mouseSurfX) return "mouseSurfX";
    if (a.mouseSurfY != b.mouseSurfY) return "mouseSurfY";
    if (a.surfW != b.surfW) return "surfW";
    if (a.surfH != b.surfH) return "surfH";
    // 文字キュー (M75b)。charCount より先に chars を見る — 数が同じで中身だけ違う列も
    // 「どの文字か」まで名指しできるように
    for (int i = 0; i < 8; ++i) {
        if (a.chars[i] != b.chars[i]) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "chars[%d]", i);
            return std::string(buf);
        }
    }
    if (a.charCount != b.charCount) return "charCount";
    if (std::memcmp(a.pad3, b.pad3, sizeof(a.pad3)) != 0) return "pad3";
    return std::string();
}

} // namespace

ReplayDiffResult DiffReplayFiles(const std::wstring& a, const std::wstring& b)
{
    ReplayDiffResult r;
    ReplayPlayer pa;
    ReplayPlayer pb;
    if (!pa.Load(a) || !pb.Load(b)) {
        r.summary = "one of the .rep files could not be loaded";
        return r;
    }
    const MyeReplayHeader& ha = pa.Header();
    const MyeReplayHeader& hb = pb.Header();
    char buf[256];
    // ヘッダ = 「同じ世界から同じ条件で録り始めたか」。ここが割れているなら
    // tick 列を比べても意味が無い (別のシーン同士を比べているだけ)
    struct HeaderField {
        const char* name;
        uint64_t a;
        uint64_t b;
    };
    const HeaderField fields[] = {
        { "version", ha.version, hb.version },
        { "playerCount", ha.playerCount, hb.playerCount },
        { "rngState", ha.rngState, hb.rngState },
        { "rngInc", ha.rngInc, hb.rngInc },
        { "entityCount", ha.entityCount, hb.entityCount },
        { "snapshotSize", ha.snapshotSize, hb.snapshotSize },
        // ---- v9 (M81b)。sim の結果に効く項目だけを比べる ----
        // ★session.role / inputDelay / deadlineTicks / rejoinTimeoutTicks は**記録者の事情**
        //   (ホストと参加者、サーバとクライアントで違って当然) なので比べない。
        //   provenance も engine / game / protocol / api / replay の版は比べない: Debug と Release の
        //   混在 (--allow-game-mismatch) で正当に食い違う。sim に効く contentHash と schemaVersion、
        //   開始スナップショットの initialSnapshotHash だけを見る
        { "flags", ha.flags, hb.flags },
        { "session.playerCount", ha.session.playerCount, hb.session.playerCount },
        { "session.tickRate", ha.session.tickRate, hb.session.tickRate },
        { "session.seed", ha.session.seed, hb.session.seed },
        { "session.configBits", ha.session.configBits, hb.session.configBits },
        { "session.referenceW", ha.session.referenceW, hb.session.referenceW },
        { "session.referenceH", ha.session.referenceH, hb.session.referenceH },
        { "session.fontMetricsHash", ha.session.fontMetricsHash, hb.session.fontMetricsHash },
        { "provenance.schemaVersion", ha.provenance.schemaVersion, hb.provenance.schemaVersion },
        { "provenance.contentHash", ha.provenance.contentHash, hb.provenance.contentHash },
        { "provenance.initialSnapshotHash", ha.provenance.initialSnapshotHash,
          hb.provenance.initialSnapshotHash },
        { "startMeta.tick", ha.startMeta.tick, hb.startMeta.tick },
        { "startMeta.worldHash", ha.startMeta.worldHash, hb.startMeta.worldHash },
        { "startMeta.lastEventSeq", ha.startMeta.lastEventSeq, hb.startMeta.lastEventSeq },
        // ★tickCount はここに入れない (M52i)。desync バンドルの 2 本は「割れた側が先に
        //   気づいて先に止まる」ので**必ず長さが違う**。長さ違いを門前払いにすると、
        //   本当に見たい「どの tick から割れたか」に一生たどり着けない。
        //   共通部分を比べてから、最後に長さの違いを結論として述べる
    };
    for (const HeaderField& f : fields) {
        if (f.a != f.b) {
            std::snprintf(buf, sizeof(buf), "header.%s differs: %llu vs %llu", f.name,
                          static_cast<unsigned long long>(f.a),
                          static_cast<unsigned long long>(f.b));
            r.summary = buf;
            return r;
        }
    }
    if (pa.Snapshot() != pb.Snapshot()) {
        r.summary = "the embedded start snapshots differ";
        return r;
    }
    const uint64_t ticks = (pa.TickCount() < pb.TickCount()) ? pa.TickCount() : pb.TickCount();
    for (uint64_t t = 0; t < ticks; ++t) {
        for (uint32_t p = 0; p < ha.playerCount; ++p) {
            const std::string field =
                FirstDifferentInputField(pa.InputForTick(t, p), pb.InputForTick(t, p));
            if (!field.empty()) {
                std::snprintf(buf, sizeof(buf),
                              "tick %llu: input lane %u differs at %s (the two runs did NOT "
                              "consume the same input)",
                              static_cast<unsigned long long>(t), p, field.c_str());
                r.firstDiffTick = t;
                r.summary = buf;
                return r;
            }
        }
        if (pa.HasSystemInput() && pb.HasSystemInput()) {
            // flags が一致しているので両方持つか両方持たない。イベントは入力の一部なので
            // ハッシュより先に見る (「同じ入力で割れた」のか「入力が違う」のかを区別する)
            const std::string field =
                FirstDifferentSystemInputField(pa.SystemInputForTick(t), pb.SystemInputForTick(t));
            if (!field.empty()) {
                std::snprintf(buf, sizeof(buf),
                              "tick %llu: %s differs (the two runs did NOT consume the same "
                              "system input)",
                              static_cast<unsigned long long>(t), field.c_str());
                r.firstDiffTick = t;
                r.summary = buf;
                return r;
            }
        }
        if (pa.ExpectedHash(t) != pb.ExpectedHash(t)) {
            std::snprintf(buf, sizeof(buf),
                          "tick %llu: world hash differs (%016llx vs %016llx) - same input, "
                          "different simulation",
                          static_cast<unsigned long long>(t),
                          static_cast<unsigned long long>(pa.ExpectedHash(t)),
                          static_cast<unsigned long long>(pb.ExpectedHash(t)));
            r.firstDiffTick = t;
            r.summary = buf;
            return r;
        }
    }
    if (pa.TickCount() != pb.TickCount()) {
        // 共通部分は完全一致した = 「同じ tick 列を回したが、片方が先に止まった」。
        // これは一致ではないので same は立てない (検証としては失敗のまま)
        std::snprintf(buf, sizeof(buf),
                      "the first %llu ticks are identical but the runs have different lengths "
                      "(%llu vs %llu ticks)",
                      static_cast<unsigned long long>(ticks),
                      static_cast<unsigned long long>(pa.TickCount()),
                      static_cast<unsigned long long>(pb.TickCount()));
        r.firstDiffTick = ticks;
        r.summary = buf;
        return r;
    }
    std::snprintf(buf, sizeof(buf), "identical: %llu ticks x %u lanes",
                  static_cast<unsigned long long>(ticks), ha.playerCount);
    r.same = true;
    r.summary = buf;
    return r;
}

} // namespace mye
