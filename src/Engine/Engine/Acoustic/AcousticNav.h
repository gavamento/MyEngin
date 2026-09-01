//====================================================================================
//                          AcousticNav.h
//  MyEngine/ 秋田蓮音                                                      09/01/2026
//                                          伝播グリッドを間引いた航法グリッドと流れ場
//====================================================================================
#pragma once
#include <cstdint>
#include <vector>

#include "Engine/Engine/Acoustic/AcousticGrid.h"

namespace mye {

class AcousticField;

// 敵の道案内 (M65f、計画 hushed-rippling-beacon 判断 6)。
//
// 音響グリッドを `navCellRatio` で間引いた粗グリッドの上で、目標セルから
// **同じ整数チャンファ Dijkstra** を回して距離場 (= 流れ場) を作る。
// 敵は自分のセルで勾配を降りるだけ。
//
// ★**NavMesh を作らないのが判断の前提**。音が通れる所は敵も通れる、という一致を
//   「同じ占有配列・同じ距離の重み」から出しているので、
//   「聞こえた場所」と「そこへ行く道」が別のデータ構造に分かれることが構造的に無い。
//
// ★★**キャッシュを持たないことがこのクラスの設計の本体**。
//   LRU / 遅延構築 / 予算分割を入れると「場が間に合ったか」が敵の moveInput
//   (= ワールドハッシュ対象) を変えるので、**導出値のつもりのキャッシュが隠れた
//   sim 状態に化ける**。しかもそれは巻き戻しでだけ割れる = 最悪の型のバグになる。
//   毎 tick 全再計算なら場は (占有, 目標セル) の純関数で、履歴が存在しない。
//   重いときは**解像度を落とす方向にしか逃げない** (navCellRatio 2 -> 4)。
class AcousticNav {
public:
    // 1 tick に張れる流れ場の本数。目標が同じ敵は 1 本を共有する (セルで dedupe)。
    // 超えた敵は「場が無い」= その tick は動かない — **先着で決まるのは
    // entity.index 昇順の走査順なので決定論**
    static constexpr int kMaxFields = 4;

    // 粗グリッドを組み直す (形か占有の署名が変わったときだけ焼き直す)。
    // field にボリュームが無ければ空にして戻る
    void Sync(const AcousticField& field);

    // その tick に張った流れ場を全部捨てる。**毎 tick の先頭で必ず呼ぶこと** —
    // これを呼び忘れた瞬間にキャッシュが生まれ、上の判断が崩れる
    void BeginTick();

    // 目標のワールド座標から流れ場を 1 本張る。同じ粗セルを指す要求は同じ場を返す。
    // 戻り値: 場の index / 張れなければ -1 (グリッド外・目標が閉セル・本数超過)
    int BuildFlowField(float wx, float wy, float wz);

    // 場 index と現在位置から進む向き (水平、単位ベクトル) を得る。
    // 戻り値: 進める向きが在ったか。false = 到達不能 or 目標セルに居る
    // ★向きは**26 近傍のどれか**に量子化される。滑らかにするために距離を補間して
    //   勾配を取ると、順序ではなく値の比較に float が入り込む — この層は
    //   「順序を決めるものは全部整数」で通す (見た目のがたつきは速度で均される)
    bool SampleDirection(int field, float wx, float wy, float wz, float& outDx,
                         float& outDz) const;

    // 目標セルに十分近いか (到着判定。粗セル 1 個ぶん)
    bool ReachedTarget(int field, float wx, float wy, float wz) const;

    void Reset();
    bool Valid() const { return nav_.Valid() && !navSolid_.empty(); }
    const AcousticGridDesc& Grid() const { return nav_; }
    int FieldCount() const { return static_cast<int>(fields_.size()); }
    // 粗セルが閉じているか (グリッド外も閉扱い)
    bool IsSolid(int32_t cx, int32_t cy, int32_t cz) const;

private:
    struct Field {
        int32_t tx = 0, ty = 0, tz = 0;   // 目標の粗セル
        std::vector<uint16_t> dist;       // kUnreached = 到達不能
    };
    void BuildDistance(Field& f) const;

    AcousticGridDesc nav_;            // 粗グリッド (導出値)
    std::vector<uint8_t> navSolid_;   // 粗占有 (導出値)。1 = 閉
    AcousticGridDesc srcGrid_;        // 元にした細グリッド (形が変わったら焼き直す)
    uint64_t sourceSig_ = 0;          // 元にした AcousticField の署名 (焼き直し判定)
    int32_t sourceRatio_ = 0;
    std::vector<Field> fields_;       // **その tick 限りの導出値** (BeginTick で消える)
    mutable std::vector<std::vector<int32_t>> buckets_; // Dial 法の作業領域 (使い回し)
};

} // namespace mye
