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
    std::vector<TerrainChunk> chunks;

    const TerrainChunk* At(uint32_t cx, uint32_t cz) const;
};

// chunkTiles の丸め (範囲外・0・負値を必ず有効値へ落とす)。純関数
uint32_t ClampChunkTiles(int32_t requested);

// ハイトマップを chunkTiles 単位で分割し、各チャンクの AABB まで埋める。
// data が Valid() でなければ false (out は空)。純関数
bool BuildChunkLayout(const TerrainAsset::TerrainData& data, int32_t chunkTiles,
                      TerrainChunkLayout& out);

// チャンク 1 枚の頂点 / インデックスを組む。純関数。
//
// ★法線は**チャンクではなく地形全体の texel 座標**で中心差分を取る。チャンク内で
//   閉じて計算すると境界の頂点が両側で違う法線になり、継ぎ目がライティングの線として出る。
// ★タンジェントは持たない (`MeshVertex` に無い) — common.hlsli の PerturbNormal が
//   画面微分から TBN を組むので、法線マップは追加属性なしで載る (M58d)。
void BuildChunkMesh(const TerrainAsset::TerrainData& data, const TerrainChunk& chunk,
                    std::vector<MeshVertex>& outVerts, std::vector<uint32_t>& outIndices);

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

// 描画側 (M58c の TerrainPass) へ渡す可視チャンク 1 件。
struct TerrainDrawItem {
    AssetID mesh = {};
    DirectX::XMFLOAT4X4 world = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    float viewZ = 0.0f; // ソート / LOD 選択 (M58e) 用。RenderItem::viewZ と同じ規約
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
    size_t CacheSize() const { return cache_.size(); }
    // 地形アセットを焼き直したとき (M58f のブラシ) に呼ぶ。次の Collect で作り直す
    void Clear() { cache_.clear(); }

private:
    struct Entry {
        TerrainAsset::TerrainData data;
        TerrainChunkLayout layout;
        std::vector<AssetID> chunkMeshes; // layout.chunks と同じ並び
        TerrainSurface surface;           // M58d: レイヤ bind (地形単位)
        bool valid = false;               // false = ロードに失敗した (毎フレーム再試行しない)
    };

    // source (assets\ 相対) + chunkTiles をキーに構築済みインスタンスを引く。
    // 失敗も含めてキャッシュする — 壊れたパスを毎フレーム開き直すと編集中に固まるため
    const Entry* Acquire(const char* source, int32_t chunkTiles, MeshLibrary& meshes,
                         TextureLibrary& textures, const std::wstring& assetsRoot);

    std::unordered_map<std::wstring, Entry> cache_;
    std::vector<uint32_t> visibleScratch_; // Collect 内で再利用 (毎フレームの確保を避ける)
    uint32_t lastTotal_ = 0;
    uint32_t lastVisible_ = 0;
};

} // namespace mye
