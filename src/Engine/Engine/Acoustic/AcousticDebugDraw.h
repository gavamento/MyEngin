//====================================================================================
//                          AcousticDebugDraw.h
//  MyEngine/ 秋田蓮音                                                      09/01/2026
//                                          音響の場のデバッグ可視化（波面／占有／音源／聴者）
//====================================================================================
#pragma once
#include <vector>

namespace mye {

class World;
class AcousticField;
struct DebugLineCmd;

// ---- 音響デバッグ可視化 (M65b) ----
// **`Physics\PhysicsDebugDraw.h` と同じ型**: World と場を読むだけ / 出力は debugLines への
// 一方通行 / トグルはエディタ全体で 1 個のグローバル。
//
// ★**トグルも出力も sim 状態ではない**。立てても倒しても RunOneTick の結果は 1 ビットも
//   変わらない。だから記録中に線を出しても .rep は割れないし、既定 off なので
//   既存のスクショ回帰にも一切出てこない。
//
// M65d で残光ボリュームが GPU に上がるまでは、**波が壁を貫通せず角を曲がったことを
// 人間が確かめる唯一の手段がこれ**。だから伝播と同じサブで入れてある。
struct AcousticDebugFlags {
    // 直近に確定したリング (= 今の波面) のセル。各セルから**親の方角へ**短い線を引くので、
    // 「どこから回り込んできたか」が線の向きとして読める
    bool frontier = false;
    // 占有セルのうち**開セルに面しているものだけ**十字で描く。全部描くと箱が塗り潰される
    bool occupancy = false;
    bool waveOrigin = false; // 音源セルの大きい十字 (音色で色分け)
    bool listener = false;   // 聴者の十字 + 最後に聞いた位置への線 (M65f で中身が入る)
    // ボリュームの AABB ワイヤ (M65h)。「高さが壁より 0.5m 高いだけで波が壁を
    // 飛び越える」罠 (M65b 申し送り 1) を目視で防ぐための 12 本
    bool bounds = false;
    bool Any() const { return frontier || occupancy || waveOrigin || listener || bounds; }
};

AcousticDebugFlags& GetAcousticDebugFlags();

// 線を out へ**追記**する (クリアは呼び出し側 = TickRunner が tick 頭で 1 回)。
// field が空 (ボリュームが無い) なら何もしない。
// ★出力量は cap で頭打ちにしてある — 128x32x128 の占有を素直に全部描くと
//   1 フレームで 150 万本積んで描画側が死ぬ
void BuildAcousticDebugLines(World& world, const AcousticField& field,
                             const AcousticDebugFlags& flags, std::vector<DebugLineCmd>& out);

} // namespace mye
