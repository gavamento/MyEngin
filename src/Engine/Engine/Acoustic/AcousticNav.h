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
//
// ★★**敵は登らない** (2026-09-13)。グリッドは立体なので、書架や閉じた扉の**上の空いた層**も
//   「開」になる。音はそこを越えてよいが、敵 (CharacterController) は越えられない。以前は
//   流れ場が上の層を通って障害物を越える道を張り、SampleDirection が返す水平成分だけを
//   受け取った敵が書架の側面を押し続けた (三校 stage2 で実測: moveInput 3m/s・速度 0 のまま)。
//   そこで流れ場の辺を「同じ層か下の層へ」に限る。落ちる (下りる) のは重力が運ぶので許す。
//   空中の目標は BuildFlowField が真下の床の層まで落とす。
//   「音が通れる所は敵も通れる」は**同じ層の中では**そのまま成り立つ
class AcousticNav {
public:
    // 同じ目標セルの要求は共有する。異なる目標はすべて同期的に処理し、
    // 要求順や過去の予算消費によって後続の敵を停止させない。

    // 粗グリッドを組み直す (形か占有の署名が変わったときだけ焼き直す)。
    // field にボリュームが無ければ空にして戻る
    void Sync(const AcousticField& field);

    // その tick に張った流れ場を全部捨てる。**毎 tick の先頭で必ず呼ぶこと** —
    // これを呼び忘れた瞬間にキャッシュが生まれ、上の判断が崩れる
    void BeginTick();
    // tick 限定の通行禁止円。場を構築する前に登録する (音響の占有は変更しない)。
    void ExcludeCircle(float x, float z, float radius);

    // 目標のワールド座標から流れ場を 1 本張る。同じ粗セルを指す要求は同じ場を返す。
    // 目標が空中 (真下が開) なら、真下が閉じるまで層を落としてから張る = 音源の足元へ向かう。
    // 戻り値: 場の index / 張れなければ -1 (グリッド外・開セルへ寄せられない目標)
    int BuildFlowField(float wx, float wy, float wz);

    // 場 index と現在位置から進む向き (水平、単位ベクトル) を得る。
    // 戻り値: 進める向きが在ったか。false = 到達不能 or 目標セルに居る
    // ★上の層の隣は選ばない (登らない)。
    // ★進む隣は**26 近傍のどれか**を整数の比較で選ぶ。滑らかにするために距離を補間して
    //   勾配を取ると、順序ではなく値の比較に float が入り込む — この層は
    //   「順序を決めるものは全部整数」で通す。返す向きは「今の位置から選んだ隣のセル中心へ」
    //   (2026-09-13。隣の向きそのままだと、角をかすめて障害物の面に接した敵が面へ直角に押し続ける)
    bool SampleDirection(int field, float wx, float wy, float wz, float& outDx,
                         float& outDz) const;

    // 目標セルに十分近いか (到着判定。粗セル 1 個ぶん)
    bool ReachedTarget(int field, float wx, float wy, float wz) const;

    // 場 field の目標へ (wx,wy,wz) から辿れないとき、**そこから辿れる**開セルのうち
    // 目標セルに最も近いものの中心を返す (光で入口を塞がれた部屋の中の目標など)。
    // 戻り値: 差し替え先を返したか。false = 既に辿れる / 自分のセルが解決できない / 場が無効
    // ★近さはセル座標の整数 2 乗距離、同点は走査順 (z, y, x 昇順) で先に見つかったほう
    bool NearestReachable(int field, float wx, float wy, float wz, float& outX, float& outY,
                          float& outZ) const;

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
    // towardTarget = true: 「各セルから目標まで」の場 (辺は目標側が同じか下の層)。
    // false: 「目標セル (= 出発点) から各セルまで」の場 (辺は行き先が同じか下の層)。
    // ★登らない規則で辺に向きが付いたので、2 つは同じ場にならない
    void BuildDistance(Field& f, bool towardTarget) const;
    // 位置 -> 自分の粗セル。閉セルなら開いている隣へ**表の順**に逃がす (見つからなければ false)
    bool ResolveCell(float wx, float wy, float wz, int32_t& cx, int32_t& cy, int32_t& cz) const;

    AcousticGridDesc nav_;            // 粗グリッド (導出値)
    std::vector<uint8_t> navSolid_;   // 粗占有 (導出値)。1 = 閉
    std::vector<uint8_t> excluded_;
    AcousticGridDesc srcGrid_;        // 元にした細グリッド (形が変わったら焼き直す)
    uint64_t sourceSig_ = 0;          // 元にした AcousticField の署名 (焼き直し判定)
    int32_t sourceRatio_ = 0;
    std::vector<Field> fields_;       // **その tick 限りの導出値** (BeginTick で消える)
    mutable std::vector<std::vector<int32_t>> buckets_; // Dial 法の作業領域 (使い回し)
};

} // namespace mye
