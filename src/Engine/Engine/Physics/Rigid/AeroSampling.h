#pragma once

namespace mye {

struct ShapePose;

// ---- 面サンプリング空力カーネル (M59c) ----
// 「表面要素 (点・外向き法線・面積・その点の速度) → 力・トルクの蓄積」という 1 つの純関数に
// 空力を集約する。等方空力 (M59b) が向きを見ない代表面積 1 個で済ませていたのに対し、
// こちらは面ごとに圧力を積むので **揚力・風見安定 (weathercock) が式の別成分として自然に出る**。
//
// ★**加算順序は固定であり、並列化は永久に禁止**。float の加算は非結合なので、要素を
//   どの順で足すかが結果のビットを決める。「速いから」で並べ替えた瞬間に Debug/Release/WARP
//   のビット一致 (このリポジトリ最大の制約) が壊れる。要素列は必ず下記の固定順で回すこと。
//
// ★**三角関数を一度も呼ばない**。平板モデルの sin^2(alpha) は (n.u)^2 として、
//   sin(alpha)cos(alpha) は内積の積として出る。カプセルの方位分割だけは方向が要るが、
//   これも定数表 (kAeroAzimuth) で持つ — std::cos/std::sin は CRT 実装依存でビットが動きうる。
//
// 第 2 の呼び手として M60' の布を想定している (予約事項 3)。布は自分で SurfaceElement を
// 作って同じ AccumulateSurfaceElement へ流せばよく、カーネル側は形状を知らない。

// 表面要素 1 枚
struct SurfaceElement {
    float px = 0, py = 0, pz = 0; // 代表点 (ワールド)
    float nx = 0, ny = 1, nz = 0; // 外向き単位法線 (ワールド)
    float area = 0;               // [m^2]
    float vx = 0, vy = 0, vz = 0; // その点の速度 (ワールド。剛体なら v + omega x r)
};

// 空気の状態と平板モデルの係数
struct AeroCoeffs {
    float density = 1.225f;        // [kg/m^3]
    float windX = 0, windY = 0, windZ = 0; // 一様定常風 [m/s]
    // 風上面の圧力係数 Cn。F = Cn * rho * A * un^2。
    // **正対した平板の抗力が 1/2 rho Cd A u^2 と一致するのは Cn = Cd/2 のとき**なので、
    // 呼び出し側は Cd/2 を渡す規約にしてある。同じ Cn で球を面積分すると実効 Cd は
    // Cn (= 平板の半分) になる — 「同じ正面面積でも球は平板の半分しか抵抗しない」という
    // Newton 流モデルの帰結で、実測 (球 0.47 / 平板 1.28) の比 0.37 とも整合する
    float normalCoeff = 0.5f;
    float tangentCoeff = 0.01f; // 表面摩擦 (風上/風下を問わず接線方向に効く)
};

// 力とトルクの蓄積先
struct AeroAccum {
    float fx = 0, fy = 0, fz = 0; // 合力 [N]
    float tx = 0, ty = 0, tz = 0; // 基準点まわりの合トルク [N*m]
};

// 1 要素ぶんの空気力を acc へ積む。ref = トルクの基準点 (質量中心)。
// 完全な純関数 — 同じ入力からは常に同じビットが出る (selftest が 2 回実行で固定)
void AccumulateSurfaceElement(const SurfaceElement& e, const AeroCoeffs& c, float refX, float refY,
                              float refZ, AeroAccum& acc);

// カプセル側面の方位分割数。**定数表と対で決まっている**ので変えるときは表も差し替えること
inline constexpr int kAeroCapsuleSegments = 8;

// 形状を固定順の表面要素へ分解して acc へ積む。
//   box     : 基底順の 6 面 (+X, -X, +Y, -Y, +Z, -Z)
//   capsule : 方位 8 分割の側面 (kAeroAzimuth の順) → +Y 端 → -Y 端
//             (半球端は「面積 pi r^2 / 2 の円盤」に置き換えてある。軸方向の流れに対する
//              半球の圧力積分がちょうどその円盤と一致するため。傾いた流れでの横力は落ちる)
//   sphere  : 解析 1 発 (面積分の閉形式。等方なのでトルクは出ない)
//   mesh    : 対象外 (要素を 1 枚も出さない)
// v / omega はワールド。基準点は pose の中心 (= 現状の質量中心)
void AccumulateShapeAero(const ShapePose& pose, float vx, float vy, float vz, float wx, float wy,
                         float wz, const AeroCoeffs& c, AeroAccum& acc);

} // namespace mye
