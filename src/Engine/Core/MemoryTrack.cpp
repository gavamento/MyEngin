#include "Engine/Core/Profiler.h"

#include <atomic>
#include <cstdlib>
#include <new>

// global operator new/delete フック (M12)。
// - このオブジェクトは Engine.lib に入り、GetMemoryStats() の参照でリンクに引き込まれる
//   → Editor.exe / (将来の) Runtime.exe 内の new/delete を捕捉する。
//   GameLogic.dll は Engine.lib をリンクしない別 CRT モジュールなので対象外 (spec 8.4 / 計画)。
// - カウンタは atomic のみ (ロック/コンテナ無し) なので再入・デッドロックしない。
// - 名前空間スコープの atomic{0} は定数初期化される → main 前の静的初期化中の確保も安全に数える。
// - サイズ非追跡 (ヘッダを付けるとアライメント整合が崩れるため)。生存「数」と累計バイトのみ。
namespace {
std::atomic<uint64_t> g_allocs{ 0 };
std::atomic<uint64_t> g_frees{ 0 };
std::atomic<uint64_t> g_bytes{ 0 };
} // namespace

namespace mye::prof {

MemStats GetMemoryStats()
{
    MemStats s;
    s.totalAllocs = g_allocs.load(std::memory_order_relaxed);
    s.totalFrees = g_frees.load(std::memory_order_relaxed);
    s.liveAllocs = s.totalAllocs - s.totalFrees;
    s.totalBytes = g_bytes.load(std::memory_order_relaxed);
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
