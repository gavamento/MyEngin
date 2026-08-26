#pragma once
#include <cstdint>

#include "Engine/Core/EntityID.h"

namespace mye {

class Scene;
struct SkinnedModel;

// ---- ラグドール生成器 (M60g2) ----
// **sim には 1 バイトも足さない**。エディタ時に 1 回きり、スケルトンの形から
// 「M60g1 が駆動できる階層」を組み立てるだけの器。
//
// ★M60i で `src\Editor\` から Engine 層へ移した。ショーケース (`--joint-demo`) の
//   ラグドールを `DemoContent` が組むため — Editor 層に置いたままだと **Runtime.exe が
//   同じシーンを組めない** (golden スクショは Runtime で撮る)。中身は元から Engine 層の
//   型しか触っていなかったので、移動は include の付け替えだけで済んでいる。
//   これで「生成器が吐く階層」そのものが replay 6 ペア目の被写体になった。
//
//   Skin (SkinnedMesh + Ragdoll(active=false))
//    ├ <骨名>            Part(joint) / PartBounds / Rigidbody(compoundColliders) / Joint(Cone)
//    │  └ <骨名>_Shape   Collider(capsule)
//    └ ...
//
// ★**カプセルが 2 段になっているのは `ColliderComponent` が center も rotation も
//   持たないから**。形状はエンティティ原点中心・ローカル Y 軸に固定で、部位側の
//   LocalTransform は `PartFollowSystem` の持ち物 (== jointGlobal) なので回せない。
//   そこで M60e の複合コライダー (`Rigidbody.compoundColliders`) で子の形状を吸わせ、
//   「骨の中点へ平行移動 + Y を骨方向へ回す」を子の LocalTransform に持たせている。
//   こうすると骨が +Y を向いていない rig (Blender 系以外) でもカプセルが骨に乗る。
//
// ★**同じスケルトンからは毎回ビット同一の階層が出ること**が検証条件 (g2-1)。そのため
//   骨の走査は joints 配列の index 昇順に固定、子が複数ある骨は **index 最小の子**へ
//   伸ばす、と決め打ちしてある。浮動小数も式の形を変えずに 1 本道で書いている。
//
// ★部位の LocalTransform は `DecomposeRowMajorTRS` (PartFollowSystem と共有) で作る。
//   追従システムがバインドポーズで書く値とビット一致していないと、Play を押した瞬間に
//   ラグドールがカクッと飛ぶ。
namespace ragdoll_build {

struct Options {
    // カプセル半径 = 骨長 × これ。0.5 を超えると球に潰れる (害は無い)
    float radiusRatio = 0.22f;
    // カプセル全高 = 骨長 × これ。骨の**中央**に置くので、両端の関節に隙間が空く。
    // ★**1.0 にしてはいけない**。隣り合う骨は関節点を共有しているので、カプセルを骨の
    //   端から端まで張ると両者が半径ぶん必ず食い込み、接触ソルバが毎 tick 押し返して
    //   ラグドールが永久に眠らない (実測: 3000 tick 回しても 1 本も入眠しなかった)。
    //   **関節で繋がった相手との衝突を切る仕組みは v1 のソルバに無い**ので、
    //   ここは幾何で離すのが唯一の手段。
    float lengthRatio = 0.8f;
    // これ未満の長さしか取れない骨は部位を作らない (長さ 0 の骨にカプセルは張れない)。
    // 飛ばされた骨は Joint が「部位を持つ最も近い祖先」へ直接繋がる
    float minBoneLength = 1.0e-4f;
    // **その rig の最長骨に対してこの比より短い骨は部位にしない**。人体 rig なら
    // 手・足・指がここで落ちる (Unity のラグドールウィザードが手足の先を作らないのと同じ)。
    // ★見た目の割り切りではなく**必須**: 極端に細く軽い骨は「関節に吊られながら床に
    //   触れている」状態で永久に微振動し、島の全員が静まるまで眠らない仕組みのせいで
    //   **ラグドール全体が一生眠らなくなる**。関節を外しても接触を切っても止まるので、
    //   接触と関節が同じ低慣性ボディで噛み合ったときだけ出る (実測: 他の 6 本が
    //   sleepTicks 30/30 に達しているのに、最短の骨だけ 1〜7 で叩き落とされ続ける)。
    // 落ちた骨は描画では親の骨に固定されて付いてくる (パレットが階層合成で埋める)
    float minBoneRatio = 0.25f;
    // 質量 = カプセル体積 × これ [kg/m^3]。既定は水と同じ。**一律 1kg にしない**のは
    // 指と胴が同じ重さのラグドールが目に見えて不自然に暴れるため
    float density = 1000.0f;
    // 半径の下限 = **その rig の最長骨** × これ。★短い骨 (手・足・指) をそのまま細く
    //   作ると慣性が桁で小さくなり、「関節に吊られながら床に触れている」状態で
    //   永久に微振動して**ラグドール全体が眠れなくなる** (島は全員が静まるまで眠らない)。
    //   関節だけ外しても接触だけ外しても止まるので、この 2 つが噛み合ったときだけ出る。
    //   絶対値ではなく rig の大きさに対する比なのは、cm 単位のモデルでも効かせるため
    float minRadiusRatio = 0.10f;
    float swingLimitDeg = 45.0f; // Cone の円錐半頂角
    float twistLimitDeg = 30.0f; // Cone のツイスト範囲 (±)
    // 生成する剛体の減衰。既定 (0.0 / 0.05) より強くする — ラグドールは「暴れずに
    // 早く落ち着く」ほうが望ましく、残留スピンを引きずると入眠しない
    float linearDamping = 0.02f;
    float angularDamping = 0.20f;
};

// skin (SkinnedMeshComponent 持ち) の下にラグドール階層を生成し、作った部位の本数を返す。
// 0 = 何も作らなかった (骨が無い / 名前が空 or 64 バイト以上 / 長さが取れない)。
// **既に Ragdoll を持っていても部位は重ねて足す** — 消すのは呼び側 (Undo) の仕事。
int Build(Scene& scene, EntityID skin, const SkinnedModel& model, const Options& opt = Options{});

// 生成される部位の本数を先に数える (メニュー項目の disable 判定用)。
// `Build` とまったく同じ選別を通すので、「押せたのに何も出ない」が起きない
int CountParts(const SkinnedModel& model, const Options& opt = Options{});

} // namespace ragdoll_build
} // namespace mye
