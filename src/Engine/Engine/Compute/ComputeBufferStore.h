//====================================================================================
//                          ComputeBufferStore.h
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          ForEach 用バッファストア (State / Fx。決定的な採番)
//====================================================================================
#pragma once
#include <cstdint>
#include <vector>

#include "Engine/Engine/Compute/ComputeTypes.h"

namespace mye::compute {

struct ComputeStoreLimits {
    uint32_t maxSlots = 1024;
    uint32_t maxElements = 4194240;           // kMaxKernelElements と同じ (D21)
    uint32_t maxStride = 2048;                // D3D11 の StructureByteStride の上限
    uint64_t maxBufferBytes = 128ull << 20;   // 1 本あたり
    uint64_t stateTotalBytes = 16ull << 20;   // State の生存分の合計
};

// ストアの種類。ハンドルの世代欄の上位 2 bit に入り、他のストアと v21 (ComputeAbiRunner) のハンドルを弾く
enum class StoreLane : uint8_t { Game = 0, Script = 1 };

// バッファのハンドルは「世代 32 bit + スロット 32 bit」で、最小の空きスロットから採番する。
// 世代欄は [lane 印 2 bit | スロットごとの作成回数 30 bit]。lane 印は常に非 0 (v21 の世代は小さい連番)。
// 乱数・時間・ポインタを使わないので、同じ操作列から同じハンドルが出る (スナップショットの対象にできる)。
class ComputeBufferStore {
public:
    struct Slot {
        uint32_t generation = 0; // 作成回数 (解放しても戻さない)。0 = 未使用
        bool live = false;
        BufferKind kind = BufferKind::State;
        uint32_t count = 0;
        uint32_t stride = 0;
        std::vector<uint8_t> data; // CPU 側の中身 (Fx では GPU の CPU ミラー)
        bool fxCpuDirty = false;   // Fx: ミラーが GPU より新しい (次の GPU 実行の前に upload する)
    };

    explicit ComputeBufferStore(StoreLane lane, ComputeStoreLimits limits = {});

    // 0 = 失敗。count / stride が 0、上限超過、スロット満杯、State の総バイト枠超過
    uint64_t Create(uint32_t count, uint32_t stride, BufferKind kind);
    // 無効ハンドル・二重解放は false で何もしない
    bool Release(uint64_t handle);
    void ReleaseAll();

    bool IsValid(uint64_t handle) const { return Find(handle) != nullptr; }
    // 無効なら nullptr。ポインタは次の Create / Release まで有効
    Slot* Find(uint64_t handle);
    const Slot* Find(uint64_t handle) const;
    uint32_t SlotIndex(uint64_t handle) const { return static_cast<uint32_t>(handle & 0xFFFFFFFFu); }

    // 先頭から count 要素を書く / 読む。count がバッファより大きい・無効ハンドル・null は false で何もしない。
    // Fx への Write は fxCpuDirty を立てる
    bool Write(uint64_t handle, const void* src, uint32_t count);
    bool Read(uint64_t handle, void* dst, uint32_t count) const;

    const ComputeStoreLimits& Limits() const { return limits_; }
    void SetStateTotalBytes(uint64_t bytes) { limits_.stateTotalBytes = bytes; }
    uint32_t LiveCount() const;
    uint64_t StateBytes() const { return stateBytes_; }
    const std::vector<Slot>& Slots() const { return slots_; }
    std::vector<Slot>& MutableSlots() { return slots_; }
    // スロット番号 index が今の世代で持つハンドル (生きていなければ 0)
    uint64_t HandleOf(uint32_t index) const;
    StoreLane Lane() const { return lane_; }

private:
    uint32_t LaneMark() const;

    StoreLane lane_;
    ComputeStoreLimits limits_;
    std::vector<Slot> slots_;
    uint64_t stateBytes_ = 0;
};

} // namespace mye::compute
