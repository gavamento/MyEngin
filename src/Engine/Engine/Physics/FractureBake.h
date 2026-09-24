//====================================================================================
//                          FractureBake.h
//  MyEngin/ 秋田蓮音                                                     09/25/2026
//                                          破壊分割コア: Voronoi分割・凸包・接着グラフ
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Engine/Physics/ConvexHull.h"
#include "Engine/Engine/Physics/FractureMesh.h"

namespace mye {

// ---- 破壊分割コア: Voronoi 分割 (M80b) ----
// sub-01 (FractureMesh.h) の平面切断+蓋を使って、spec §4.1「焼き」の 2〜8 を純関数として
// 完成させる。入力は閉じていて外向き (CheckClosedMesh 済み) なソースメッシュ、出力は
// 「破片の列 + 接着グラフ + 焼きの記録」。ファイル形式・ライブラリ登録は sub-03、
// 開いたメッシュのボクセル化は sub-04、スキンは sub-10 (このファイルは骨を知らない)。

inline constexpr int32_t kMaxFracturePieces = 256;
inline constexpr int32_t kMaxFractureNeighbors = 32;

// 破片 i の隣接 1 本 (相手 index、面積)
struct FractureNeighbor {
    int32_t pieceIndex = 0;
    float area = 0.0f;
};

// 焼き結果の破片 1 個。outer / cap / hull は破片原点 (体積重心) を基準にしたローカル空間
struct FracturePieceBake {
    DirectX::XMFLOAT3 origin{ 0, 0, 0 }; // 体積重心のソース空間位置
    double volume = 0.0;
    FractureMesh outer;             // 元の三角形を切っただけの外側面 (蓋を含まない)
    FractureMesh cap;                // 断面 (蓋。隣接ごとの面が混在する 1 本のメッシュ)
    ConvexHullData hull;             // 外側+蓋の全頂点から作った凸包
    std::vector<FractureNeighbor> neighbors; // 隣接表 (相手 index 昇順、32 本まで)
    int32_t droppedNeighbors = 0;    // 32 本を超えて切り捨てた本数
};

struct FractureBakeInput {
    FractureMesh sourceMesh; // 呼び出し側が CheckClosedMesh で「閉じていて外向き」を保証すること
    uint32_t seed = 1;
    int32_t pieceCount = 16; // 目安。上限は kMaxFracturePieces
    float minVolumeRatio = 0.1f; // 極小片の統合しきい値 (平均体積に対する比)
};

struct FractureBakeResult {
    bool success = false;
    std::string failReason; // success == false のときだけ意味を持つ
    std::vector<FracturePieceBake> pieces;
    int32_t seedsRequested = 0;
    int32_t seedsPlaced = 0;   // 内部シードとして実際に置けた数 (試行上限で届かないことがある)
    int32_t mergedCount = 0;   // 極小片統合が起きた回数
};

// spec §4.1 焼きの 2〜8 (内部シード・セル切断・非連結分離・極小片統合・凸包・接着グラフ)。
// 決定論: 同じ input から同じ pieces の並び・同じバイト列を返す (FractureBakeDigest で確認できる)
bool BakeFracture(const FractureBakeInput& input, FractureBakeResult& out);

// SelfTest 専用の入口: 内部シード生成 (PlaceSeeds) を経由せず、明示的な位置をそのままシードとして
// 使う。それ以外 (セル切断・非連結分離・極小片統合・凸包・接着グラフ) は BakeFracture と同じ
// パイプラインを通るので、隣接面積などを狙った配置で検証できる
bool BakeFractureWithSeeds(const FractureMesh& sourceMesh, const std::vector<DirectX::XMFLOAT3>& seeds,
                           float minVolumeRatio, FractureBakeResult& out);

// 焼き結果全体を決定的に直列化したバイト列の FNV-1a 64bit ダイジェスト
// (Engine/Core/Hash.h の HashBytes と同じ規約)。Debug/Release の比較に使う
uint64_t FractureBakeDigest(const FractureBakeResult& result);

} // namespace mye
