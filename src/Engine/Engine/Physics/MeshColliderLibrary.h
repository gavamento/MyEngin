#pragma once
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/EntityID.h"

namespace mye {

struct RenderResources;

// ---- 静的メッシュコライダー (M41、Collider.shape=3) ----
// MeshLibrary が保持する CPU 頂点 (ローカル空間) から三角形リスト + AABB ツリー (BVH) を
// 構築してキャッシュする。静的/kinematic 専用 (動的剛体のメッシュ形状は対象外)。
// 決定論: 構築は「centroid の中央分割 + (centroid, 三角形番号) の明示ソート」で
// 入力メッシュのみに依存。クエリの三角形番号列は昇順に整列してから判定に使う (走査順非依存)。

struct MeshBvhNode {
    float minX = 0, minY = 0, minZ = 0, maxX = 0, maxY = 0, maxZ = 0; // ローカル AABB
    int32_t left = -1;    // 内部ノード: 子 index。葉は -1
    int32_t right = -1;
    int32_t triStart = 0; // 葉のみ: triOrder の範囲
    int32_t triCount = 0;
};

struct MeshColliderData {
    std::vector<DirectX::XMFLOAT3> positions;
    std::vector<uint32_t> indices;  // 3*triCount (メッシュの元 index 順のまま)
    std::vector<int32_t> triOrder;  // BVH 葉が参照する三角形番号列 (構築時に再配置)
    std::vector<MeshBvhNode> nodes; // [0] = root。三角形 0 枚なら empty

    int32_t TriCount() const { return static_cast<int32_t>(indices.size() / 3); }
};

// CPU 頂点列から BVH 付きコライダーデータを構築する (純関数、selftest 対象)
void BuildMeshColliderData(const std::vector<DirectX::XMFLOAT3>& positions,
                           const std::vector<uint32_t>& indices, MeshColliderData& out);

// ローカル AABB と重なる三角形番号を **昇順** で out に収集 (最大 maxOut、戻り値 = 書いた数)。
// 判定は AABB 同士の保守的重なり (走査は固定順の反復スタック = 決定論)
int MeshGatherTris(const MeshColliderData& m, float minX, float minY, float minZ, float maxX,
                   float maxY, float maxZ, int32_t* out, int maxOut);

// AssetID → コライダーデータの lazy 構築 + キャッシュ。EngineLoop が所有する
class MeshColliderLibrary {
public:
    void Init(RenderResources* resources) { resources_ = resources; }
    // 未登録メッシュ / CPU 頂点なしは nullptr (呼び出し側は shape=3 を無視する)
    const MeshColliderData* Get(AssetID meshAsset);
    // 任意データの直接登録 (selftest / 手続き生成メッシュ用)。同 ID は差し替え
    void Register(AssetID id, MeshColliderData data);
    void Clear() { cache_.clear(); }

private:
    RenderResources* resources_ = nullptr;
    std::unordered_map<uint64_t, std::unique_ptr<MeshColliderData>> cache_;
};

// モジュール注入 (assetkey:: と同じ流儀)。EngineLoop が起動時に Install し終了時に外す。
// PhysicsSystem / クエリ / トリガーの pose 構築サイトが shape=3 の meshData 解決に使う。
// メインスレッド専用
namespace meshcol {
void Install(MeshColliderLibrary* lib);
const MeshColliderData* Resolve(AssetID meshAsset); // 未接続/未登録 = nullptr
} // namespace meshcol

} // namespace mye
