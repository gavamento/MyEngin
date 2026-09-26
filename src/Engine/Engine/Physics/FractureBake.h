//====================================================================================
//                          FractureBake.h
//  MyEngin/ 秋田蓮音                                                     09/25/2026
//                                          破壊分割コア: Voronoi分割・凸包・接着グラフ
//====================================================================================
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Engine/Physics/ConvexHull.h"
#include "Engine/Engine/Physics/FractureMesh.h"

namespace mye {

// ---- 破壊分割コア: Voronoi 分割 (M80b) ----
// sub-01 (FractureMesh.h) の平面切断+蓋を使って、spec §4.1「焼き」の 1〜8 を純関数として
// 完成させる (1 の閉じ判定/ボクセル化分岐は sub-04 の FractureVoxel.h と組み合わせて
// BakeFracture が行う)。出力は「破片の列 + 接着グラフ + 焼きの記録」。ファイル形式・
// ライブラリ登録は sub-03、スキンは sub-10 (このファイルは骨を知らない)。

inline constexpr int32_t kMaxFracturePieces = 256;
inline constexpr int32_t kMaxFractureNeighbors = 32;

// 焼き方式の版。BakeFracture の出力が変わる変更 (アルゴリズム・丸め・順序) をしたら必ず上げる。
// 上げないと、RT の BVH など「同じ ID の中身」を前提にしたキャッシュが古いまま残る。
// `.mfrac` の保存名 (内容ハッシュ) に混ぜ込み、同じソース入力でも版が違えば別ファイルとして
// 保存させる — 上げ忘れは FractureLibrary::ReloadFromFile が保険になる
inline constexpr uint32_t kFractureBakeVersion = 2; // 隣接 32 本超の切り捨てを対称化

// 破片 i の隣接 1 本 (相手 index、面積)
struct FractureNeighbor {
    int32_t pieceIndex = 0;
    float area = 0.0f;
};

// 隣接が 32 本 (kMaxFractureNeighbors) を超える破片の切り捨てを対称に行う (spec §4.1 焼き7)。
// neighbors は破片 index 昇順の対称な隣接グラフ (neighbors[i] に j があれば neighbors[j] に i が
// ある) を入力に取り、超過分を面積の小さい順 (同値は相手 index 小) に選んで両側から同時に消す。
// i 側の判断で j 側も巻き込むため、32 本以下だった破片が結果的にそれより少なくなることがある
// (上限を守ることだけが制約で、それ自体は仕様違反ではない)。droppedCount[i] は i 側から実際に
// 消えた本数 (neighbors と同じ並び、対称なので総和は必ず偶数)。決定的 (同じ入力で同じ出力)
void CapNeighborsSymmetrically(std::vector<std::vector<FractureNeighbor>>& neighbors,
                               std::vector<int32_t>& droppedCount);

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

// 焼きの大まかな進行段階 (Editor の非同期ワーカーが「焼いています: <段階>」に使う表示専用の
// 分類。出力バイト列にも FractureBakeDigest にも影響しない — 呼び出し側は無視してよい)
enum class FractureBakeStage : int32_t {
    ClosedCheck = 0, // 閉じ判定
    Voxelize = 1,    // ボクセル化 (openMeshMode==1 かつ閉じていないときだけ通る)
    Split = 2,       // 内部シード配置 + セル切断
    Hull = 3,        // 破片ごとの凸包・接着グラフの仕上げ
};
// 非 null なら BakeFracture が各段階に入るたび同期呼び出しする (ワーカースレッド上で呼ばれる
// 前提。呼び出し側が自分のスレッドで安全な形 — atomic 変数の書き込み等 — にすること)
using FractureBakeProgressFn = void (*)(FractureBakeStage stage, void* userData);

struct FractureBakeInput {
    FractureMesh sourceMesh; // 閉じていなくてよい (openMeshMode で拒否/ボクセル化を選ぶ)
    uint32_t seed = 1;
    int32_t pieceCount = 16; // 目安。上限は kMaxFracturePieces
    float minVolumeRatio = 0.1f; // 極小片の統合しきい値 (平均体積に対する比)
    int32_t openMeshMode = 0;    // 0 = 閉じていなければ拒否 / 1 = ボクセル化を許容 (spec §4.1 焼き1)
    // openMeshMode==1 のときのボクセル解像度 (FractureVoxel.h が [16,256] へクランプ)。
    // 既定 32 は sub-04 の計測に基づく (開いた箱 + pieceCount=16 で Release 10 秒以内に収まり、
    // 48 以上では断面の三角形分割が失敗する組み合わせがあるため)
    int32_t voxelResolution = 32;
    FractureBakeProgressFn progress = nullptr; // 任意 (Editor の非同期焼き用、M80i)
    void* progressUserData = nullptr;
    // 取り消しの旗 (任意、M80p)。非 null かつ true なら、段階の合間 (ClosedCheck/Voxelize/Split
    // の境界) とセル切断ループの合間で焼きを打ち切り、failReason="取り消し" で false を返す。
    // 既定 nullptr は既存呼び出しと同じ挙動 (見ない)
    const std::atomic<bool>* cancelFlag = nullptr;
};

struct FractureBakeResult {
    bool success = false;
    std::string failReason; // success == false のときだけ意味を持つ
    // 閉じていないための拒否だったときの構造化理由 (Editor が赤字メッセージを組み立てる用、
    // M80i)。failReason の文字列 (日本語固定) をパースしなくて済むように、CheckClosedMesh の
    // 生の集計をそのまま残す。rejectedOpenMesh==false のときは意味を持たない
    bool rejectedOpenMesh = false;
    int32_t boundaryEdges = 0;
    int32_t nonManifoldEdges = 0;
    int32_t orientationMismatches = 0;
    bool cancelled = false; // cancelFlag による打ち切り (M80p)。failReason の文字列判定はしない
    std::vector<FracturePieceBake> pieces;
    int32_t seedsRequested = 0;
    int32_t seedsPlaced = 0;   // 内部シードとして実際に置けた数 (試行上限で届かないことがある)
    int32_t mergedCount = 0;   // 極小片統合が起きた回数
};

// 焼きの入口 (spec §4.1 焼きの 1〜8)。まず CheckClosedMesh で sourceMesh を検査し、
// 閉じていなければ openMeshMode で分岐する (0 = 理由付きで拒否、1 = FractureVoxel.h の
// ボクセル化+surface nets で閉じたメッシュに変換してから続行)。閉じているが内向き
// (signedVolume < 0) なら FlipMeshWinding で外向きに正規化する。そのあとの内部シード・
// セル切断・非連結分離・極小片統合・凸包・接着グラフは同じ 1 本のパイプラインを通る。
// 決定論: 同じ input から同じ pieces の並び・同じバイト列を返す (FractureBakeDigest で確認できる)
bool BakeFracture(const FractureBakeInput& input, FractureBakeResult& out);

// SelfTest 専用の入口: 内部シード生成 (PlaceSeeds) を経由せず、明示的な位置をそのままシードとして
// 使う。それ以外 (セル切断・非連結分離・極小片統合・凸包・接着グラフ) は BakeFracture と同じ
// パイプラインを通るので、隣接面積などを狙った配置で検証できる。sourceMesh は
// 「閉じていて外向き」を呼び出し側が保証すること (開いたメッシュの処理は BakeFracture 側だけが持つ)
bool BakeFractureWithSeeds(const FractureMesh& sourceMesh, const std::vector<DirectX::XMFLOAT3>& seeds,
                           float minVolumeRatio, FractureBakeResult& out);

// 焼き結果全体を決定的に直列化したバイト列の FNV-1a 64bit ダイジェスト
// (Engine/Core/Hash.h の HashBytes と同じ規約)。Debug/Release の比較に使う
uint64_t FractureBakeDigest(const FractureBakeResult& result);

} // namespace mye
