#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include <DirectXMath.h>

namespace mye {

// ---- 凸包 (M60f、Collider.shape=5) ----
// メッシュ資産の CPU 頂点から凸多面体を作り、**動的剛体の形状**として使えるようにする。
// shape=3 (三角形スープ) が静的専用なのは「面の裏表しか分からない三角形の集まりでは
// 貫通量が定義できない」ためで、凸包は閉じた凸体なので SAT で分離軸と貫通量が出せる =
// 動的剛体同士でも解ける。これが shape=3 と別の種を立てる唯一の理由。
//
// 決定論: 生成は入力頂点の**順序に依らない**。頂点を (x, y, z, 元 index) の全順序で明示
// ソートしてから重複を落とし、以降の「最遠点」「可視面」「面の統合」の選択もすべて
// index 昇順のタイブレークで固定してある (BVH 構築が centroid + 三角形番号のソートで
// 決定論を作ったのと同じ手)。生成結果は `.mcvx` クック blob へ保存され、
// **フレッシュ生成とビット同一が契約** (.mmdl と同じ約束)。
//
// 生成アルゴリズムは QuickHull ではなく**総当たりの逐次追加**にしてある。QuickHull の
// 面隣接構造は「地平線が非多様体になった」ときの復旧が難しく、縮退入力 (同一平面に
// 潰れたメッシュ、極端に細い三角形) で静かに壊れた凸包を返す危険がある。逐次追加なら
// 可視面と地平線を毎回その場で数え直すので、途中で破綻したらその点を捨てて先へ進める。
// 上限 kConvexMaxVerts のおかげで計算量も実測で問題にならない。

// 凸包の頂点数上限。SAT の稜線×稜線軸は O(EA·EB) なので、ここを上げると衝突判定が
// 二乗で重くなる。上限に達したら以降の点を足さない = 真の凸包の**部分集合**になるだけで
// 凸性は保たれる (遠い点から順に足しているので、打ち切っても入力順には依らない)
inline constexpr int kConvexMaxVerts = 64;
// 単体多面体 (全面が三角形) の上限から導いた面数 / 稜線数の上限。面の統合は数を減らす
// だけなので、この 2 本を超えることはない (超えたら衝突側が黙って切り捨てる)
inline constexpr int kConvexMaxFaces = 2 * kConvexMaxVerts - 4;
inline constexpr int kConvexMaxEdges = 3 * kConvexMaxVerts - 6;

// 凸包の 1 面 (凸多角形)。三角形ではなく多角形なのは参照面クリップのため —
// 箱の凸包が 12 枚の三角形のままだと面接触のマニフォールドが 3 点で頭打ちになり、
// box-box の 4 点接触と同じ安定性が出ない
struct ConvexFace {
    float nx = 0, ny = 1, nz = 0; // 外向き単位法線 (ローカル、スケール未適用)
    float d = 0;                  // 支持平面 n·x = d
    int32_t first = 0;            // faceVerts 内の開始 index
    int32_t count = 0;            // 頂点数 (外から見て CCW、>= 3)
};

// SAT の稜線×稜線軸に使う稜線。f0/f1 は共有する 2 面 = Gauss map 枝刈りの材料
// (この 2 面の法線が張る弧と、相手側の弧が交差する稜線ペアだけが軸候補になる)
struct ConvexEdge {
    int32_t v0 = 0, v1 = 0; // 必ず v0 < v1 に正規化 (重複除去のキー)
    int32_t f0 = 0, f1 = 0;
};

struct ConvexHullData {
    std::vector<DirectX::XMFLOAT3> verts;
    std::vector<ConvexFace> faces;
    std::vector<int32_t> faceVerts; // faces[i].first から count 個が面 i の頂点列
    std::vector<ConvexEdge> edges;
    // 密度 1・スケール未適用の質量特性。inertia は**重心まわり**のフルテンソル
    // (凸多面体の慣性は一般に非対角なので対角では表現できない — M60e と同じ理由)
    float volume = 0.0f;
    DirectX::XMFLOAT3 com = { 0, 0, 0 };
    float inertia[3][3] = {};
    DirectX::XMFLOAT3 aabbMin = { 0, 0, 0 };
    DirectX::XMFLOAT3 aabbMax = { 0, 0, 0 };
    float boundRadius = 0.0f; // 形状原点からの最大頂点距離 (CCD の起動しきい値 / 粗い棄却)

    bool Valid() const
    {
        return verts.size() >= 4 && faces.size() >= 4 && volume > 0.0f;
    }
};

// 点群から凸包を生成する。戻り値 false = 縮退 (点 / 線分 / 平面に潰れている) で
// 真の凸包が作れなかった場合で、**out には入力 AABB の箱が入る** — 呼び出し側が
// 「形状なし」に落ちて物体がすり抜けるより、太らせた箱で当たるほうが常に安全。
// 潰れた軸には extent の 1e-4 相当の最小厚みを与える (零体積は慣性が発散するため)
bool BuildConvexHull(const std::vector<DirectX::XMFLOAT3>& points, ConvexHullData& out);

// スケール適用後の質量特性 (四面体積分、密度 1)。inertia は重心まわりのフルテンソル。
// **非一様スケールでは慣性テンソルが相似変換にならない**ので、毎回スケール後の頂点で
// 積分し直す (頂点 64・面数十なので収集時に 1 回なら十分に安い)。
// スケールは絶対値で扱う (ApplyScaledExtents / ShapeVolumeWorld と同一規約 —
// 負スケールで巻き順が反転して体積が負になるのを避ける)
void ConvexMassProperties(const ConvexHullData& h, float sx, float sy, float sz, float& volume,
                          DirectX::XMFLOAT3& com, float inertia[3][3]);

// 方向 (dx,dy,dz) に最も遠い頂点 index (同値は index 小)。SAT の支持点はここ 1 箇所
int32_t ConvexSupportLocal(const ConvexHullData& h, float dx, float dy, float dz);

// blob ⇄ 構造体 (`.mcvx` クック用)。Deserialize は境界検査つきで、破損ファイルでは
// false を返すだけで決して落ちない (CookedCache の他の種と同じ契約)
void SerializeConvexHull(const ConvexHullData& h, std::vector<uint8_t>& out);
bool DeserializeConvexHull(const uint8_t* p, size_t size, size_t& pos, ConvexHullData& out);

} // namespace mye
