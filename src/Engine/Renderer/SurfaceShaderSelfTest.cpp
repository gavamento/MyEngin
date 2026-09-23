//====================================================================================
//                          SurfaceShaderSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                     09/24/2026
//                                          M79 sub-01: 規約・生成エントリの成立確認
//====================================================================================
#include "Engine/Renderer/SurfaceShaderSelfTest.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ShaderManager.h"
#include "Engine/Renderer/SurfaceProgram.h"
#include "Engine/Renderer/SurfaceShaderTypes.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace mye {
namespace {

int g_fails = 0;

void Check(bool cond, const char* what)
{
    if (cond) {
        MYE_LOG_INFO("  PASS: %s", what);
    } else {
        MYE_LOG_ERROR("  FAIL: %s", what);
        ++g_fails;
    }
}

// ---- フィクスチャ HLSL (規約どおり / 規約違反 / 実描画プローブ) ----

// 規約どおり: Properties + MyEnginePerMaterial + 作者 Texture2D + ヘルパ (MyeSunShadow/MyeApplyFog)
// を一通り使い、名前解決とレイアウト検査の材料にする
const char* kRegGoodHlsl = R"HLSL(
/*@MyEngineProperties
_Tint ("Tint", Color) = (1,1,1,1)
_Amp ("Amplitude", Range(0,1)) = 0.1
@*/
#include "MyEngineSurface.hlsli"

cbuffer MyEnginePerMaterial
{
    float4 _Tint;
    float _Amp;
};

Texture2D _MainTex;

struct VSIn
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
};

struct VSOut
{
    float4 pos : SV_Position;
    float3 normalW : NORMAL;
};

VSOut VSMain(VSIn v)
{
    VSOut o;
    float3 p = v.pos + v.normal * (_Amp * sin(gTime));
    float4 posW = mul(float4(p, 1.0f), gWorld);
    o.pos = mul(posW, gViewProj);
    o.normalW = v.normal;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float shadow = MyeSunShadow(i.pos.xyz);
    float3 lit = MyeApplyFog(_Tint.rgb * shadow, i.pos.xyz);
    float4 tex = _MainTex.Sample(gSampler, float2(0.5f, 0.5f));
    return float4(lit, _Tint.a) * tex;
}
)HLSL";

// 規約違反: VSOut のメンバ名が pos ではなく position (型・意味は同じでもエンジンの
// 生成エントリは名前で curOut.pos / i.myeInner.pos を読むためコンパイルが落ちる
const char* kRegBadHlsl = R"HLSL(
#include "MyEngineSurface.hlsli"

struct VSIn
{
    float3 pos : POSITION;
};

struct VSOut
{
    float4 position : SV_Position;
};

VSOut VSMain(VSIn v)
{
    VSOut o;
    o.position = mul(mul(float4(v.pos, 1.0f), gWorld), gViewProj);
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    return float4(1.0f, 1.0f, 1.0f, 1.0f);
}
)HLSL";

// 実描画プローブ: VSIn を意図的に「TEXCOORD0 が先・POSITION が後・NORMAL 省略」の
// 部分集合・順不同にし、MeshVertex 固定オフセットの入力レイアウトを実描画で検証する。
// 頂点変位 (0.1f*sin(gTime)) を static gTime に乗せ、速度エントリの前後 2 回評価で
// 別の値 (gWorld と gTime の両方) を使うことを read-back で確認する
const char* kVelocityProbeHlsl = R"HLSL(
#include "MyEngineSurface.hlsli"

struct VSIn
{
    float2 uv : TEXCOORD0;
    float3 pos : POSITION;
};

struct VSOut
{
    float4 pos : SV_Position;
};

VSOut VSMain(VSIn v)
{
    VSOut o;
    float3 p = v.pos;
    p.y += 0.1f * sin(gTime);
    float4 posW = mul(float4(p, 1.0f), gWorld);
    o.pos = mul(posW, gViewProj);
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    return float4(1.0f, 1.0f, 1.0f, 1.0f);
}
)HLSL";

void WriteFile(const std::filesystem::path& path, const char* content)
{
    std::ofstream f(path, std::ios::binary);
    f << content;
}

