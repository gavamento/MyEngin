#include "Engine/Core/Diagnostics/Profiler.h"

#include <atomic>
#include <cstdlib>
#include <malloc.h>
#include <new>

// global operator new/delete フック (M12)。
// - このオブジェクトは Engine.lib に入り、GetMemoryStats() の参照でリンクに引き込まれる
//   → Editor.exe / Runtime.exe 内の new/delete を捕捉する。
//   GameLogic.dll は Engine.lib をリンクしない別 CRT モジュールなので対象外 (spec 8.4 / 計画)。
// - カウンタは atomic のみ (ロック/コンテナ無し) なので再入・デッドロックしない。
// - 名前空間スコープの atomic{0} は定数初期化される → main 前の静的初期化中の確保も安全に数える。
// - ブロックにヘッダを付けない (アライメント整合が崩れるため)。生存バイト数は確保・解放の両側で
//   _msize (ヒープが実際に割り当てたサイズ) を足し引きして出す。要求サイズで引けないのは、
//   解放側が要求サイズを知らないため。_msize のコストは実測で 1 回あたり 10 ns 前後
//   (malloc + free 1 組が 65 ns 前後の機械。plans/profiler-overhaul.md)。
// - 札 (MemTag) は確保したスレッドの thread_local を読むだけ。解放側は札を知らない。
namespace {
std::atomic<uint64_t> g_allocs{ 0 };
std::atomic<uint64_t> g_frees{ 0 };
std::atomic<uint64_t> g_bytes{ 0 };
std::atomic<uint64_t> g_blockBytesAllocated{ 0 }; // _msize の和 (確保側)
std::atomic<uint64_t> g_blockBytesFreed{ 0 };     // _msize の和 (解放側)
std::atomic<uint64_t> g_tagAllocs[mye::prof::kMemTagCount]{};
std::atomic<uint64_t> g_tagBytes[mye::prof::kMemTagCount]{};
} // namespace

namespace mye::prof {

MemStats GetMemoryStats()
{
    MemStats s;
    s.totalAllocs = g_allocs.load(std::memory_order_relaxed);
    s.totalFrees = g_frees.load(std::memory_order_relaxed);
    s.liveAllocs = s.totalAllocs - s.totalFrees;
    s.totalBytes = g_bytes.load(std::memory_order_relaxed);
    // 2 つを別々に読むので、間に他スレッドの確保・解放が挟まると瞬間的にずれる。表示用なので許す
    // (解放側が先に進んで負になる場合だけ 0 に丸める)
    const uint64_t allocated = g_blockBytesAllocated.load(std::memory_order_relaxed);
    const uint64_t freed = g_blockBytesFreed.load(std::memory_order_relaxed);
    s.liveBytes = (allocated > freed) ? allocated - freed : 0;
    for (int i = 0; i < kMemTagCount; ++i) {
        s.tagAllocs[i] = g_tagAllocs[i].load(std::memory_order_relaxed);
        s.tagBytes[i] = g_tagBytes[i].load(std::memory_order_relaxed);
    }
    return s;
}

} // namespace mye::prof

void* operator new(std::size_t size)
{
    void* p = std::malloc(size != 0 ? size : 1);
    if (!p) {
        throw std::bad_alloc();
    }
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    g_bytes.fetch_add(size, std::memory_order_relaxed);
    g_blockBytesAllocated.fetch_add(_msize(p), std::memory_order_relaxed);
    const int tag = static_cast<int>(mye::prof::CurrentMemTag());
    g_tagAllocs[tag].fetch_add(1, std::memory_order_relaxed);
    g_tagBytes[tag].fetch_add(size, std::memory_order_relaxed);
    return p;
}

void* operator new[](std::size_t size)
{
    return operator new(size);
}

void operator delete(void* p) noexcept
{
    if (p) {
        g_frees.fetch_add(1, std::memory_order_relaxed);
        g_blockBytesFreed.fetch_add(_msize(p), std::memory_order_relaxed);
        std::free(p);
    }
}

void operator delete[](void* p) noexcept
{
    operator delete(p);
}

// サイズ付き delete (C++14。定義しないと CRT 既定が呼ばれカウンタが漏れる)
void operator delete(void* p, std::size_t) noexcept
{
    operator delete(p);
}

void operator delete[](void* p, std::size_t) noexcept
{
    operator delete(p);
}
