#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/EntityID.h"
#include "Engine/Engine/Asset/TerrainAsset.h"
#include "Engine/Renderer/FrustumCull.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/TerrainPass.h" // M58d: TerrainSurface (レイヤ bind の正本)

namespace mye {

class World;

// 地形のチャンク分割 + 視錐台カリング (M58b、spec §6.5)。
//
// **描画専用レーン** — ここに書かれた値は 1 つもワールドハッシュに載らない。
// 地形コリジョン (= sim レーン入り) は engine_spec §6.5 で M59 送りと決めてある。
//
// 分割・メッシュ生成・カリングは全部**純関数**にしてある。GPU も World も要らないので
// TerrainSelfTest がヘッドレスで丸ごと検査できる = 描画パス (M58c) が乗る前に
// 「格子の作り方」だけを固定できる。実際に GPU バッファを作るのは
// TerrainSystem::Collect の中の MeshLibrary::Register 1 箇所だけ。

// チャンク 1 辺のタイル数の許容範囲。
// 下限 2: 1 だと 1 チャンク = 三角 2 枚で、チャンク数が頂点数と同じオーダーになる。
// 上限 256: 257x257 頂点 = 約 66k 頂点 / 1 チャンクで、16bit index を使う将来の
// 最適化余地 (65536 頂点) をぎりぎり跨がない位置に置いた。
// **TerrainComponent.chunkTiles の既定値 32 はこの範囲の代表値** (Core 層は Engine 層の
// ヘッダを読めないので Components.h 側は数値リテラル + 相互参照コメントで持っている)
inline constexpr int32_t kTerrainMinChunkTiles = 2;
inline constexpr int32_t kTerrainMaxChunkTiles = 256;
inline constexpr int32_t kTerrainDefaultChunkTiles = 32;

// ---- LOD (M58e) ----
// **エンジン初の LOD 機構**。地形以外に持ち込まないよう、選択も刻みもこのヘッダに閉じてある。
// LOD n は「頂点格子を 2^n texel おきに間引いたメッシュ」で、三角数は 1/4^n になる。
// 3 段にしてあるのは chunkTiles の既定 32 が stride 4 でもまだ 8 セル残る (= 端数チャンクでも
// 格子として成立する) 下限だから。4 段目 (stride 8) は 32 タイルで 4 セルまで潰れる
inline constexpr uint32_t kTerrainLodCount = 3;

inline constexpr uint32_t TerrainLodStride(uint32_t lod)
{
    return 1u << (lod < kTerrainLodCount ? lod : kTerrainLodCount - 1);
}

// スカート深さの安全率 (M58e)。**幾何的には段差ちょうどで隙間は閉じる**が、
// 「ちょうど接する」= 頂点を共有しない 2 面が同じ線で終わる形なので、ラスタライズの
// フィル規則しだいで 1 画素の隙間が残りうる。25% はそのぶんの逃げで、意味論ではない
inline constexpr float kTerrainSkirtMargin = 1.25f;

// チャンク 1 枚 (純データ)。頂点格子はハイトマップの texel 格子そのもの —
// タイル (tx, tz) の 4 隅が texel (tx, tz)..(tx+1, tz+1) に対応する。
struct TerrainChunk {
    uint32_t chunkX = 0; // チャンク格子座標 (デバッグ表示 / LOD の隣接判定 (M58e) 用)
    uint32_t chunkZ = 0;
    uint32_t tileX0 = 0; // 担当タイルの開始 (= 頂点格子の開始 texel でもある)
    uint32_t tileZ0 = 0;
    uint32_t tilesX = 0; // 担当タイル数。右端 / 奥端のチャンクだけ端数になる
    uint32_t tilesZ = 0;
    // 地形ローカル空間の AABB (**中心原点** — 組み込みプリミティブと同じ規約)。
    // y はこのチャンクが実際に含む高さの min/max なので、平坦な地形でも
    // 「地形全体の高さ範囲」に膨らまない = カリングが効く
    DirectX::XMFLOAT3 localMin = { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 localMax = { 0.0f, 0.0f, 0.0f };
};

// 分割結果。chunks の並びは z 外側 / x 内側の固定順 (決定的)。
struct TerrainChunkLayout {
    uint32_t chunkTiles = 0;
    uint32_t countX = 0;
    uint32_t countZ = 0;
    // M58e: この layout に対して解決済みの LOD 段数とスカート深さ (記録)。
    // lodCount == 1 = LOD 無効 = スカートも出ない (M58d までと同じメッシュ)
    uint32_t lodCount = 1;
    float skirtDepth = 0.0f;
    std::vector<TerrainChunk> chunks;

