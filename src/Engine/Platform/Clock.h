#pragma once
#include <cstdint>

namespace mye {

// QueryPerformanceCounter ラッパー。
// 実時間はフレームレート制御と accumulator への供給にのみ使う。
// シミュレーションは常に固定 dt (EngineLoop) で進むため、ここの値が
// ゲームロジックへ直接渡ることはない (spec 11 章 決定論ポリシー)。
class Clock {
public:
    void Init();

    // 前回 BeginFrame からの経過秒を返す (初回は 0)
    double BeginFrame();

    // Init からの経過秒
    double Now() const;

private:
    int64_t freq_ = 1;
    int64_t start_ = 0;
    int64_t last_ = 0;
};

} // namespace mye
