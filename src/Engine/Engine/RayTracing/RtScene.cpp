#include "Engine/Engine/RayTracing/RtScene.h"

#include <algorithm>
#include <chrono>

#include "Engine/Core/Log.h"
#include "Engine/Engine/Physics/MeshColliderLibrary.h"
#include "Engine/Renderer/FrustumCull.h"
#include "Engine/Renderer/GpuBufferUtil.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/RenderTypes.h"

using namespace DirectX;

namespace mye {

bool RtScene::Init(GraphicsDevice& device)
{
    device_ = &device;
    return true;
}

void RtScene::Shutdown()
{
    blasSlots_.clear();
    blasKeys_.clear();
    nodes_ = {};
    tris_ = {};
    attrs_ = {};
    tlas_ = {};
    instances_ = {};
    materials_ = {};
    bindings_ = {};
    device_ = nullptr;
}

bool RtScene::Upload(GpuArray& arr, const void* data, uint32_t elemSize, uint32_t count)
{
    if (!device_ || count == 0) {
        return false;
    }
    if (arr.capacity < count) {
        // 作り直し (伸長は 1.5 倍 + 余裕。毎フレームの再確保を避ける)
        arr.buf.Reset();
        arr.srv.Reset();
        arr.capacity = 0;
        const uint32_t cap = count + count / 2 + 16;
        if (!gpubuf::CreateStructured(device_->Device(), elemSize, cap, nullptr, 0, arr.buf,
                                      nullptr, &arr.srv)) {
            MYE_LOG_ERROR("RtScene: structured buffer creation failed (%u x %u B)", cap, elemSize);
            return false;
        }
        arr.capacity = cap;
    }
    D3D11_BOX box = {};
    box.right = elemSize * count;
    box.bottom = 1;
    box.back = 1;
    device_->Context()->UpdateSubresource(arr.buf.Get(), 0, &box, data, 0, 0);
    return true;
}

void RtScene::RebuildBlasIfNeeded(const std::vector<InstanceDesc>& instances,
                                  RenderResources& resources)
{
    // 参照されているメッシュ集合 (ソート済み・重複なし) を作り、前回と同じなら何もしない
    meshKeyScratch_.clear();
    for (const InstanceDesc& d : instances) {
        if (!d.mesh.IsNull()) {
            meshKeyScratch_.push_back(d.mesh.value);
        }
    }
    std::sort(meshKeyScratch_.begin(), meshKeyScratch_.end());
    meshKeyScratch_.erase(std::unique(meshKeyScratch_.begin(), meshKeyScratch_.end()),
                          meshKeyScratch_.end());
    if (meshKeyScratch_ == blasKeys_) {
        return;
    }

    blasSlots_.clear();
    nodeScratch_.clear();
    triScratch_.clear();
    attrScratch_.clear();

    RtBlas blas;
    for (const uint64_t key : meshKeyScratch_) {
        const AssetID id{ key };
        // BVH は物理のメッシュコライダーと共用 (lazy 構築 + キャッシュ済み)
        const MeshColliderData* md = meshcol::Resolve(id);
        const Mesh* mesh = resources.meshes.Get(id);
        if (!md || md->nodes.empty() || !mesh) {
            continue; // CPU 頂点なし / 未登録メッシュはレイトレ対象外
        }
        FlattenBlas(*md, mesh->normals, mesh->uvs, blas);
        if (blas.nodes.empty()) {
            continue;
        }

        const int32_t nodeBase = static_cast<int32_t>(nodeScratch_.size());
        const int32_t triBase = static_cast<int32_t>(triScratch_.size());
        for (RtBvhNode n : blas.nodes) {
            if (n.left < 0) { // 葉: 三角形範囲を連結後の位置へ
                n.left = -((-n.left - 1) + triBase + 1);
            } else { // 内部: 子 index を連結後の位置へ
                n.left += nodeBase;
                n.right += nodeBase;
            }
            nodeScratch_.push_back(n);
        }
        triScratch_.insert(triScratch_.end(), blas.tris.begin(), blas.tris.end());
        attrScratch_.insert(attrScratch_.end(), blas.attrs.begin(), blas.attrs.end());
        blasSlots_[key] = BlasSlot{ nodeBase, triBase, true };
    }
    blasKeys_ = meshKeyScratch_;
    triCount_ = static_cast<int32_t>(triScratch_.size());

    if (nodeScratch_.empty()) {
        return;
    }
    Upload(nodes_, nodeScratch_.data(), sizeof(RtBvhNode),
           static_cast<uint32_t>(nodeScratch_.size()));
    Upload(tris_, triScratch_.data(), sizeof(RtTri), static_cast<uint32_t>(triScratch_.size()));
    Upload(attrs_, attrScratch_.data(), sizeof(RtTriAttr),
           static_cast<uint32_t>(attrScratch_.size()));
}

void RtScene::Update(const std::vector<InstanceDesc>& instances, RenderResources& resources)
{
    bindings_ = RtSceneBindings{};
    buildMs_ = 0.0f;
    if (!device_ || instances.empty()) {
        return;
    }
    const auto t0 = std::chrono::high_resolution_clock::now();

    RebuildBlasIfNeeded(instances, resources);
    if (blasSlots_.empty() || !nodes_.srv) {
        return;
    }

    // インスタンス + ワールド AABB + マテリアル (マテリアルはインスタンスと 1:1。
    // 重複排除しないぶん単純で、32B/インスタンスなので実害はない)
    boundScratch_.clear();
    instScratch_.clear();
    matScratch_.clear();
    for (const InstanceDesc& d : instances) {
        const auto slot = blasSlots_.find(d.mesh.value);
        if (slot == blasSlots_.end() || !slot->second.valid) {
            continue;
        }
        const Mesh* mesh = resources.meshes.Get(d.mesh);
        if (!mesh) {
            continue;
        }
        RtAabb ab;
        WorldAabb(d.world, mesh->aabbMin, mesh->aabbMax, ab.min, ab.max);
        boundScratch_.push_back(ab);

        // worldToLocal (行ベクトル規約 4x3)。方向は正規化せずに使うので t はワールドのまま
        XMFLOAT4X4 wi;
        XMStoreFloat4x4(&wi, XMMatrixInverse(nullptr, XMLoadFloat4x4(&d.world)));
        RtInstance inst;
        inst.invRow0 = { wi._11, wi._12, wi._13, 0.0f };
        inst.invRow1 = { wi._21, wi._22, wi._23, 0.0f };
        inst.invRow2 = { wi._31, wi._32, wi._33, 0.0f };
        inst.invRow3 = { wi._41, wi._42, wi._43, 0.0f };
        inst.blasRoot = slot->second.nodeBase;
        inst.materialIndex = static_cast<int32_t>(matScratch_.size());
        instScratch_.push_back(inst);

        RtMaterial rm;
        if (const Material* mat = resources.materials.Get(d.material)) {
            const XMFLOAT3 lin = SrgbToLinear(XMFLOAT3{ mat->baseColor.x, mat->baseColor.y,
                                                        mat->baseColor.z });
            rm.baseColor = lin;
            rm.metallic = mat->metallic;
            rm.roughness = mat->roughness;
            // M46i: 自己発光。放射輝度 = リニア baseColor * 強度。ラスタ側 (gbMaterial.b) と
            // 同じ「albedo に強度を掛ける」規約なので、発光面をラスタで見た明るさと
            // その面がバウンス先で光源として振る舞う明るさが一致する
            rm.emissive = { lin.x * mat->emissiveIntensity, lin.y * mat->emissiveIntensity,
                            lin.z * mat->emissiveIntensity };
        }
        matScratch_.push_back(rm);
    }
    if (instScratch_.empty()) {
        return;
    }

    // TLAS を組み、葉が連続範囲を指せるようインスタンス/マテリアルをその順に並べ替える
    BuildTlas(boundScratch_, tlasScratch_, orderScratch_);
    if (tlasScratch_.empty() || orderScratch_.size() != instScratch_.size()) {
        return;
    }
    {
        std::vector<RtInstance> reordered(instScratch_.size());
        for (size_t i = 0; i < orderScratch_.size(); ++i) {
            reordered[i] = instScratch_[static_cast<size_t>(orderScratch_[i])];
        }
        instScratch_.swap(reordered);
    }

    const uint32_t instCount = static_cast<uint32_t>(instScratch_.size());
    const bool ok =
        Upload(tlas_, tlasScratch_.data(), sizeof(RtBvhNode),
               static_cast<uint32_t>(tlasScratch_.size()))
        && Upload(instances_, instScratch_.data(), sizeof(RtInstance), instCount)
        && Upload(materials_, matScratch_.data(), sizeof(RtMaterial),
                  static_cast<uint32_t>(matScratch_.size()));
    if (!ok) {
        return;
    }

    bindings_.nodes = nodes_.srv.Get();
    bindings_.tris = tris_.srv.Get();
    bindings_.attrs = attrs_.srv.Get();
    bindings_.tlas = tlas_.srv.Get();
    bindings_.instances = instances_.srv.Get();
    bindings_.materials = materials_.srv.Get();
    bindings_.instanceCount = static_cast<int32_t>(instCount);

    const auto t1 = std::chrono::high_resolution_clock::now();
    buildMs_ = std::chrono::duration<float, std::milli>(t1 - t0).count();
}

} // namespace mye