    const TerrainChunk* At(uint32_t cx, uint32_t cz) const;
};

// chunkTiles の丸め (範囲外・0・負値を必ず有効値へ落とす)。純関数
uint32_t ClampChunkTiles(int32_t requested);

// ハイトマップを chunkTiles 単位で分割し、各チャンクの AABB まで埋める。
// data が Valid() でなければ false (out は空)。純関数
bool BuildChunkLayout(const TerrainAsset::TerrainData& data, int32_t chunkTiles,
                      TerrainChunkLayout& out);

// LOD n のストライドで、チャンク 1 辺のタイル範囲を刻んだ texel 列を作る (M58e)。純関数。
//
// ★**末尾は必ずチャンクの端**にする (端数タイルは最後のセルだけ短くなる)。ここを
//   「stride の倍数で切り上げ」にすると、端数チャンクの縁が地形の外へはみ出すか
//   内側で止まるかのどちらかになり、同じ LOD の隣接チャンクどうしですら縁を共有できない。
// 出力は cells+1 個 (tiles==0 なら開始 texel 1 個だけ)
void TerrainLodSamples(uint32_t tile0, uint32_t tiles, uint32_t stride,
                       std::vector<uint32_t>& out);

// カメラ空間深度から LOD 段を選ぶ (M58e)。純関数。
// lodDistance <= 0 = LOD 無効 (常に 0)。切替距離は lodDistance * 2^n で、
// 段が上がるほど遠くなる = 画面上の三角密度がおおよそ一定になる。
// viewZ が負 (カメラの背後) / NaN は 0 に倒す
uint32_t SelectTerrainLod(float viewZ, float lodDistance);

// LOD 境界で生じうる縁の最大段差 (ワールド Y 単位) を測る (M58e)。純関数。
//
// ★**これが「クラックが出ない」の根拠**。隣接チャンクが LOD la / lb のとき、共有する縁の
//   高さは互いに「自分の制御点を線形補間した曲線」なので、両者の差が最大この値になる。
//   スカートをこの値ぶん下ろせば、2 枚の面の縦方向の区間が必ず重なる = 幾何的に穴が開かない。
//   スカート深さを勘で決めずにここから導けるのが、この関数を置いた理由
//   (TerrainSelfTest が「実際に生成した縁」と突合する)。
// 外周の縁は隣が居ないので数えない (数えるとスカートが無駄に深くなる)
float ComputeMaxLodEdgeGap(const TerrainAsset::TerrainData& data,
                           const TerrainChunkLayout& layout, uint32_t lodCount);

// スカートぶん各チャンク AABB の下端を下げる (M58e)。
// **分割 → 段差の計測 → AABB の拡張**の順にしか組めない (段差の計測が layout を要る) ので、
// BuildChunkLayout の引数ではなく後掛けの関数にしてある。
// 下げないと「上面は視錐台の外だがスカートは見えている」チャンクをカリングが落とす
void ExpandLayoutForSkirt(TerrainChunkLayout& layout, uint32_t lodCount, float skirtDepth);

// チャンク 1 枚の頂点 / インデックスを組む。純関数。
//
// ★法線は**チャンクではなく地形全体の texel 座標**で中心差分を取る。チャンク内で
//   閉じて計算すると境界の頂点が両側で違う法線になり、継ぎ目がライティングの線として出る。
// ★タンジェントは持たない (`MeshVertex` に無い) — common.hlsli の PerturbNormal が
//   画面微分から TBN を組むので、法線マップは追加属性なしで載る (M58d)。
//
// M58e: lod > 0 で頂点格子を 2^lod texel おきに間引く。skirtDepth > 0 で
// **隣接チャンクがある縁にだけ**スカート (真下へ伸ばした帯) を足す。
//  ★外周の縁にスカートを出さない — 隣が居ない縁は割れようがないのに、出すと地形の外側に
//    「垂直な壁」が立って絵に映る (地形は面であって塊ではない)。
//  ★スカートの表は**チャンクの外向き**。隣のチャンク側から覗いたときに見える必要があるので、
//    両チャンクが互いに外を向いたスカートを出すことで、どちら側から見ても隙間が塞がる
void BuildChunkMesh(const TerrainAsset::TerrainData& data, const TerrainChunk& chunk,
                    std::vector<MeshVertex>& outVerts, std::vector<uint32_t>& outIndices,
                    uint32_t lod = 0, float skirtDepth = 0.0f);

// 視錐台に入るチャンクの index を outVisible へ積む (層の並び順を保つ)。
// 戻り値 = 可視数。純関数
size_t CullChunks(const TerrainChunkLayout& layout, const Frustum& frustum,
                  const DirectX::XMFLOAT4X4& world, std::vector<uint32_t>& outVisible);

// 地形のレイヤ表 (アセット) → 描画側の bind (TerrainSurface) へ解決する (M58d)。
// テクスチャのロードとスプラットマップの GPU 化もここで行う (ヘッドレス =
// TextureLibrary::Init 前なら AssetID は空のまま返る。tint / tiling / 有効フラグの
// 組み立ては GPU 非依存なので TerrainSelfTest がそこを検査する)。
//
// ★**レイヤ数に満たないスロットは tint.a = 0 で殺す。** シェーダは有効フラグを掛けてから
//   合計 1 へ再正規化するので、レイヤが 4 未満でも / 手書きのスプラット画像で未使用
//   チャンネルに重みが載っていても色が痩せない。
// ★**レイヤが 1 枚も無い地形は M58c の単色 (くすんだ緑) に倒す** — 「既定値 = 従来の
//   見た目」の規約。レイヤ表を空にしただけで真っ白になるのは事故にしか見えない。
// srcPath = `.terrain.json` の絶対パス (レイヤの相対パス解決の基準)
TerrainSurface BuildTerrainSurface(const TerrainAsset::TerrainData& data,
                                   const std::wstring& srcPath, TextureLibrary& textures);

// 地形インスタンス 1 件を組み立てるパラメータ (M58e)。**そのままキャッシュのキー**になる —
// ここに入っている値を変えるとメッシュそのものが別物になるため。
struct TerrainBuildParams {
    int32_t chunkTiles = kTerrainDefaultChunkTiles;
    // LOD 段数。1 = LOD 無効 = スカートも出ない (M58d までとビット同一のメッシュ)。
    // **既定が 1 なのが「新機能は全部 opt-in」の実体** — 地形を置いただけの絵は変わらない
    uint32_t lodCount = 1;
    // スカート深さ。**< 0 = スカート無し (A/B 用) / 0 = 自動 (ComputeMaxLodEdgeGap 由来) /
    // > 0 = 明示指定**。自動が既定なのは「勘で決めた深さ」が絵にしか現れないため
    float skirtDepth = 0.0f;
};

// 描画側 (M58c の TerrainPass) へ渡す可視チャンク 1 件。
struct TerrainDrawItem {
    AssetID mesh = {};
    DirectX::XMFLOAT4X4 world = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    float viewZ = 0.0f; // ソート / LOD 選択 (M58e) 用。RenderItem::viewZ と同じ規約
    uint32_t lod = 0;   // M58e: このチャンクに選ばれた LOD 段 (診断 / selftest 用)
    // レイヤ表 (M58d) や高さ (M58e) を引くための参照。TerrainSystem のキャッシュを指すので
    // 次の Collect でキャッシュを捨てない限り有効
    const TerrainAsset::TerrainData* terrain = nullptr;
    EntityID entity = {};
    uint32_t chunkIndex = 0;
    // M58d: 解決済みのレイヤ bind (スプラット + 4 レイヤの albedo/normal/tint/tiling)。
    // 地形単位の値だが、描画側がソートで並べ替えるのでチャンクごとに複製して持つ
    TerrainSurface surface;
};

// 地形インスタンスのキャッシュ + 収集。RenderSystem が 1 つ持つ想定 (M58c で配線)。
class TerrainSystem {
public:
    // World の TerrainComponent を走査し、未構築の地形を (ロード → 分割 → メッシュ登録) して
    // から視錐台で間引き、可視チャンクを out へ積む。
    //
    // ★キャッシュは unordered_map だが**走査順には一切使わない** — 出力順は
    //   ForEachArchetype/row (決定的) + layout.chunks の固定順だけで決まる。
    //   描画順が実行ごとに揺れるとアルファブレンドや Z 争いの見た目が非決定になるため。
    // 戻り値 = 検査したチャンク総数 (out.size() が可視数)
    uint32_t Collect(World& world, MeshLibrary& meshes, TextureLibrary& textures,
                     const std::wstring& assetsRoot, const Frustum& frustum,
                     const DirectX::XMFLOAT4X4& view, std::vector<TerrainDrawItem>& out);

