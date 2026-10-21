//====================================================================================
//                          Line2DGeometry.h
//  MyEngin/ 秋田蓮音                                                       10/22/2026
//                                          Line2D の帯 (ストローク) と塗りの頂点生成 (純関数)
//====================================================================================
#pragma once
#include <cstdint>
#include <vector>

#include <DirectXMath.h>

namespace mye {
namespace line2d {

// 帯の継ぎ目で、マイター (角を尖らせる) の長さが半幅の何倍までなら許すか。超える鋭い角はベベル (角を切る)
inline constexpr float kMiterLimit = 2.0f;

// 帯の頂点。x / y はローカル座標。u = 線に沿った 0..1 (折れ線全体の長さに対する割合)、v = 左 0 .. 右 1
struct StrokeVertex {
    float x;
    float y;
    float u;
    float v;
};

// points[0..count) を幅 width の帯にした三角形リスト (3 頂点 1 組) を out に作る。
// 線分ごとに四角形 (6 頂点) で、継ぎ目の左右の点は前後の線分が共有する (マイター)。
// ベベルのときは線分ごとの法線で別の点を持ち、外側の隙間に三角形 (3 頂点) を足す。端は平ら。
// 同じ位置が続く点は 1 つにまとめる。loop は 3 点以上で最後と最初をつなぐ (2 点以下は開いた線として扱う)。
// 線分が 1 本も残らない (count < 2、全点が同位置) / width <= 0 は out が空
void BuildStroke(const DirectX::XMFLOAT2* points, int count, float width, bool loop, std::vector<StrokeVertex>& out);

// points[0..count) を閉じた多角形として三角形に割る (耳刈り)。out は points への添字 3 つ 1 組。
// 時計回り / 反時計回りのどちらでもよい。自己交差は未定義 (入力側の責務)。count < 3 は out が空。
// 実体は Physics の TriangulatePolygon2D (Freeform ライトと同じ)
void Triangulate(const DirectX::XMFLOAT2* points, int count, std::vector<uint32_t>& out);

} // namespace line2d
} // namespace mye
