//====================================================================================
//                          XpbdSolver.h
//  MyEngine/ 秋田蓮音                                                      08/27/2026
//                                          XPBD 距離拘束の射影 (純関数群)
//====================================================================================
#pragma once
#include <vector>

#include "Engine/Core/Ecs/EntityID.h"
#include "Engine/Engine/Physics/Rigid/Shapes.h"
#include "Engine/Engine/Physics/Xpbd/XpbdBackend.h"

namespace mye {

// XPBD の 1 池ぶんの段 (M60'c)。PhysicsSystem のサブステップループが呼ぶ:
//   剛体の速度積分の後   → Predict (重力 → 減衰 → 位置予測)
//   剛体ソルバ/位置補正の後 → Solve  (拘束射影 固定反復 → v = (x − prev)/h)
// 全て scalar float・固定反復・固定順 (決定論契約。早期終了しない)。
// compliance / damping はコンポーネントから毎 tick 読む導出値なので引数で受ける
// (池に置くと snapshot 状態が増えるだけで正本が 2 つになる)。
namespace xpbd {

// 拘束射影の固定反復回数。剛体の kSolverIterations と同じ「収束判定で早期終了しない」規約
inline constexpr int kIterations = 8;

// 終端アタッチの連成コンテキスト (M60'd)。「粒子 1 個 ↔ 剛体のアンカー点」の距離 0 拘束を
// 距離拘束と同じ反復ループに混ぜるための、剛体側の見え方 (Body そのものは Renderer 層規則
// と同じ理屈で持ち込まない — PhysicsSystem.cpp のファイルローカル型なので参照できない)。
//
// ★剛体側の重みは Macklin 系の実効逆質量 w(n) = invMass + (r×n)·I⁻¹(r×n)。
//   M60a の ConstraintBlock (K⁻¹ ブロック) を流用しない理由: あれは**速度レベル**の拘束で
//   方向と K⁻¹ を生成時に固定する前提 + 解かれる場所が剛体速度ソルバ内 (粒子が居ない)。
//   XPBD は反復ごとに法線を引き直すので、スカラーの w(n) をその場で組むのが正しい形
//   (式自体は FinalizeConstraintBlock の対角項 i==j と同じ)。
// ★腕 r は反復中固定 (「K は生成時に 1 回」の家風と同じ線形化)。アンカー位置だけは
//   蓄積補正 outD + outT×r で追従させる — これをしないと反復 2 週目以降が
//   「もう動かした剛体」を見ずに同じ補正を重ね掛けする。
// ★ax/ay/az は**このサブステップの位置積分後**を呼び側が先取りした予測位置で渡す
//   (剛体の位置積分は XPBD の後段にあるため。現在位置で渡すと連成が 1 サブステップ
//   遅れて余計な伸びが見える)。眠り/静的/kinematic は invMass=0 + invI 零行列 = 不動ピン。
// ★出力 outD/outT は「COM の並進量 / 回転ベクトル」。呼び側が v += outD/h・ω += outT/h で
//   速度へ返す (位置そのものへは書かない — 後段の位置積分が補正済み速度で動かすので、
//   両方に書くと二重適用になる)
struct AttachContext {
    uint32_t particle = 0;                    // 池内の粒子 index (Rope は末尾)
    float ax = 0.0f, ay = 0.0f, az = 0.0f;    // アンカーの予測ワールド位置
    float rx = 0.0f, ry = 0.0f, rz = 0.0f;    // COM(予測) → アンカー(予測) のワールド腕
    float invMass = 0.0f;                     // 剛体の線形逆質量
    float invI[3][3] = {};                    // 剛体のワールド逆慣性 (freezeRot は零行列)
    float outDx = 0.0f, outDy = 0.0f, outDz = 0.0f; // 蓄積した COM 並進補正
    float outTx = 0.0f, outTy = 0.0f, outTz = 0.0f; // 蓄積した回転ベクトル
    // アタッチ行が**粒子側**へ適用した補正量の総和 Σ(w_p·|dλ|) [m]。眠った剛体の起床判定
    // (PhysicsSystem) 用 — アタッチ行は各反復の末尾なので「反復前の違反量」は前サブステップ
    // 末の吸い付きでほぼ 0 に戻っており信号にならない。実際に働いた補正の総和だけが
    // 「ロープが引いている」を運ぶ (吊るしただけなら g·h² 規模で収まる)
    float outAbsCorr = 0.0f;
};

// ---- 粒子 vs 世界の衝突 (M60'e、片方向) ----
// 粒子を半径 r の球として、剛体/静的コライダーの形状から押し出す。剛体側は**不動**として扱う
// (力を返すのは M60'f)。判定は shapes::Collide (球 vs 全 6 形状) をそのまま使う —
// 計画にあった Shapes.cpp 内部ルーチン (SphereTriContact 等) の公開は、Collide が球 vs
// メッシュ/地形/凸包を既に解く (メッシュ/地形は最深の三角形 1 枚) ので不要だった。
//
// ★接触はサブステップ冒頭 (位置予測の直後) に 1 回だけ集め、反復中は「平面 n·x ≥ planeD」
//   として射影する (PBD の標準形)。反復中に形状判定を引き直さない = コストが粒子数 × 反復に
//   比例しない + 反復ごとに法線が飛ばない。予測位置では離れていても反復中に距離拘束が押し込む
//   ぶんは kContactMargin の先読みで拾う (平面が非活性 C ≥ 0 なら何もしない)。
// ★摩擦は履歴を持たない位置ベース (Macklin 2014 Unified Particle Physics の式)。
//   このサブステップの接線変位を、法線方向に押した量 × μ の範囲で打ち消す。アンカー履歴を
//   持たないので sim 状態は増えない (計画のリスク 4)。

// 接触候補を拾う先読み距離 [m]。予測位置で r + これ以内なら平面を作る
inline constexpr float kContactMargin = 0.02f;

// 世界側の形状 1 個 (PhysicsSystem がサブステップごとに剛体から組む。Body はファイルローカル型
// なので形状だけを渡す — AttachContext と同じ理屈)
struct WorldShape {
    ShapePose pose;
    float minX = 0.0f, minY = 0.0f, minZ = 0.0f; // shapes::ComputeAabb (候補の絞り込み用)
    float maxX = 0.0f, maxY = 0.0f, maxZ = 0.0f;
    float muS = 0.0f; // 静止摩擦係数 (矯正済み)
    float muK = 0.0f; // 動摩擦係数 (矯正済み)
    EntityID entity = kNullEntity; // 持ち主の剛体 (池ごとの除外判定に使う)
};

// 粒子 1 個 ↔ 形状 1 個の接触平面。サブステップ内のスクラッチで、sim 状態ではない
// (毎サブステップ位置から純関数で作り直す)
struct ParticleContact {
    uint32_t particle = 0;
    float nx = 0.0f, ny = 1.0f, nz = 0.0f; // 形状 → 粒子 (粒子を押し出す向き)
    float planeD = 0.0f;                   // 拘束 C = n·x − planeD ≥ 0
    float muS = 0.0f, muK = 0.0f;
    float pushed = 0.0f; // 反復中に法線方向へ押した総量 [m] (摩擦の上限 μ·pushed に使う)
};

// 池の (ピン以外の) 粒子と shapes の接触平面を out へ集める。順序は (粒子 index, 形状 index)
// の昇順 = 明示キーの固定順 (決定論)。exclude0/1 の剛体とは当たらない (池の持ち主と
// アタッチ先。ロープがぶら下がっている箱そのものに押し出されて暴れるのを防ぐ —
// 関節の disableCollision と同じ理屈)。radius は NaN/負を 0 へ矯正して使う
void CollectContacts(const XpbdBackend::Pool& pool, float radius,
                     const std::vector<WorldShape>& shapes, EntityID exclude0, EntityID exclude1,
                     std::vector<ParticleContact>& out);

// 重力 → (最初のサブステップのみ) 減衰 → prev 退避 → 位置予測。
// damping は**毎 tick の率** (Rigidbody.linearDamping と同じ) なので applyDamping は
// sub == 0 だけ true にする — サブステップごとに掛けると N 乗になる (剛体と同じ罠)
void Predict(XpbdBackend::Pool& pool, float gx, float gy, float gz, float h, bool applyDamping,
             float damping);

// 距離拘束を λ 蓄積つきで固定反復射影し、v = (x − prev)/h で速度を確定する。
// α̃ = compliance/h² (XPBD)。lambdaScratch は呼び出し側が使い回すスクラッチ
// (サブステップ冒頭で 0 リセットされる。sim 状態ではない)。
// attach (M60'd) は反復ごとに**距離拘束の後**で解く (鎖の末尾行 = 固定順)。
// アタッチ自体の compliance は常に 0 (剛結合) — 弾ませたいなら鎖側の compliance で作る。
// contacts (M60'e) は各反復の**最後** (アタッチの後) に射影する — 貫通しないことを最優先にする
// ため。反復の後、速度確定の前に摩擦を掛ける。null / 空なら従来とビット同一
void Solve(XpbdBackend::Pool& pool, float compliance, float h, std::vector<float>& lambdaScratch,
           AttachContext* attach = nullptr, std::vector<ParticleContact>* contacts = nullptr);

} // namespace xpbd

} // namespace mye