// ---- 予約 CB のリフレクション検査 (D3DReflect のオフセットと C++ の offsetof を照合) ----

bool FindVar(const SurfaceEntryReflection& refl, const char* name, SurfaceReflectedVar& out)
{
    const auto it = refl.vars.find(name);
    if (it == refl.vars.end()) {
        return false;
    }
    out = it->second;
    return true;
}

void CheckVarOffset(const SurfaceEntryReflection& refl, const char* name, size_t expectOffset,
                    size_t expectSize, const char* what)
{
    SurfaceReflectedVar v;
    if (!FindVar(refl, name, v)) {
        MYE_LOG_ERROR("  FAIL: %s (variable '%s' not found in reflection)", what, name);
        ++g_fails;
        return;
    }
    Check(v.offset == expectOffset && v.size == expectSize, what);
    if (v.offset != expectOffset || v.size != expectSize) {
        MYE_LOG_ERROR("    reflected offset=%u size=%u / expected offset=%zu size=%zu",
                      v.offset, v.size, expectOffset, expectSize);
    }
}

bool CreateDynamicCB(ID3D11Device* dev, UINT size, ComPtr<ID3D11Buffer>& out)
{
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = size;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(dev->CreateBuffer(&bd, nullptr, out.GetAddressOf()));
}

template <typename T>
void UploadCB(ID3D11DeviceContext* dc, ID3D11Buffer* cb, const T& data)
{
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(dc->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &data, sizeof(T));
        dc->Unmap(cb, 0);
    }
}

} // namespace

