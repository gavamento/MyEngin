//====================================================================================
//                          GpuAliveEstimator.h
//  MyEngin/ 秋田蓮音                                                       08/27/2026
//                                          GPUパーティクル生存数のCPU側推定（寿命スケジュール）
//====================================================================================
#pragma once
#include <cmath>
#include <cstdint>
#include <map>

namespace mye {

// GPU パーティクルの生存数を CPU 側で推定する (リードバック禁止 — ADR-008)。
// 放出時に「死亡予定 tick → 個数」のバケットへ積み、tick 境界で満期分を減算する。
// GPU 側の死因は寿命のみ (particle_sim.cs.hlsl: life -= dt; life <= 0 で dead list へ。
// 深度衝突 M42e は反射するだけで殺さない) なので、寿命は CPU の決定論 RNG が生成済み
// という前提と合わせて readback なしにほぼ正確な生存数が出せる。
// ずれる要因は 2 つだけ:
// - dead list 枯渇 (容量飽和) で GPU 側の放出が落ちたとき → Alive() の上限クランプで吸収
// - GPU の逐次減算 (life - dt を k 回) と ceil(life/dt) の丸め差 → 境界値で ±1 tick
// 表示専用の概算 — sim 状態やワールドハッシュには決して載せないこと。
struct GpuAliveEstimator {
    // 今 tick 放出した 1 粒を記帳する。lifeSeconds は放出時の寿命、dt は固定 tick 長。
    // 放出 tick に sim も走る (放出 → 更新の順) ため初回の life -= dt は当 tick に起きる。
    // k 回目の sim で life - k*dt <= 0 となるので、死亡 tick = 放出 tick + k - 1
    void OnEmit(float lifeSeconds, float dt)
    {
        int32_t k = (dt > 0.0f) ? static_cast<int32_t>(std::ceil(lifeSeconds / dt)) : 1;
        if (k < 1) {
            k = 1; // life <= dt は放出と同 tick で死亡 (aliveOut に入らない)
        }
        deathBuckets_[tick_ + k - 1] += 1;
        ++alive_;
    }

    // tick 境界: 今 tick の sim で死んだ分を減算して次 tick へ進める。
    // 当 tick の OnEmit をすべて記帳した後、Update の末尾で 1 回呼ぶこと
    void EndTick()
    {
        while (!deathBuckets_.empty() && deathBuckets_.begin()->first <= tick_) {
            alive_ -= deathBuckets_.begin()->second;
            deathBuckets_.erase(deathBuckets_.begin());
        }
        ++tick_;
    }

    // 現在の生存数の推定。capacity で頭打ちにする (飽和時は GPU 側の放出が落ちるため)
    uint32_t Alive(uint32_t capacity) const { return alive_ < capacity ? alive_ : capacity; }

private:
    std::map<int32_t, uint32_t> deathBuckets_; // 死亡予定 tick → 個数 (昇順 = 満期順)
    uint32_t alive_ = 0;                       // バケット合計のミラー (毎回の総和を避ける)
    int32_t tick_ = 0;
};

} // namespace mye
