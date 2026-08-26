#pragma once
#include <vector>

namespace mye {

class World;
class XpbdBackend;
struct SolidContact;
struct DebugLineCmd;

// ---- 物理デバッグ可視化 (M59e) ----
// 接触点・接触法線・法線インパルスの強さ・剛体の速度を線として吐く。
// 出力先は EngineLoop の debugLines なので、**SceneView だけでなく GameView にも出る**
// (スクリプトの DebugDrawLine と同じレーンに相乗りしている)。
//
// ★**トグルも出力も sim 状態ではない**。収集は World を読むだけ、出力は debugLines への
//   一方通行で、立てても倒しても RunOneTick の結果は 1 ビットも変わらない
//   (PhysicsSelfTest が ON/OFF の並走ハッシュ一致で常時固定する)。だからこそ
//   エディタ全体で 1 個のグローバルに置ける (CameraPilotState と同じ流儀)。
struct PhysicsDebugFlags {
    bool contacts = false;   // 接触点の十字 + 法線
    bool impulses = false;   // 法線インパルスの強さを法線の長さに乗せる (contacts と併用)
    bool velocities = false; // 剛体の速度ベクトル
    // M60a: 関節のアンカー 2 点 + それを結ぶ「ずれ」の線 + 軸。**アンカーがどこで軸が
    // どっちを向いているかが見えないと関節のデバッグは成立しない**ので、拘束ソルバと
    // 同じサブで入れてある (M59 が可視化を面空力より前に置いて正解だったのと同じ判断)。
    // ★これも他の 2 つと同じで**線が積まれるのは Play 中だけ** (積むのは tick 側なので)。
    //   編集中の authoring 用には SceneViewWindow のギズモ (SpringJoint と同じ棚) が別にある
    bool joints = false;
    // M60'c: 変形体 (ロープ等)。粒子を結ぶ拘束の線 + ピン留め粒子の十字。
    // 粒子は WorldMatrix を持たないので池 (XpbdBackend) を直接読む — これも読むだけ
    bool deform = false;
    bool Any() const { return contacts || velocities || joints || deform; }
};

PhysicsDebugFlags& GetPhysicsDebugFlags();

// contacts (key 昇順) と剛体から線を out へ**追記**する。out のクリアは呼び出し側
// (TickRunner が tick 頭で 1 回)。World は読むだけ。
// 呼ぶ場所は TransformSystem の後 — 速度ベクトルの根元に当 tick のワールド位置が要るため
// (接触点は SolidContact 自身がワールド座標を持っているのでどちらでもよい)。
// xpbd (M60'c): deform フラグの線源。null なら変形体は描かない
void BuildPhysicsDebugLines(World& world, const std::vector<SolidContact>& contacts,
                            const PhysicsDebugFlags& flags, std::vector<DebugLineCmd>& out,
                            const XpbdBackend* xpbd = nullptr);

} // namespace mye