    uint32_t LastChunkCount() const { return lastTotal_; }
    uint32_t LastVisibleCount() const { return lastVisible_; }
    // M58e: 直近の Collect で LOD 段 lod が選ばれた可視チャンク数 (HUD / selftest 用)
    uint32_t LastLodCount(uint32_t lod) const
    {
        return lod < kTerrainLodCount ? lastLodCounts_[lod] : 0;
    }
    size_t CacheSize() const { return cache_.size(); }
    // 地形アセットを焼き直したとき (M58f のブラシ) に呼ぶ。次の Collect で作り直す
    void Clear() { cache_.clear(); }

private:
    struct Entry {
        TerrainAsset::TerrainData data;
        TerrainChunkLayout layout;
        // M58e: **[chunkIndex * lodCount + lod]**。LOD 無効なら lodCount == 1 =
        // M58d までと同じ「チャンクごとに 1 本」
        std::vector<AssetID> chunkMeshes;
        TerrainSurface surface; // M58d: レイヤ bind (地形単位)
        uint32_t lodCount = 1;  // M58e: 実際に焼いた段数
        bool valid = false;     // false = ロードに失敗した (毎フレーム再試行しない)
    };

    // source (assets\ 相対) + 組み立てパラメータをキーに構築済みインスタンスを引く。
    // 失敗も含めてキャッシュする — 壊れたパスを毎フレーム開き直すと編集中に固まるため
    const Entry* Acquire(const char* source, const TerrainBuildParams& params, MeshLibrary& meshes,
                         TextureLibrary& textures, const std::wstring& assetsRoot);

    std::unordered_map<std::wstring, Entry> cache_;
    std::vector<uint32_t> visibleScratch_; // Collect 内で再利用 (毎フレームの確保を避ける)
    uint32_t lastTotal_ = 0;
    uint32_t lastVisible_ = 0;
    uint32_t lastLodCounts_[kTerrainLodCount] = {};
};

} // namespace mye
