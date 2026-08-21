#include "Engine/Engine/Replay/CrashRing.h"

#include <cstring>

#include <Windows.h>

#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Replay/Replay.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Platform/CrashHandler.h"

namespace mye {
namespace {

// ハンドラ内で使う作業領域 (スタックへ置かない — CrashHandler.cpp と同じ理由)
wchar_t g_payloadPath[1024] = {};

MyeReplayHeader* HeaderOf(std::vector<std::byte>& image)
{
    return reinterpret_cast<MyeReplayHeader*>(image.data());
}
const MyeReplayHeader* HeaderOf(const std::vector<std::byte>& image)
{
    return reinterpret_cast<const MyeReplayHeader*>(image.data());
}

} // namespace

bool CrashRing::Begin(const SimRefs& refs, uint64_t tick)
{
    if (!enabled_) {
        return false;
    }
    if (!TakeSnapshot(refs, tick)) {
        MYE_LOG_WARN("[crash] could not capture the sim snapshot - crash.rep is disabled");
        enabled_ = false;
        return false;
    }
    MYE_LOG_INFO("[crash] rep ring armed at tick %llu (%zu bytes/snapshot, %llu tick interval)",
                 static_cast<unsigned long long>(tick), snapshotBytes_,
                 static_cast<unsigned long long>(config_.snapshotInterval));
    return true;
}

bool CrashRing::TakeSnapshot(const SimRefs& refs, uint64_t tick)
{
    if (refs.scene == nullptr || !CaptureSimSnapshot(refs, scratch_)) {
        return false;
    }
    World& world = refs.scene->GetWorld(); // Rng() は非 const 版しか無い
    const uint32_t lanes = (config_.playerCount == 0) ? 1u : config_.playerCount; // M52g
    const size_t recordBytes = sizeof(InputSnapshot) * lanes + sizeof(uint64_t);
    const size_t need = sizeof(MyeReplayHeader) + scratch_.size() + config_.maxTicks * recordBytes;

    // ★ここから ready_ = true までは .rep として一貫していない。
    //   image_ の再確保も含むので、この区間で落ちたら crash.rep は諦める (crash.txt は残る)。
    //   ★recordBytes_ の更新も**この中**でやること — ハンドラは RecordBytes() と
    //     イメージ内のヘッダ (playerCount) を突き合わせて長さを出すので、
    //     フラグを下ろす前に片方だけ書き換えると一貫しないイメージが出口を通れてしまう
    ready_ = false;
    recordBytes_ = recordBytes;
    if (image_.size() < need) {
        image_.assign(need, std::byte{ 0 });
    }
    MyeReplayHeader header;
    header.rngState = world.Rng().State();
    header.rngInc = world.Rng().Inc();
    header.entityCount = world.AliveCount();
    header.snapshotSize = scratch_.size();
    header.tickCount = 0;
    header.playerCount = lanes; // M52g
    std::memcpy(image_.data(), &header, sizeof(header));
    std::memcpy(image_.data() + sizeof(header), scratch_.data(), scratch_.size());

    recordBase_ = sizeof(header) + scratch_.size();
    snapshotBytes_ = scratch_.size();
    snapshotTick_ = tick;
    nextTick_ = tick;
    ticksSinceSnapshot_ = 0;
    inFlight_ = false;
    ++snapshotCount_;
    ready_ = true;
    return true;
}

std::byte* CrashRing::RecordAt(uint64_t index)
{
    return image_.data() + recordBase_ + static_cast<size_t>(index) * RecordBytes();
}

void CrashRing::OnTickBegin(uint64_t tick, const InputSnapshot* inputs, uint32_t playerCount)
{
    if (!enabled_ || !ready_) {
        return;
    }
    if (tick != nextTick_) {
        // シーク (M52e) やリングの取り直し要求で tick 列が飛んだ。
        // 連続していない入力列は .rep として意味を成さないので、記録を止めて
        // 次の tick 末に撮り直す (Begin し直すのと同じ状態)
        ready_ = false;
        return;
    }
    MyeReplayHeader* header = HeaderOf(image_);
    if (header->tickCount >= config_.maxTicks) {
        ready_ = false; // 上限。次の tick 末で撮り直す
        return;
    }
    std::byte* rec = RecordAt(header->tickCount);
    const uint32_t lanes = header->playerCount;
    const InputSnapshot zero = {};
    for (uint32_t p = 0; p < lanes; ++p) {
        const InputSnapshot& src = (inputs != nullptr && p < playerCount) ? inputs[p] : zero;
        std::memcpy(rec + sizeof(InputSnapshot) * p, &src, sizeof(InputSnapshot));
    }
    // ★まだ走っていない tick なので期待ハッシュは存在しない = 予約値 0 (Replay.h)
    const uint64_t unverified = 0;
    std::memcpy(rec + sizeof(InputSnapshot) * lanes, &unverified, sizeof(uint64_t));
    // ここで初めてレコードが「見える」。発行は tickCount の 1 ストアだけなので、
    // どこで落ちてもイメージは常に整合する (書きかけのレコードは範囲外に居る)
    header->tickCount += 1;
    inFlight_ = true;
}

void CrashRing::OnTickEnd(const SimRefs& refs, uint64_t ranTick, uint64_t hashAfter)
{
    if (!enabled_) {
        return;
    }
    if (ready_ && inFlight_) {
        MyeReplayHeader* header = HeaderOf(image_);
        std::byte* rec = RecordAt(header->tickCount - 1);
        // 8 バイト整列の単一ストア = 途中で落ちても中途半端な値にはならない
        std::memcpy(rec + sizeof(InputSnapshot) * header->playerCount, &hashAfter,
                    sizeof(uint64_t));
        inFlight_ = false;
        nextTick_ = ranTick + 1;
        ++ticksSinceSnapshot_;
    }
    if (!ready_ || ticksSinceSnapshot_ >= config_.snapshotInterval
        || HeaderOf(image_)->tickCount >= config_.maxTicks) {
        TakeSnapshot(refs, ranTick + 1);
    }
}

const std::byte* CrashRing::RepImage(size_t& outSize) const
{
    outSize = 0;
    if (!ready_ || image_.empty()) {
        return nullptr;
    }
    const MyeReplayHeader* header = HeaderOf(image_);
    const size_t size = sizeof(MyeReplayHeader) + static_cast<size_t>(header->snapshotSize)
        + static_cast<size_t>(header->tickCount) * RecordBytes();
    if (size > image_.size()) {
        return nullptr; // 整合していない (ここへ来たら実装のバグ)
    }
    outSize = size;
    return image_.data();
}

bool CrashRing::WriteRepFile(const wchar_t* path) const
{
    size_t size = 0;
    const std::byte* data = RepImage(size);
    if (data == nullptr || size == 0) {
        return false;
    }
    return CrashWriteFileRaw(path, data, size);
}

uint64_t CrashRing::RecordCount() const
{
    if (image_.empty()) {
        return 0;
    }
    return HeaderOf(image_)->tickCount;
}

size_t CrashRing::ImageBytes() const
{
    size_t size = 0;
    RepImage(size);
    return size;
}

void WriteCrashPayload(void* user, const wchar_t* bundleDir)
{
    auto* payload = static_cast<CrashPayload*>(user);
    if (payload == nullptr || bundleDir == nullptr) {
        return;
    }
    if (payload->ring != nullptr) {
        crashfmt::WSink p(g_payloadPath, 1024);
        p.Str(bundleDir);
        p.Ascii("\\crash.rep");
        payload->ring->WriteRepFile(g_payloadPath);
    }
    if (payload->sceneSource[0] != L'\0') {
        // ★シーンの再シリアライズはしない (JSON 生成 = 確保の塊)。元ファイルを丸ごと写すだけ。
        //   コードから組んだシーン (デモ等) は sourceSource が空 = 何も出ない —
        //   crash.rep には開始スナップショットが埋まっているので再現には要らない
        crashfmt::WSink p(g_payloadPath, 1024);
        p.Str(bundleDir);
        p.Ascii("\\scene.json");
        CopyFileW(payload->sceneSource, g_payloadPath, FALSE);
    }
}

} // namespace mye
