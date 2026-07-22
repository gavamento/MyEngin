#pragma once
#include <cstdint>

#include "Engine/Renderer/RenderTypes.h"

namespace mye {

class World;
class GraphicsDevice;
class ShaderManager;
struct RenderResources;

struct ParticleStats {
    uint32_t aliveTotal = 0; // CPU は正確、GPU は放出累計ベースの推定
    float updateMs = 0.0f;   // CPU: CPU 時間 / GPU: GpuTimer 計測
};

// パーティクルバックエンドの切替インターフェース (engine_spec.md 7.2)。
// CPU / GPU の両実装が同じ ParticleEmitterComponent 定義データを解釈する。
// 切替時は Reset() → 再スタート (生存パーティクルは破棄 — 初期実装の仕様)
class IParticleBackend {
public:
    virtual ~IParticleBackend() = default;
    virtual const char* Name() const = 0;
    virtual bool Init(GraphicsDevice& device, ShaderManager& shaders) = 0;
    virtual void Shutdown() = 0;
    virtual void Reset() = 0;

    // 固定 tick で呼ばれる (フェーズ 4)。World からエミッタを列挙して同期・更新する
    virtual void Update(World& world, float dt) = 0;

    // シーン描画後に呼ばれる (両レンダリングパス共通の Forward 後段)。
    // renderOffsetX: 比較モードで横に並べて表示するためのオフセット
    // resources: エミッタのテクスチャ (M32b フリップブック) 解決に使う
    virtual void Render(GraphicsDevice& device, const RenderView& view, ShaderManager& shaders,
                        RenderResources& resources, float renderOffsetX) = 0;

    virtual ParticleStats Stats() const = 0;
};

} // namespace mye
