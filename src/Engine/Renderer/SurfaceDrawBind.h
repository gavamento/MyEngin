//====================================================================================
//                          SurfaceDrawBind.h
//  MyEngin/ 秋田蓮音                                                     09/24/2026
//                                          サーフェスシェーダーの名前解決バインド共通処理
//====================================================================================
#pragma once
#include <d3d11.h>

#include "Engine/Renderer/SurfaceProgram.h"

namespace mye {

// name が VS/PS どちらかのリフレクション表に載っていれば、そのステージの該当スロットへ
// バインドする (両方に載っていれば両方へ)。載っていなければ何もしない。
// Forward/Deferred のサーフェス描画 (M79 sub-02/sub-03) が共有する

inline void BindSurfaceNamedCB(ID3D11DeviceContext* dc, const SurfaceEntryReflection& vsRefl,
                               const SurfaceEntryReflection& psRefl, const char* name,
                               ID3D11Buffer* cb)
{
    if (!cb) {
        return;
    }
    if (const auto it = vsRefl.resources.find(name); it != vsRefl.resources.end()) {
        dc->VSSetConstantBuffers(it->second.bindSlot, 1, &cb);
    }
    if (const auto it = psRefl.resources.find(name); it != psRefl.resources.end()) {
        dc->PSSetConstantBuffers(it->second.bindSlot, 1, &cb);
    }
}

inline void BindSurfaceNamedSRV(ID3D11DeviceContext* dc, const SurfaceEntryReflection& vsRefl,
                                const SurfaceEntryReflection& psRefl, const char* name,
                                ID3D11ShaderResourceView* srv)
{
    if (const auto it = vsRefl.resources.find(name); it != vsRefl.resources.end()) {
        dc->VSSetShaderResources(it->second.bindSlot, 1, &srv);
    }
    if (const auto it = psRefl.resources.find(name); it != psRefl.resources.end()) {
        dc->PSSetShaderResources(it->second.bindSlot, 1, &srv);
    }
}

inline void BindSurfaceNamedSampler(ID3D11DeviceContext* dc, const SurfaceEntryReflection& vsRefl,
                                    const SurfaceEntryReflection& psRefl, const char* name,
                                    ID3D11SamplerState* sampler)
{
    if (const auto it = vsRefl.resources.find(name); it != vsRefl.resources.end()) {
        dc->VSSetSamplers(it->second.bindSlot, 1, &sampler);
    }
    if (const auto it = psRefl.resources.find(name); it != psRefl.resources.end()) {
        dc->PSSetSamplers(it->second.bindSlot, 1, &sampler);
    }
}

} // namespace mye
