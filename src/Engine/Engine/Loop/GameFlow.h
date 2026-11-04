#pragma once
#include <cstdint>
#include <map>
#include <vector>

#include "Shared/FeelMath.h"

namespace mye {

// ゲームフローの時間制御 (M51g、決定台帳 5)。
// ポーズ/タイムスケールは dt スケールではなく「この tick でゲート対象システムを進めるか」の
// tick ゲートで表現する (60Hz 整数 tick の決定論と噛み合わせるため — dt を触ると
// 固定 tick 時刻の前提が全システムで壊れる)。
// Scene が保持する sim 状態で、全フィールドが WorldHash 対象 (RNG の直後に追記)。
//
// M94b: 手触りの時間効果 (ヒットストップと時限スロー、ADR-031)。どちらも同じ tick ゲートの上に載る:
//   - ヒットストップ: freezeTicks が残っている tick はゲート対象を進めない (accum も凍結)
//   - 時限スロー: slowHoldTicks の間 slowPercent、続く slowRecoverTicks で 100% へ戻る
//     (戻り方は slowRecoverEase)。スローの tick はヒットストップが明けてから数える
//   実効の速さは min(scalePercent, スローの現在値)。要求は RequestFreeze / RequestSlow だけが受ける。
//   ★既定値 (効果なし) のあいだは HasFeelEffect() が偽で、ハッシュはこの 6 欄を畳まない
//     = M94 以前の録画とハッシュ列が 1 ビットも変わらない
inline constexpr int32_t kMaxFeelTimeTicks = 600; // 1 回の要求で止める・遅くする上限 (10 秒)

struct TimeControl {
    bool paused = false;        // true でゲート対象 (物理/アニメ/衝突/パーティクル) を止める
    int32_t scalePercent = 100; // 100 = 等速、50 = 半速。有効域 0-100 (>100 は 100 扱い)
    int32_t accum = 0;          // スケール蓄積 (0-99)。100 に達した tick だけステップする
    // ---- M94b: 手触りの時間効果 ----
    int32_t freezeTicks = 0;      // 残りのヒットストップ tick
    int32_t slowPercent = 100;    // 時限スローの速さ [0,100]
    int32_t slowHoldTicks = 0;    // slowPercent を保つ残り tick
    int32_t slowRecoverTicks = 0; // 100% へ戻る全 tick 数
    int32_t slowRecoverLeft = 0;  // 戻りの残り tick
    int32_t slowRecoverEase = 0;  // 戻り方 (MYE_EASE_*)

    // ヒットストップかスローが残っているか (= ハッシュに 6 欄を畳むか)
    bool HasFeelEffect() const
    {
        return freezeTicks != 0 || slowPercent != 100 || slowHoldTicks != 0 || slowRecoverTicks != 0
            || slowRecoverLeft != 0 || slowRecoverEase != 0;
    }

    // スローの「次に進む tick」で使う速さ。スローが無ければ 100
    int32_t CurrentSlowPercent() const
    {
        if (slowHoldTicks > 0) {
            return slowPercent;
        }
        if (slowRecoverLeft > 0 && slowRecoverTicks > 0) {
            // 戻りの k 本目 (1..R) で進み具合 k / R。最後の 1 本でちょうど 100 になる
            const float progress = static_cast<float>(slowRecoverTicks - slowRecoverLeft + 1)
                / static_cast<float>(slowRecoverTicks);
            const float pct = static_cast<float>(slowPercent)
                + static_cast<float>(100 - slowPercent) * MyeEase(slowRecoverEase, progress);
            const int32_t rounded = static_cast<int32_t>(pct + 0.5f);
            return rounded < 0 ? 0 : (rounded > 100 ? 100 : rounded);
        }
        return 100;
    }

    // ヒットストップを要求する。残りと比べて長い方を残す (足し算はしない — 連打で止まり続けない)
    void RequestFreeze(int32_t ticks)
    {
        const int32_t t = ticks < 0 ? 0 : (ticks > kMaxFeelTimeTicks ? kMaxFeelTimeTicks : ticks);
        if (t > freezeTicks) {
            freezeTicks = t;
        }
    }

