//====================================================================================
//                          ComputeBufferStore.cpp
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          ForEach 用バッファストアの実装
//====================================================================================
#include "Engine/Engine/Compute/ComputeBufferStore.h"

#include <cstring>

#include "Engine/Core/Util/ByteIo.h"

namespace mye::compute {
namespace {

constexpr uint32_t kGenerationBits = 30;
constexpr uint32_t kGenerationMask = (1u << kGenerationBits) - 1u;

} // namespace

ComputeBufferStore::ComputeBufferStore(StoreLane lane, ComputeStoreLimits limits)
    : lane_(lane)
    , limits_(limits)
{
}

uint32_t ComputeBufferStore::LaneMark() const
{
    // Game = 0b10、Script = 0b11 (上位 2 bit)
    return (lane_ == StoreLane::Game ? 2u : 3u) << kGenerationBits;
}

uint64_t ComputeBufferStore::Create(uint32_t count, uint32_t stride, BufferKind kind, EntityID owner)
{
    if (count == 0 || stride == 0 || (stride & 3u) != 0 || stride > limits_.maxStride
        || count > limits_.maxElements) {
        return 0;
    }
    const uint64_t bytes = static_cast<uint64_t>(count) * stride;
    if (bytes > limits_.maxBufferBytes) {
        return 0;
    }
    if (kind == BufferKind::State && stateBytes_ + bytes > limits_.stateTotalBytes) {
        return 0;
    }
    // 最小の空きスロット。無ければ末尾へ足す
    size_t index = slots_.size();
    for (size_t i = 0; i < slots_.size(); ++i) {
        if (!slots_[i].live) {
            index = i;
            break;
        }
    }
    if (index == slots_.size()) {
        if (slots_.size() >= limits_.maxSlots) {
            return 0;
        }
        slots_.emplace_back();
    }
    Slot& s = slots_[index];
    s.generation = (s.generation % kGenerationMask) + 1u; // 1..kGenerationMask を巡回。0 にはしない
    s.live = true;
    s.kind = kind;
    s.count = count;
    s.stride = stride;
    s.owner = owner;
    s.data.assign(static_cast<size_t>(bytes), 0);
    s.fxCpuDirty = kind == BufferKind::Fx; // 作ったばかりの Fx は GPU バッファが無い
    if (kind == BufferKind::State) {
        stateBytes_ += bytes;
    }
    return HandleOf(static_cast<uint32_t>(index));
}

uint64_t ComputeBufferStore::HandleOf(uint32_t index) const
{
    if (index >= slots_.size() || !slots_[index].live) {
        return 0;
    }
    const uint32_t gen = LaneMark() | slots_[index].generation;
    return (static_cast<uint64_t>(gen) << 32) | index;
}

ComputeBufferStore::Slot* ComputeBufferStore::Find(uint64_t handle)
{
    return const_cast<Slot*>(static_cast<const ComputeBufferStore*>(this)->Find(handle));
}

const ComputeBufferStore::Slot* ComputeBufferStore::Find(uint64_t handle) const
{
    if (handle == 0) {
        return nullptr;
    }
    const uint32_t gen = static_cast<uint32_t>(handle >> 32);
    const uint32_t index = static_cast<uint32_t>(handle & 0xFFFFFFFFu);
    if (index >= slots_.size() || (gen & ~kGenerationMask) != LaneMark()) {
        return nullptr;
    }
    const Slot& s = slots_[index];
    if (!s.live || s.generation != (gen & kGenerationMask)) {
        return nullptr;
    }
    return &s;
}

bool ComputeBufferStore::Release(uint64_t handle)
{
    Slot* s = Find(handle);
    if (s == nullptr) {
        return false;
    }
    if (s->kind == BufferKind::State) {
        stateBytes_ -= s->data.size();
    }
    s->live = false;
    s->count = 0;
    s->stride = 0;
    s->owner = EntityID{};
    s->fxCpuDirty = false;
    std::vector<uint8_t>().swap(s->data);
    return true;
}

void ComputeBufferStore::ReleaseAll()
{
    slots_.clear();
    stateBytes_ = 0;
}

bool ComputeBufferStore::Write(uint64_t handle, const void* src, uint32_t count)
{
    Slot* s = Find(handle);
    if (s == nullptr || src == nullptr || count > s->count) {
        return false;
    }
    std::memcpy(s->data.data(), src, static_cast<size_t>(count) * s->stride);
    if (s->kind == BufferKind::Fx) {
        s->fxCpuDirty = true;
    }
    return true;
}

bool ComputeBufferStore::Read(uint64_t handle, void* dst, uint32_t count) const
{
    const Slot* s = Find(handle);
    if (s == nullptr || dst == nullptr || count > s->count) {
        return false;
    }
    std::memcpy(dst, s->data.data(), static_cast<size_t>(count) * s->stride);
    return true;
}

uint32_t ComputeBufferStore::LiveCount() const
{
    uint32_t n = 0;
    for (const Slot& s : slots_) {
        n += s.live ? 1u : 0u;
    }
    return n;
}

bool ComputeBufferStore::HasLiveState() const
{
    for (const Slot& s : slots_) {
        if (s.live && s.kind == BufferKind::State) {
            return true;
        }
    }
    return false;
}

namespace {
constexpr uint8_t kSnapDead = 0;
constexpr uint8_t kSnapState = 1;
constexpr uint8_t kSnapFx = 2;
} // namespace

void ComputeBufferStore::WriteSnapshot(ByteWriter& w) const
{
    w.Count(slots_.size());
    for (const Slot& s : slots_) {
        w.U32(s.generation);
        if (!s.live) {
            w.U8(kSnapDead);
        } else if (s.kind == BufferKind::Fx) {
            w.U8(kSnapFx);
        } else {
            w.U8(kSnapState);
            w.U32(s.count);
            w.U32(s.stride);
            w.U32(s.owner.index);
            w.U32(s.owner.generation);
            w.Raw(s.data.data(), s.data.size());
        }
    }
}

bool ComputeBufferStore::ReadSnapshot(ByteReader& r, std::vector<Slot>& out) const
{
    out.clear();
    // 1 スロットは最低 5 バイト (世代 + 印)
    const size_t n = r.Count(sizeof(uint32_t) + sizeof(uint8_t));
    if (!r.Ok() || n > limits_.maxSlots) {
        return false;
    }
    out.resize(n);
    for (Slot& s : out) {
        s.generation = r.U32();
        const uint8_t mark = r.U8();
        if (!r.Ok() || s.generation > kGenerationMask) {
            return false;
        }
        if (mark == kSnapDead) {
            continue;
        }
        s.live = true;
        if (mark == kSnapFx) {
            s.kind = BufferKind::Fx;
            continue;
        }
        if (mark != kSnapState) {
            return false;
        }
        s.kind = BufferKind::State;
        s.count = r.U32();
        s.stride = r.U32();
        s.owner.index = r.U32();
        s.owner.generation = r.U32();
        // Create と同じ上限で検証する (壊れた blob の巨大な確保を防ぐ)。State の総バイト枠は、設定が
        // 録画時から変わっていても復元を拒まないよう見ない
        if (!r.Ok() || s.generation == 0 || s.count == 0 || s.stride == 0 || (s.stride & 3u) != 0
            || s.stride > limits_.maxStride || s.count > limits_.maxElements) {
            return false;
        }
        const uint64_t bytes = static_cast<uint64_t>(s.count) * s.stride;
        if (bytes > limits_.maxBufferBytes || bytes > r.Remaining()) {
            return false;
        }
        s.data.resize(static_cast<size_t>(bytes));
        r.Raw(s.data.data(), s.data.size());
    }
    return r.Ok();
}

void ComputeBufferStore::ApplySnapshot(std::vector<Slot>&& snapshot, std::vector<uint64_t>& droppedFx)
{
    // 撮影後に作られた Fx (表の外・世代違い) は捨てる。Fx は表示用で sim の状態ではない
    for (uint32_t i = 0; i < slots_.size(); ++i) {
        const Slot& cur = slots_[i];
        if (!cur.live || cur.kind != BufferKind::Fx) {
            continue;
        }
        const bool kept = i < snapshot.size() && snapshot[i].live && snapshot[i].kind == BufferKind::Fx
            && snapshot[i].generation == cur.generation;
        if (!kept) {
            droppedFx.push_back(HandleOf(i));
        }
    }
    std::vector<Slot> next(snapshot.size());
    stateBytes_ = 0;
    for (size_t i = 0; i < snapshot.size(); ++i) {
        Slot& s = snapshot[i];
        if (s.live && s.kind == BufferKind::Fx) {
            if (i < slots_.size() && slots_[i].live && slots_[i].kind == BufferKind::Fx
                && slots_[i].generation == s.generation) {
                next[i] = std::move(slots_[i]);
            } else {
                next[i].generation = s.generation;
            }
        } else {
            if (s.live) {
                stateBytes_ += s.data.size();
            }
            next[i] = std::move(s);
        }
    }
    slots_ = std::move(next);
}

} // namespace mye::compute
