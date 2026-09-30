#pragma once
#include <cstdint>
#include <map>
#include <vector>

namespace mye {

// ゲームフローの時間制御 (M51g、決定台帳 5)。
// ポーズ/タイムスケールは dt スケールではなく「この tick でゲート対象システムを進めるか」の
// tick ゲートで表現する (60Hz 整数 tick の決定論と噛み合わせるため — dt を触ると
// 固定 tick 時刻の前提が全システムで壊れる)。
// Scene が保持する sim 状態で、全フィールドが WorldHash 対象 (RNG の直後に追記)。
struct TimeControl {
    bool paused = false;        // true でゲート対象 (物理/アニメ/衝突/パーティクル) を止める
    int32_t scalePercent = 100; // 100 = 等速、50 = 半速。有効域 0-100 (>100 は 100 扱い)
    int32_t accum = 0;          // スケール蓄積 (0-99)。100 に達した tick だけステップする

    // この tick でゲート対象システムを進めるか。accum を進める副作用があるので
    // 「sim が走る tick に 1 回だけ」呼ぶこと (EngineLoop の tick ループ専用)
    bool Advance()
    {
        if (paused) {
            return false; // ポーズ中は accum も凍結 (再開時は溜めた続きから)
        }
        const int32_t scale = scalePercent < 0 ? 0 : (scalePercent > 100 ? 100 : scalePercent);
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