    // 時限スローを要求する。今のスローより遅いものが勝ち、同じ速さなら残りが長いものが勝つ。
    // percent >= 100 か、保持も戻りも 0 なら何もしない
    void RequestSlow(int32_t percent, int32_t holdTicks, int32_t recoverTicks, int32_t recoverEase)
    {
        const int32_t pct = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
        const int32_t hold = holdTicks < 0 ? 0 : (holdTicks > kMaxFeelTimeTicks ? kMaxFeelTimeTicks : holdTicks);
        const int32_t rec = recoverTicks < 0 ? 0 : (recoverTicks > kMaxFeelTimeTicks ? kMaxFeelTimeTicks : recoverTicks);
        if (pct >= 100 || (hold == 0 && rec == 0)) {
            return;
        }
        const bool active = slowHoldTicks > 0 || slowRecoverLeft > 0;
        const int32_t cur = CurrentSlowPercent();
        const bool wins = !active || pct < cur || (pct == cur && hold + rec > slowHoldTicks + slowRecoverLeft);
        if (!wins) {
            return;
        }
        slowPercent = pct;
        slowHoldTicks = hold;
        slowRecoverTicks = rec;
        slowRecoverLeft = rec;
        slowRecoverEase = (recoverEase >= 0 && recoverEase < MYE_EASE_COUNT) ? recoverEase : 0;
    }

    // ヒットストップとスローを既定値へ戻す (シーン遷移・停止)
    void ClearFeelEffects()
    {
        freezeTicks = 0;
        slowPercent = 100;
        slowHoldTicks = 0;
        slowRecoverTicks = 0;
        slowRecoverLeft = 0;
        slowRecoverEase = 0;
    }

    // この tick でゲート対象システムを進めるか。accum とスローの残りを進める副作用があるので
    // 「sim が走る tick に 1 回だけ」呼ぶこと (EngineLoop の tick ループ専用)
    bool Advance()
    {
        if (paused) {
            return false; // ポーズ中は accum もヒットストップもスローも凍結 (再開時は続きから)
        }
        if (freezeTicks > 0) {
            --freezeTicks; // ヒットストップ: accum も進めない (明けた tick は止まる前の位相から)
            return false;
        }
        const int32_t base = scalePercent < 0 ? 0 : (scalePercent > 100 ? 100 : scalePercent);
        const int32_t slow = CurrentSlowPercent();
        const int32_t scale = slow < base ? slow : base;
        if (slowHoldTicks > 0) {
            --slowHoldTicks;
        } else if (slowRecoverLeft > 0) {
            --slowRecoverLeft;
        }
        if (slowHoldTicks == 0 && slowRecoverLeft == 0 && (slowPercent != 100 || slowRecoverTicks != 0)) {
            slowPercent = 100; // 終わったスローは既定値へ畳む (HasFeelEffect を偽へ戻す)
            slowRecoverTicks = 0;
            slowRecoverEase = 0;
        }
        accum += scale;
        if (accum >= 100) {
            accum -= 100; // scale <= 100 なので残りは常に 0-99
            return true;
        }
        return false;
    }
};

// シーン跨ぎ永続 key-value ストア (M51g、決定台帳 5)。
// std::map (ordered) = キー昇順走査が決定論なので WorldHash / セーブ出力にそのまま使える。
// キーは名前の FNV-1a ハッシュ (HashStr)、値は生バイト列 (型付き糖衣は M51h の ABI 側)。
// Scene が保持するが Scene::Clear / LoadScene では消えない (シーンを跨いで生きるのが存在意義)。
// Play/Stop の復元は PlayModeController がスナップショットする。
class PersistStore {
public:
    using Map = std::map<uint64_t, std::vector<uint8_t>>;

    void Set(uint64_t key, const void* data, size_t size)
    {
        std::vector<uint8_t>& v = entries_[key];
        v.clear();
        if (size > 0) {
            const uint8_t* p = static_cast<const uint8_t*>(data);
            v.assign(p, p + size);
        }
    }
    // 見つからなければ nullptr (空 blob の「ある」と不在を区別する)
    const std::vector<uint8_t>* Find(uint64_t key) const
    {
        auto it = entries_.find(key);
        return (it != entries_.end()) ? &it->second : nullptr;
    }
    bool Erase(uint64_t key) { return entries_.erase(key) != 0; }
    void Clear() { entries_.clear(); }
    Map& Entries() { return entries_; }
    const Map& Entries() const { return entries_; }

private:
    Map entries_;
};

} // namespace mye
