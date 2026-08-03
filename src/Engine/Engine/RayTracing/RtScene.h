#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Core/EntityID.h"
#include "Engine/Engine/RayTracing/RtSceneBuild.h"
#include "Engine/Renderer/RayTracing/RtPasses.h"

namespace mye {

class GraphicsDevice;
struct RenderResources;

// レイトレ用シーンの GPU 常駐管理 (M46b)。
//   BLAS = メッシュ単位でキャッシュ (物理と共用の MeshColliderData から焼く) → 連結バッファ
//   TLAS / インスタンス / マテリアル = 毎フレーム再構築 (動的な物体に無条件で追従)
// 描画専用 — sim / WorldHash / リプレイには一切関与しない。
// GpuParticleBackend と同じく Engine 層で D3D リソースを持つ (レイヤ規約の既存前例)
class RtScene {
public:
    // RenderSystem が ECS から収集する 1 インスタンス
    struct InstanceDesc {
        AssetID mesh = {};
        AssetID material = {};
        DirectX::XMFLOAT4X4 world = {};
    };

    bool Init(GraphicsDevice& device);
    void Shutdown();

    // フレーム毎の更新。instances が空なら Bindings() は無効 (IsValid()==false) になる
    void Update(const std::vector<InstanceDesc>& instances, RenderResources& resources);

    const RtSceneBindings& Bindings() const { return bindings_; }
    int32_t InstanceCount() const { return bindings_.instanceCount; }
    int32_t TriangleCount() const { return triCount_; }
    // 直近の Update の CPU 時間 (TLAS 構築 + アップロード。ProfilerWindow 表示用)
    float BuildCpuMs() const { return buildMs_; }

private:
    // 要素数つきの構造化バッファ (容量が足りなくなったら作り直す)
    struct GpuArray {
        Microsoft::WRL::ComPtr<ID3D11Buffer> buf;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        uint32_t capacity = 0;
    };

    bool Upload(GpuArray& arr, const void* data, uint32_t elemSize, uint32_t count);
    // 参照メッシュ集合が変わったときだけ BLAS を焼き直して連結バッファを作る
    void RebuildBlasIfNeeded(const std::vector<InstanceDesc>& instances,
                             RenderResources& resources);

    GraphicsDevice* device_ = nullptr;

    // メッシュ AssetID → 連結配列での位置。BLAS 本体はキャッシュのみで GPU 転送後は不要
    struct BlasSlot {
        int32_t nodeBase = 0;
        int32_t triBase = 0;
        bool valid = false;
    };
    std::unordered_map<uint64_t, BlasSlot> blasSlots_;
    std::vector<uint64_t> blasKeys_; // 前回焼いたメッシュ集合 (ソート済み。差分検知用)

    GpuArray nodes_, tris_, attrs_, tlas_, instances_, materials_;
    RtSceneBindings bindings_;
    int32_t triCount_ = 0;
    float buildMs_ = 0.0f;

    // フレーム毎のスクラッチ (アロケーション回避)
    std::vector<RtBvhNode> nodeScratch_;
    std::vector<RtTri> triScratch_;
    std::vector<RtTriAttr> attrScratch_;
    std::vector<RtAabb> boundScratch_;
    std::vector<RtBvhNode> tlasScratch_;
    std::vector<int32_t> orderScratch_;
    std::vector<RtInstance> instScratch_;
    std::vector<RtMaterial> matScratch_;
    std::vector<uint64_t> meshKeyScratch_;
};

} // namespace mye
