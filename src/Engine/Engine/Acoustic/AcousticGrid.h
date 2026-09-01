//====================================================================================
//                          AcousticGrid.h
//  MyEngine/ 秋田蓮音                                                      09/01/2026
//                                          音響ボクセルグリッドの座標変換とチャンファ重み（純関数のみ）
//====================================================================================
#pragma once
#include <cmath>
#include <cstdint>

namespace mye {

// 音響ボクセルグリッドの寸法と原点 (M65a、計画 hushed-rippling-beacon)。
//
// ★**次元は整数で持ち、範囲 (extent) を導出値にする**。逆に「half extent とセルサイズから
//   次元を割り出す」形にすると **float -> int の丸めがグリッド形状そのもの**になり、
//   丸めが 1 変わるだけで波の到達セルが全部ずれる。整数で持てばそのクラスの事故が
//   構造的に起きない。
// ★グリッドは常に軸平行 — ボリュームの WorldMatrix の**回転は無視する**。斜めグリッドは
//   セル走査も残光の再アドレスも一気に難しくなるので v1 の範囲外 (計画「実装しない」)。
struct AcousticGridDesc {
    int32_t dimX = 0, dimY = 0, dimZ = 0;
    float cellSize = 0.5f;
    // セル (0,0,0) の**最小角**のワールド座標 (中心ではない)。
    // 中心にすると WorldToCell が半セルぶんの加算を毎回背負うので、最小角に倒してある
    float minX = 0.0f, minY = 0.0f, minZ = 0.0f;

