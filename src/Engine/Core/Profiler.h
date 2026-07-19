#pragma once
#include <cstdint>
#include <vector>

// 軽量プロファイラ (M12)。決定論規約: 計測値は sim 状態でもワールドハッシュでもないため
// リプレイに影響しない。スコープ記録はフレーム毎に固定領域へ貯める (tick 中の追加確保なし)。
namespace mye::prof {

// ---- CPU 階層スコープ計測 ----
struct ScopeRecord {
    const char* name;
    float ms;
    int depth;
};

void BeginFrame();                // 毎フレーム先頭で呼ぶ (前フレーム分をクリア)
void PushScope(const char* name); // 手動 (通常は MYE_PROFILE_SCOPE を使う)
void PopScope();
const std::vector<ScopeRecord>& FrameScopes(); // 表示用 (今フレームの確定済みスコープ)

struct ScopeTimer {
    explicit ScopeTimer(const char* n) { PushScope(n); }
    ~ScopeTimer() { PopScope(); }
    ScopeTimer(const ScopeTimer&) = delete;
    ScopeTimer& operator=(const ScopeTimer&) = delete;
};

// ---- レンダ統計 (ドローコール / 三角形) ----
struct RenderStats {
    int drawCalls = 0;
    int triangles = 0;
};
void AddDraw(int triangles); // 描画パスの DrawIndexed 地点で呼ぶ
RenderStats GetRenderStats();

// ---- メモリ (MemoryTrack.cpp の global operator new/delete フック) ----
struct MemStats {
    uint64_t totalAllocs = 0; // 累計確保回数
    uint64_t totalFrees = 0;  // 累計解放回数
    uint64_t liveAllocs = 0;  // 生存中 (allocs - frees)
    uint64_t totalBytes = 0;  // 累計確保バイト数
};
MemStats GetMemoryStats();

} // namespace mye::prof

#define MYE_PROF_CAT2(a, b) a##b
#define MYE_PROF_CAT(a, b) MYE_PROF_CAT2(a, b)
#define MYE_PROFILE_SCOPE(name) ::mye::prof::ScopeTimer MYE_PROF_CAT(prof_scope_, __LINE__)(name)
