#include "Engine/Renderer/Pipeline/PerfDrawBenchmark.h"

#include <algorithm>
#include <chrono>
#include <vector>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace mye {

double RunDrawSubmissionBenchmark(int warmups, int samples)
{
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &featureLevel, 1,
                                 D3D11_SDK_VERSION, &device, nullptr, &context))) return -1.0;
    static constexpr char kShader[] =
        "float4 vs(uint id:SV_VertexID):SV_Position {"
        "float2 p=float2((id==2)?3:-1,(id==1)?3:-1);return float4(p,0,1); }"
        "float4 ps():SV_Target { return float4(1,0,0,1); }";
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob;
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, nullptr, nullptr, nullptr, "vs", "vs_5_0",
                          0, 0, &vsBlob, nullptr))
        || FAILED(D3DCompile(kShader, sizeof(kShader) - 1, nullptr, nullptr, nullptr, "ps", "ps_5_0",
                             0, 0, &psBlob, nullptr))) return -1.0;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader;
    if (FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                                          &vertexShader))
        || FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr,
                                           &pixelShader))) return -1.0;
    D3D11_TEXTURE2D_DESC textureDesc = {};
    textureDesc.Width = 1;
    textureDesc.Height = 1;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
    if (FAILED(device->CreateTexture2D(&textureDesc, nullptr, &texture))
        || FAILED(device->CreateRenderTargetView(texture.Get(), nullptr, &target))) return -1.0;
    ID3D11RenderTargetView* view = target.Get();
    context->OMSetRenderTargets(1, &view, nullptr);
    context->VSSetShader(vertexShader.Get(), nullptr, 0);
    context->PSSetShader(pixelShader.Get(), nullptr, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_VIEWPORT viewport = {};
    viewport.Width = 1.0f;
    viewport.Height = 1.0f;
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);

    // No Flush or GPU completion wait: this is command submission CPU time.
    std::vector<double> timings;
    for (int trial = -warmups; trial < samples; ++trial) {
        const auto begin = std::chrono::steady_clock::now();
        for (int draw = 0; draw < 1000; ++draw) context->Draw(3, 0);
        const auto end = std::chrono::steady_clock::now();
        if (trial >= 0) timings.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
    }
    std::sort(timings.begin(), timings.end());
    return timings.empty() ? -1.0 : timings[timings.size() / 2];
}

} // namespace mye