    bool Valid() const
    {
        return dimX > 0 && dimY > 0 && dimZ > 0 && cellSize > 0.0f;
    }
    // int64 で返すのは 256^3 = 16.7M が int32 の乗算で溢れないことを型で示すため
    int64_t CellCount() const
    {
        return static_cast<int64_t>(dimX) * static_cast<int64_t>(dimY)
            * static_cast<int64_t>(dimZ);
    }
};

namespace acoustic {

// ---- チャンファ距離の重み (Borgefors <11,16,19>) ----
// 6 近傍 BFS の等距離面は菱形 (L1)、26 近傍の等コスト BFS は立方体 (L∞) にしかならず、
// どちらも「音の波」に見えない (どちらも球から 40% 級に外れる)。面/辺/角に 11/16/19 を
// 配ると、**最適スケールを取ったときの**最大相対誤差が 1.6% に収まる。
// ★「16/11 と sqrt(2) を比べて 2.9% ずれている」は誤読 — チャンファ距離は「真の距離の
//   s 倍」の近似で、s は自由に選べる (実測は AcousticSelfTest (2))。
//   ChamferToMeters は s = 11 (面方向が厳密) を採る。到達距離 [m] が軸方向でぴったりに
//   なるオーサリングの分かりやすさを優先し、対角が 2.9% 長く出るのは許容する取引。
//
// ★**整数であること自体が決定論の核心**。物理の float は 1 ulp ずれても剛体が微動する
//   だけだが、伝播は**順序比較が 1 ulp ずれると訪問順が入れ替わり、親リンクが変わり、
//   AI が聞く方向が変わる**。この層の規約は「順序を決めるものは全部整数、float は
//   整数から導く末端の 1 式 (ChamferToMeters) だけ」。
inline constexpr uint16_t kFaceCost = 11;
inline constexpr uint16_t kEdgeCost = 16;
inline constexpr uint16_t kCornerCost = 19;

// 26 近傍のオフセットと重み。
// ★**並び順は決定論のタイブレークそのもの**なので変更・並べ替え禁止。
//   同コストのセルが複数の親候補を持つとき、先に visit した側が親になる
struct Neighbor {
    int8_t dx, dy, dz;
    uint16_t cost;
};
inline constexpr int kNeighborCount = 26;
extern const Neighbor kNeighbors[kNeighborCount];

// 逆方向の近傍 index。kNeighbors は (dz,dy,dx) の辞書順で中心だけを抜いた並びなので、
// 27 セル並びの「点対称 = 26 - k」がそのまま「25 - i」に落ちる (表を持つ必要がない)。
// parentDir から音源方向を復元するときに使う
inline constexpr uint8_t OppositeNeighbor(uint8_t i)
{
    return static_cast<uint8_t>(kNeighborCount - 1 - i);
}

// グリッドの上限。ここを超える設定は Sync がクランプして警告を出す。
// 4M セルで占有 4MB / 残光 4MB — WARP でも毎 tick 触れる現実的な上限として置いた
inline constexpr int32_t kMaxDim = 256;
inline constexpr int64_t kMaxCells = 4 * 1024 * 1024;

// セル座標がグリッド内か
inline bool InBounds(const AcousticGridDesc& g, int32_t cx, int32_t cy, int32_t cz)
{
    return cx >= 0 && cy >= 0 && cz >= 0 && cx < g.dimX && cy < g.dimY && cz < g.dimZ;
}

// セル座標 -> 線形 index。x が最内、次に y、最後に z (froxel の読み戻しと同じ並び)。
// **InBounds を通してから呼ぶこと** (範囲検査はしない)
inline int64_t CellIndex(const AcousticGridDesc& g, int32_t cx, int32_t cy, int32_t cz)
{
    return (static_cast<int64_t>(cz) * g.dimY + cy) * g.dimX + cx;
}

// ワールド座標 -> セル座標。グリッド外なら false (out は書かない)。
// 境界は「最小角を含み、最大角を含まない」半開区間
bool WorldToCell(const AcousticGridDesc& g, float x, float y, float z, int32_t& outX,
                 int32_t& outY, int32_t& outZ);

// セル座標 -> セル中心のワールド座標
void CellToWorldCenter(const AcousticGridDesc& g, int32_t cx, int32_t cy, int32_t cz, float& outX,
                       float& outY, float& outZ);

// チャンファ距離 -> メートル。**float が出てくる唯一の式**
inline float ChamferToMeters(uint32_t dist, float cellSize)
{
    return static_cast<float>(dist) * cellSize / static_cast<float>(kFaceCost);
}

// 到達エネルギー (M65b)。
// ★**整数チャンファ距離の純関数**であることがこの設計全体の土台。伝播中に材質で
//   減衰させると経路依存になり、波の全状態がセル配列に落ちて snapshot が数 MB になる
//   (計画 判断 3)。床材は「発音時の振幅と到達上限」にだけ効かせる。
// ★減衰式は Audio/SpatialMath.h の RolloffGain (逆二乗) をそのまま借りる —
//   **聞こえる音の減衰と見える波の減衰が同じ式**になるので、M65c 以降で実際に鳴らしても
//   絵と音がずれない。d >= maxD で厳密に 0 を返す性質もそのまま欲しい (境界でポップしない)。
float EnergyAt(uint32_t chamferDist, uint32_t maxChamferDist, float amplitude, float cellSize);

// ---- 衝撃音の強さ (M65c) ----
// 接触の法線インパルス [N*s] から発音の倍率 [0,1] を作る純関数。
// ★引数は「**載っているだけの力積を差し引いた残り**」であること (差し引きは呼び出し側)。
//   素の力積で閾値を切ると、重い箱が静止しているだけで鳴り続ける = 常時音源になる。
inline constexpr float kImpactRefImpulse = 6.0f;    // 倍率 1.0 に達する力積 (1.5kg を 1m 落下)
inline constexpr float kImpactMinImpulse = 0.35f;   // これ以下は擦り扱いで鳴らさない
inline constexpr float kImpactRestingMargin = 2.0f; // 静止支持ぶんの何倍を超えたら「当たった」
inline float ImpactGain(float excessImpulse)
{
    if (excessImpulse <= kImpactMinImpulse) {
        return 0.0f; // 境界で厳密に 0 — 微小な擦りが 1 リングの波を毎 tick 立てるのを防ぐ
    }
    const float g = excessImpulse / kImpactRefImpulse;
    return (g < 1.0f) ? g : 1.0f;
}

// ---- 残光の符号化 (M65d) ----
//
// 残光ボリュームは **R8_UNORM の 3D テクスチャ** (1 セル 1 バイト)。
// ★エネルギーをそのまま線形に uint8 へ落とすと**数メートル先で全部 0 になる** —
//   EnergyAt は逆二乗なので 5m で 1/100、20m で 1/1600 まで落ちる。企画 §3-1 の
//   「波が部屋の壁を描く」は遠い壁も薄く光ることが前提なので、**格納時にガンマを掛けて**
//   ダイナミックレンジを稼ぐ (sRGB テクスチャと同じ発想)。
// ★ガンマは 1/4 ちょうどで、実装は **sqrt を 2 回**。pow(x, 0.25f) にしないのは、
//   sqrt が IEEE-754 で正しく丸められる (= コンパイラや CRT に依らない) 唯一の
//   超越風関数だから。残光はハッシュ対象外だが、M65e で golden スクショに載るので
//   Debug/Release/WARP で同じバイトが出る形にしておく。
// 復号 (シェーダ側) は v^4 = (v*v)*(v*v)。M65e の acoustic_common.hlsli がこれを持つ。
inline constexpr float kGlowGamma = 0.25f;

inline uint8_t EncodeGlow(float energy)
{
    if (!(energy > 0.0f)) {
        return 0; // NaN もここで 0 に落ちる
    }
    const float e = (energy < 1.0f) ? energy : 1.0f;
    const float g = std::sqrt(std::sqrt(e));
    const int v = static_cast<int>(g * 255.0f + 0.5f);
    return static_cast<uint8_t>((v < 0) ? 0 : ((v > 255) ? 255 : v));
}

inline float DecodeGlow(uint8_t v)
{
    const float t = static_cast<float>(v) / 255.0f;
    const float t2 = t * t;
    return t2 * t2;
}

// 残光が 1 tick に残る割合。255 から 0 まで **227 tick = 約 3.8 秒** (実測)。
// 企画 §3-5 の「記憶の地図」= 直前に通った道が薄く残っている、の時定数。
// ★uint8 の乗算は切り捨てなので、割合が 1 未満なら必ず単調減少して 0 に届く
//   (255 -> ... -> 1 -> 0。「途中で止まる」ことが構造的に無い)。
// ★裏返すと**残光は 255 tick (4.25 秒) より長くは持てない** — 切り捨ては
//   「1 tick に最低 1 は減る」を意味するので、割合をいくら 1 に近づけても
//   末尾が線形になる。もっと長い残光が要るなら uint8 をやめるしかない
//   (R16_UNORM にするか、減衰を数 tick に 1 回へ間引くか)。
inline constexpr float kGlowDecayPerTick = 0.995f;

// 波 1 本が使う局所ボックスの半径 [セル]。到達距離の上限 = R * cellSize [m]
// (64 * 0.5 = 32m。企画の「金属板は部屋を突き抜ける」が成り立つ長さ)。
// ★メモリは **min((2R+1)^3, グリッド全体) * 3B / 波**。ボックスはグリッドで
//   クリップされるので、既定ボリューム (64x16x64 = 65,536 セル) なら 196KB/波 =
//   16 本で 3.1MB。規格上限のグリッド (kMaxCells = 4M) を 64 リングの波で 16 本
//   同時に満たすと 192MB になるが、その構成は**伝播コストのほうが先に破綻する**ので
//   実務上の上限は cellSize と dim が決めている。
// 縮退はこの値ではなく ticksPerRing / cellSize から先に触ること (計画 M65b のリスク)
inline constexpr uint32_t kMaxWaveRing = 64;

// 中心とセル数から desc を組む (ボリュームの WorldMatrix の平行移動が center)。
// dim は 1..kMaxDim にクランプし、総セル数が kMaxCells を超えたら**均等に間引く**のではなく
// 呼び出し側へ false を返す — 黙って解像度を変えると「シーンによって波の形が違う」に化ける
bool MakeGridDesc(int32_t dimX, int32_t dimY, int32_t dimZ, float cellSize, float centerX,
                  float centerY, float centerZ, AcousticGridDesc& out);

// 2 つの desc が同一か (Sync が「前回と同じグリッドか」を見るためだけの比較)
bool SameGrid(const AcousticGridDesc& a, const AcousticGridDesc& b);

} // namespace acoustic
} // namespace mye
