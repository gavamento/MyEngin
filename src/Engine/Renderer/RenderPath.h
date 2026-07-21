#pragma once
#include "Engine/Renderer/RenderTypes.h"

namespace mye {

class GraphicsDevice;
class ShaderManager;
struct RenderResources;

// レンダリングパスの差し替え点 (M1 で ForwardPath、M6.5 で DeferredPath)。
// Engine 層はこのインターフェースのみに依存する。
// パスは「ソート済みキューを受け取り提出する」だけで、収集は RenderSystem の仕事。
class IRenderPath {
public:
    virtual ~IRenderPath() = default;
    virtual const char* Name() const = 0;
    virtual bool Init(GraphicsDevice& device, ShaderManager& shaders) = 0;
    virtual void Shutdown() = 0;

    // view.rtv/dsv へ描画する (クリア込み)。queue はソート済みであること
    virtual void Render(GraphicsDevice& device, const RenderView& view, const RenderQueue& queue,
                        const SceneLightData& lights, RenderResources& resources,
                        ShaderManager& shaders) = 0;
};

} // namespace mye