bool RunSurfaceShaderSelfTest()
{
    MYE_LOG_INFO("==== M79 surface shader self test (sub-01) ====");
    g_fails = 0;

    GraphicsDevice device;
    if (!device.Init(true)) {
        MYE_LOG_ERROR("  FAIL: WARP device init");
        return false;
    }

    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / L"mye_surface_shader_selftest";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    WriteFile(dir / L"RegGood.surface.hlsl", kRegGoodHlsl);
    WriteFile(dir / L"RegBad.surface.hlsl", kRegBadHlsl);
    WriteFile(dir / L"VelocityProbe.surface.hlsl", kVelocityProbeHlsl);

    std::vector<std::wstring> dirs = { dir.wstring() };
    const std::wstring engineShaderDir = FindEngineShaderDir();
    Check(!engineShaderDir.empty(), "engine shader dir found (needed for MyEngineSurface.hlsli)");
    if (!engineShaderDir.empty()) {
        dirs.push_back(engineShaderDir);
    }

    ShaderManager shaders;
    Check(shaders.Init(device, dirs), "shader manager init");

    // ---- 1. 規約どおりのフィクスチャがコンパイルできる ----
    const AssetID goodId = shaders.LoadSurface("RegGood.surface");
    SurfaceProgram* good = shaders.GetSurface(goodId);
    Check(good != nullptr && good->valid, "RegGood.surface compiles (color/velocity/shadow)");
    if (good && !good->valid) {
        MYE_LOG_ERROR("  RegGood error: %s", good->errorMessage.c_str());
    }

    if (good && good->valid) {
        // ---- 2. 予約 CB / 予約テクスチャ / 作者資源が名前で解決される ----
        Check(good->colorPSReflect.resources.contains("_MainTex"), "author Texture2D '_MainTex' resolved by name");
        Check(good->colorPSReflect.resources.contains("MyEnginePerFrame"), "MyEnginePerFrame resolved (PS, via helpers)");
        Check(good->colorVSReflect.resources.contains("MyEngineSurfaceFrame"), "MyEngineSurfaceFrame resolved (VS, generated entry)");
        Check(good->colorVSReflect.resources.contains("MyEnginePerObject"), "MyEnginePerObject resolved (VS, generated entry)");
        Check(good->velocityVSReflect.resources.contains("MyEngineSurfaceFrame"), "MyEngineSurfaceFrame resolved (velocity VS)");
        Check(good->shadowVSReflect.resources.contains("MyEngineSurfaceFrame"), "MyEngineSurfaceFrame resolved (shadow VS)");

        // MyEnginePerMaterial の各プロパティのオフセット (HLSL 既定パック: float4 → 0, float → 16)
        CheckVarOffset(good->colorPSReflect, "_Tint", 0, 16, "MyEnginePerMaterial._Tint offset/size");
        CheckVarOffset(good->colorPSReflect, "_Amp", 16, 4, "MyEnginePerMaterial._Amp offset/size");

        // ---- 5. 予約 CB の HLSL レイアウトが C++ 構造体と一致 (変数ごと) ----
        // MyEngineSurfaceFrame / MyEnginePerObject / MyEngineWater は新規なので全フィールド、
        // MyEnginePerFrame は既存 PerFrame の移植 (総サイズ + 代表フィールドで検査)
        const SurfaceEntryReflection& vsr = good->colorVSReflect;
        CheckVarOffset(vsr, "gMyeCurViewProj", offsetof(MyEngineSurfaceFrameCB, curViewProj), 64,
                       "MyEngineSurfaceFrame.gMyeCurViewProj");
        CheckVarOffset(vsr, "gMyePrevViewProj", offsetof(MyEngineSurfaceFrameCB, prevViewProj), 64,
                       "MyEngineSurfaceFrame.gMyePrevViewProj");
        CheckVarOffset(vsr, "gMyeShadowViewProj", offsetof(MyEngineSurfaceFrameCB, shadowViewProj),
                       64, "MyEngineSurfaceFrame.gMyeShadowViewProj");
        CheckVarOffset(vsr, "gMyeCurTime", offsetof(MyEngineSurfaceFrameCB, curTime), 4,
                       "MyEngineSurfaceFrame.gMyeCurTime");
        CheckVarOffset(vsr, "gMyePrevTime", offsetof(MyEngineSurfaceFrameCB, prevTime), 4,
                       "MyEngineSurfaceFrame.gMyePrevTime");
        CheckVarOffset(vsr, "gMyeCurWaterTime", offsetof(MyEngineSurfaceFrameCB, curWaterTime), 4,
                       "MyEngineSurfaceFrame.gMyeCurWaterTime");
        CheckVarOffset(vsr, "gMyePrevWaterTime", offsetof(MyEngineSurfaceFrameCB, prevWaterTime), 4,
                       "MyEngineSurfaceFrame.gMyePrevWaterTime");
        CheckVarOffset(vsr, "gMyeWorld", offsetof(MyEnginePerObjectCB, world), 64,
                       "MyEnginePerObject.gMyeWorld");
        CheckVarOffset(vsr, "gBaseColor", offsetof(MyEnginePerObjectCB, baseColor), 16,
                       "MyEnginePerObject.gBaseColor");

        const SurfaceEntryReflection& velVsr = good->velocityVSReflect;
        CheckVarOffset(velVsr, "gMyePrevWorld", offsetof(MyEnginePerObjectCB, prevWorld), 64,
                       "MyEnginePerObject.gMyePrevWorld");

        const SurfaceEntryReflection& velPsr = good->velocityPSReflect;
        CheckVarOffset(velPsr, "gMyeJitterNdc", offsetof(MyEngineSurfaceFrameCB, jitterNdc), 8,
                       "MyEngineSurfaceFrame.gMyeJitterNdc");
        CheckVarOffset(velPsr, "gMyeHistoryValid", offsetof(MyEngineSurfaceFrameCB, historyValid),
                       4, "MyEngineSurfaceFrame.gMyeHistoryValid");

        // MyEnginePerFrame: 総サイズ + 代表フィールド (先頭・中間・末尾付近)
        SurfaceReflectedVar anyPerFrameVar;
        const bool hasPerFrameVar = FindVar(good->colorPSReflect, "gFogColor", anyPerFrameVar);
        Check(hasPerFrameVar, "MyEnginePerFrame variable 'gFogColor' found");
        if (hasPerFrameVar) {
            Check(anyPerFrameVar.cbufSize == sizeof(MyEnginePerFrameCB),
                  "MyEnginePerFrame total cbuffer size matches MyEnginePerFrameCB");
            if (anyPerFrameVar.cbufSize != sizeof(MyEnginePerFrameCB)) {
                MYE_LOG_ERROR("    reflected cbufSize=%u / sizeof(MyEnginePerFrameCB)=%zu",
                             anyPerFrameVar.cbufSize, sizeof(MyEnginePerFrameCB));
            }
        }
        CheckVarOffset(good->colorPSReflect, "gCameraPos", offsetof(MyEnginePerFrameCB, cameraPos),
                       12, "MyEnginePerFrame.gCameraPos (先頭)");
        CheckVarOffset(good->colorPSReflect, "gShadowVP", offsetof(MyEnginePerFrameCB, shadowVP), 64,
                       "MyEnginePerFrame.gShadowVP");
        CheckVarOffset(good->colorPSReflect, "gShadowVP12", offsetof(MyEnginePerFrameCB, shadowVP12),
                       128, "MyEnginePerFrame.gShadowVP12 (中間)");
        CheckVarOffset(good->colorPSReflect, "gSunColor", offsetof(MyEnginePerFrameCB, sunColor), 12,
                       "MyEnginePerFrame.gSunColor (末尾寄り)");
    }

    // ---- 3. 規約違反フィクスチャは「サーフェス規約」付きで失敗し、落ちない ----
    const AssetID badId = shaders.LoadSurface("RegBad.surface");
    SurfaceProgram* bad = shaders.GetSurface(badId);
    Check(bad != nullptr && !bad->valid, "RegBad.surface (pos member renamed) fails to compile");
    if (bad) {
        Check(bad->errorMessage.find("サーフェス規約") != std::string::npos,
              "RegBad.surface error message is prefixed with 'サーフェス規約'");
    }

    // ---- 4 & 1 (risk): 実描画で入力レイアウト (MeshVertex 固定オフセット・部分集合・順不同) と
    //      速度エントリの前後 2 回評価 (static 再代入) を検証する ----
    const AssetID probeId = shaders.LoadSurface("VelocityProbe.surface");
    SurfaceProgram* probe = shaders.GetSurface(probeId);
    Check(probe != nullptr && probe->valid, "VelocityProbe.surface compiles");

    if (probe && probe->valid) {
        ID3D11Device* dev = device.Device();
        ID3D11DeviceContext* dc = device.Context();

        // 1x1 の色・速度ターゲット (NDC (0,0) がそのままピクセル中心になるサイズ)
        ComPtr<ID3D11Texture2D> colorTex;
        ComPtr<ID3D11RenderTargetView> colorRtv;
        ComPtr<ID3D11Texture2D> velocityTex;
        ComPtr<ID3D11RenderTargetView> velocityRtv;
        ComPtr<ID3D11Texture2D> velocityStaging;

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = 1;
        td.Height = 1;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.SampleDesc = { 1, 0 };
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;

        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        bool ok = SUCCEEDED(dev->CreateTexture2D(&td, nullptr, colorTex.GetAddressOf()));
        ok = ok && SUCCEEDED(dev->CreateRenderTargetView(colorTex.Get(), nullptr, colorRtv.GetAddressOf()));

        td.Format = DXGI_FORMAT_R32G32_FLOAT;
        ok = ok && SUCCEEDED(dev->CreateTexture2D(&td, nullptr, velocityTex.GetAddressOf()));
        ok = ok
            && SUCCEEDED(
                   dev->CreateRenderTargetView(velocityTex.Get(), nullptr, velocityRtv.GetAddressOf()));

        D3D11_TEXTURE2D_DESC sd = td;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ok = ok && SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, velocityStaging.GetAddressOf()));
        Check(ok, "velocity probe render targets created");

        // 頂点バッファ: TEXCOORD0 に巨大な番兵値 (12345,67890) を入れる。
        // 固定オフセット表が間違っていて POSITION の代わりにこの値を読むと、
        // NDC が画面外に吹き飛んでラスタライズされず read-back が既定値のままになる
        std::vector<MeshVertex> verts(3);
        verts[0].position = { -3.0f, -3.0f, 0.0f };
        verts[1].position = { 3.0f, -3.0f, 0.0f };
        verts[2].position = { 0.0f, 3.0f, 0.0f };
        for (MeshVertex& v : verts) {
            v.uv = { 12345.0f, 67890.0f };
        }
        ComPtr<ID3D11Buffer> vb;
        D3D11_BUFFER_DESC vbd = {};
        vbd.ByteWidth = static_cast<UINT>(sizeof(MeshVertex) * verts.size());
        vbd.Usage = D3D11_USAGE_IMMUTABLE;
        vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA vinit = { verts.data(), 0, 0 };
        ok = SUCCEEDED(dev->CreateBuffer(&vbd, &vinit, vb.GetAddressOf()));
        Check(ok, "velocity probe vertex buffer created (MeshVertex layout, uv sentinel)");

        ComPtr<ID3D11Buffer> frameCb;
        ComPtr<ID3D11Buffer> objectCb;
        ok = CreateDynamicCB(dev, sizeof(MyEngineSurfaceFrameCB), frameCb);
        ok = ok && CreateDynamicCB(dev, sizeof(MyEnginePerObjectCB), objectCb);
        Check(ok, "velocity probe MyEngineSurfaceFrame/MyEnginePerObject CBs created");

        ComPtr<ID3D11RasterizerState> rs;
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        ok = SUCCEEDED(dev->CreateRasterizerState(&rd, rs.GetAddressOf()));
        Check(ok, "velocity probe rasterizer state (cull none) created");

        auto findSlot = [](const SurfaceEntryReflection& refl, const char* name) -> int {
            const auto it = refl.resources.find(name);
            return (it != refl.resources.end()) ? static_cast<int>(it->second.bindSlot) : -1;
        };
        const int frameSlotVs = findSlot(probe->velocityVSReflect, "MyEngineSurfaceFrame");
        const int objectSlotVs = findSlot(probe->velocityVSReflect, "MyEnginePerObject");
        const int frameSlotPs = findSlot(probe->velocityPSReflect, "MyEngineSurfaceFrame");
        Check(frameSlotVs >= 0 && objectSlotVs >= 0 && frameSlotPs >= 0,
              "velocity probe reserved CB slots resolved by name");

        // 描画 1 回ぶんの手順 (CB を差し替えて MyeVSVelocity/MyePSVelocity を 1 回 Draw する)
        auto drawAndReadback = [&](const MyEngineSurfaceFrameCB& frame, const MyEnginePerObjectCB& obj,
                                   XMFLOAT2& outVelocity) {
            UploadCB(dc, frameCb.Get(), frame);
            UploadCB(dc, objectCb.Get(), obj);

            ID3D11RenderTargetView* rtvs[2] = { colorRtv.Get(), velocityRtv.Get() };
            dc->OMSetRenderTargets(2, rtvs, nullptr);
            const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            const float clearVelocity[4] = { 999.0f, 999.0f, 0.0f, 0.0f }; // 番兵 (未カバー検出用)
            dc->ClearRenderTargetView(colorRtv.Get(), clearColor);
            dc->ClearRenderTargetView(velocityRtv.Get(), clearVelocity);

            D3D11_VIEWPORT vp = {};
            vp.Width = 1.0f;
            vp.Height = 1.0f;
            vp.MaxDepth = 1.0f;
            dc->RSSetViewports(1, &vp);
            dc->RSSetState(rs.Get());
            dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            dc->IASetInputLayout(probe->velocityInputLayout.Get());
            const UINT stride = sizeof(MeshVertex);
            const UINT offset = 0;
            ID3D11Buffer* vbRaw = vb.Get();
            dc->IASetVertexBuffers(0, 1, &vbRaw, &stride, &offset);
            dc->VSSetShader(probe->velocityVS.Get(), nullptr, 0);
            dc->PSSetShader(probe->velocityPS.Get(), nullptr, 0);

            ID3D11Buffer* frameCbRaw = frameCb.Get();
            ID3D11Buffer* objectCbRaw = objectCb.Get();
            dc->VSSetConstantBuffers(static_cast<UINT>(frameSlotVs), 1, &frameCbRaw);
            dc->VSSetConstantBuffers(static_cast<UINT>(objectSlotVs), 1, &objectCbRaw);
            dc->PSSetConstantBuffers(static_cast<UINT>(frameSlotPs), 1, &frameCbRaw);

            dc->Draw(3, 0);

            dc->CopyResource(velocityStaging.Get(), velocityTex.Get());
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            outVelocity = { 999.0f, 999.0f };
            if (SUCCEEDED(dc->Map(velocityStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                outVelocity = *reinterpret_cast<const XMFLOAT2*>(mapped.pData);
                dc->Unmap(velocityStaging.Get(), 0);
            }

            ID3D11RenderTargetView* nullRtvs[2] = { nullptr, nullptr };
            dc->OMSetRenderTargets(2, nullRtvs, nullptr);
        };

        if (ok) {
            // ---- draw A: World だけ差し替え (Time は cur==prev で相殺させる) ----
            // 期待: velocity = (-0.2, 0) — X 成分は「今 World=Identity / 前 World=平行移動 0.4」の
            // 差そのもの (ComputeVelocityUv の式どおり)。Y は時刻差なしなので厳密 0
            MyEngineSurfaceFrameCB frameA = {};
            XMStoreFloat4x4(&frameA.curViewProj, XMMatrixTranspose(XMMatrixIdentity()));
            XMStoreFloat4x4(&frameA.prevViewProj, XMMatrixTranspose(XMMatrixIdentity()));
            XMStoreFloat4x4(&frameA.shadowViewProj, XMMatrixTranspose(XMMatrixIdentity()));
            frameA.curTime = 0.7f;
            frameA.prevTime = 0.7f; // 同値 = 変位項が cur/prev で相殺する
            frameA.jitterNdc = { 0.0f, 0.0f };
            frameA.historyValid = 1;

            MyEnginePerObjectCB objA = {};
            XMStoreFloat4x4(&objA.world, XMMatrixTranspose(XMMatrixIdentity()));
            XMStoreFloat4x4(&objA.prevWorld, XMMatrixTranspose(XMMatrixTranslation(0.4f, 0.0f, 0.0f)));

            XMFLOAT2 velocityA = {};
            drawAndReadback(frameA, objA, velocityA);
            Check(std::fabs(velocityA.x - (-0.2f)) < 0.01f && std::fabs(velocityA.y - 0.0f) < 0.01f,
                  "velocity entry: World の前後差が正しく速度になる (2 回評価が別の gWorld を使う)");
            if (!(std::fabs(velocityA.x - (-0.2f)) < 0.01f && std::fabs(velocityA.y - 0.0f) < 0.01f)) {
                MYE_LOG_ERROR("    velocityA=(%f,%f) / expected=(-0.2,0)", velocityA.x, velocityA.y);
            }

            // ---- draw B: World は cur==prev (恒等) のまま、gTime だけ差し替える ----
            // 期待: velocity = (0, 0.05) — 変位項 0.1*sin(gTime) が cur=0 (sin=0) / prev=pi/2 (sin=1)
            // で 0.1 だけ差が付き、それがそのまま Y 速度になる (World 由来の成分はゼロ)
            MyEngineSurfaceFrameCB frameB = frameA;
            frameB.curTime = 0.0f;
            frameB.prevTime = 1.5707963f; // pi/2

            MyEnginePerObjectCB objB = {};
            XMStoreFloat4x4(&objB.world, XMMatrixTranspose(XMMatrixIdentity()));
            XMStoreFloat4x4(&objB.prevWorld, XMMatrixTranspose(XMMatrixIdentity()));

            XMFLOAT2 velocityB = {};
            drawAndReadback(frameB, objB, velocityB);
            Check(std::fabs(velocityB.x - 0.0f) < 0.01f && std::fabs(velocityB.y - 0.05f) < 0.01f,
                  "velocity entry: gTime の前後差が正しく速度になる (2 回評価が別の gTime を使う)");
            if (!(std::fabs(velocityB.x - 0.0f) < 0.01f && std::fabs(velocityB.y - 0.05f) < 0.01f)) {
                MYE_LOG_ERROR("    velocityB=(%f,%f) / expected=(0,0.05)", velocityB.x, velocityB.y);
            }
        }
    }

    device.Shutdown();
    fs::remove_all(dir, ec);

    if (g_fails != 0) {
        MYE_LOG_ERROR("==== M79 surface shader self test: %d FAILURE(S) ====", g_fails);
        return false;
    }
    MYE_LOG_INFO("==== M79 surface shader self test: ALL PASS ====");
    return true;
}

} // namespace mye
