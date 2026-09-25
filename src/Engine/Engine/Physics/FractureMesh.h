//====================================================================================
//                          FractureMesh.h
//  MyEngin/ 秋田蓮音                                                     09/25/2026
//                                          破壊分割コア: 閉じ判定と平面切断・断面の蓋
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <DirectXMath.h>

namespace mye {

// ---- 破壊分割コア (M80a) ----
// UE の Chaos Destruction 相当の破壊物理のうち、最もリスクの高い未知
// 「閉じたメッシュを平面で切って両側を閉じたメッシュとして取り出せるか」だけを
// Engine 層の純関数として切り出したもの。描画・物理・コンポーネントには繋がない。
// Renderer の MeshVertex は d3d11.h を引き込む GPU 資産の頂点なので、ここでは
// 持ち込まない独立した最小頂点を使う (詰め替えは FractureLibrary が行う)。

// 破壊分割コアが扱う最小限の頂点 (位置・法線・UV のみ)
struct FractureVertex {
    DirectX::XMFLOAT3 position{ 0, 0, 0 };
    DirectX::XMFLOAT3 normal{ 0, 1, 0 };
    DirectX::XMFLOAT2 uv{ 0, 0 };
};

// 三角形メッシュ。indices は 3 の倍数、外向き = CCW
struct FractureMesh {
    std::vector<FractureVertex> verts;
    std::vector<int32_t> indices;

    int32_t TriCount() const { return static_cast<int32_t>(indices.size() / 3); }
};

// 閉じ判定の結果。boundaryEdges == nonManifoldEdges == orientationMismatches == 0 のとき閉じている。
// signedVolume は closed かどうかに関わらず常に計算する (SignedVolume と同じ式)。
// 閉じたメッシュでは「実際の体積」を表し、負なら全面が内向きに巻かれている
struct ClosedMeshCheck {
    bool closed = false;
    int32_t boundaryEdges = 0;        // 使用数 1 (片側にしか面がない)
    int32_t nonManifoldEdges = 0;     // 使用数 3 以上
    int32_t orientationMismatches = 0; // 使用数 2 だが同じ向きで 2 回使われている
    double signedVolume = 0.0;
};

// 位置のビット一致で溶接してから (−0.0 は +0.0 へ畳む)、無向辺ごとの使用数と向きを調べる。
// 溶接は位置だけを見るので、UV 継ぎ目で頂点が分かれていても正しく閉じていると判定できる
ClosedMeshCheck CheckClosedMesh(const FractureMesh& mesh);

// 閉じたメッシュの全三角形の巻き順を裏返す (頂点は複製せず、各三角形の 2 番目と 3 番目の
// index を入れ替えるだけ)。内向きに巻かれた閉じたメッシュ (CheckClosedMesh().signedVolume < 0)
// を CutMeshByPlane に渡す前に外向きへ正規化するために使う
void FlipMeshWinding(FractureMesh& mesh);

// 平面 (n・x = d、n は非零なら内部で正規化) でメッシュを 1 枚切る。
// 呼び出し前に CheckClosedMesh で「閉じている (closed) かつ外向き (signedVolume > 0)」を
// 確認したメッシュを渡すこと (非多様体入力・内向き入力の結果は保証しない。内向きなら
// FlipMeshWinding で正規化してから渡す)。
// positive = n・x >= d 側、negative = n・x < d 側。外側面 (outer) と断面の蓋 (cap) を
// 両側同時に作るので、体積の和が厳密に保存される (共有した切断網から両側の蓋を作るため)
struct PlaneCutSide {
    FractureMesh outer; // 元の三角形を切っただけの外側面 (蓋を含まない)
    FractureMesh cap;   // 断面 (蓋)。法線は平面法線、UV は断面平面への正射影
};
struct PlaneCutResult {
    bool success = false;
    PlaneCutSide positive;
    PlaneCutSide negative;
    std::string failReason; // success == false のときだけ意味を持つ
};
bool CutMeshByPlane(const FractureMesh& mesh, const DirectX::XMFLOAT3& planeNormal, float planeD,
                    PlaneCutResult& out);

// 符号付き四面体体積和 (原点基準)。閉じたメッシュでのみ実際の体積として意味を持つ
double SignedVolume(const FractureMesh& mesh);

} // namespace mye
