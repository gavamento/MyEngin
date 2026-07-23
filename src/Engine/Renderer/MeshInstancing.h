#pragma once
#include <cstdint>
#include <vector>

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Renderer/RenderTypes.h"

namespace mye {

class GraphicsDevice;

// ---- メッシュ GPU インスタンシング (M38f) ----
// ソート済み opaque キュー (material → mesh → viewZ) の連続 run を 1 回の
// DrawIndexedInstanced に束ねる。ワールド行列は StructuredBuffer + SV_InstanceID で引く
// (CpuParticleBackend と同じ方式 — IA レイアウト変更不要)。
// D3D11 の SV_InstanceID は StartInstanceLocation に関係なく常に 0 始まりのため、
// バッファ内オフセットは CB の instanceBase で渡す (粒子の baseIndex と同じ)。

// 検出された run 1 個。first/count は items 内の範囲、base はインスタンスバッファ内の開始位置
struct MeshInstanceRun {
    size_t first = 0;
    uint32_t count = 0; // 常に >= 2 (単発は従来の per-item 描画のまま)
    uint32_t base = 0;
};

// 連続 run 検出 (純関数、決定論: items の並び順のみに依存)。
// canInstance[i] = false の項目は run に入らず境界にもなる (スキン/シェーダ差替/欠損リソース)。
// run の条件: 同一 material かつ同一 mesh が 2 件以上連続。
// outWorlds には run に入った項目のワールド行列を run 順・項目順で積む (row-major のまま —
// シェーダ側は row_major float4x4 の StructuredBuffer で受けるので転置不要)。
inline void BuildInstanceRuns(const std::vector<RenderItem>& items,
                              const std::vector<uint8_t>& canInstance,
                              std::vector<MeshInstanceRun>& outRuns,
                              std::vector<DirectX::XMFLOAT4X4>& outWorlds)
{
    outRuns.clear();
    outWorlds.clear();
    size_t i = 0;
    while (i < items.size()) {
        if (!canInstance[i]) {
            ++i;
            continue;
        }
        size_t j = i + 1;
        while (j < items.size() && canInstance[j]
               && items[j].material.value == items[i].material.value
               && items[j].mesh.value == items[i].mesh.value) {
            ++j;
        }
        if (j - i >= 2) {
            MeshInstanceRun run;
            run.first = i;
            run.count = static_cast<uint32_t>(j - i);
            run.base = static_cast<uint32_t>(outWorlds.size());
            outRuns.push_back(run);
            for (size_t k = i; k < j; ++k) {
                outWorlds.push_back(items[k].world);
            }
        }
        i = j;
    }
}

// ワールド行列列を DYNAMIC StructuredBuffer へ書き込む (容量成長 + SRV 供給)。
// 各パス (Forward/Deferred/Shadow) が 1 個ずつ所有する
class MeshInstanceBuffer {
public:
    bool Upload(GraphicsDevice& device, const std::vector<DirectX::XMFLOAT4X4>& worlds);
    ID3D11ShaderResourceView* SRV() const { return srv_.Get(); }
    void Reset()
    {
        buffer_.Reset();
        srv_.Reset();
        capacity_ = 0;
    }

private:
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
    uint32_t capacity_ = 0;
};

} // namespace mye
