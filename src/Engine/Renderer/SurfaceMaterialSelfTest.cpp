//====================================================================================
//                          SurfaceMaterialSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                     09/24/2026
//                                          M79 sub-02: Forward サーフェス描画とマテリアルの回帰テスト
//====================================================================================
#include "Engine/Renderer/SurfaceMaterialSelfTest.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "nlohmann/json.hpp"

#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/ForwardPath.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ProjectShaderProperties.h"
#include "Engine/Renderer/ShaderManager.h"
#include "Engine/Renderer/SurfaceProgram.h"
#include "Engine/Renderer/SurfaceShaderTypes.h" // M79b-fix (review-1 #4): MyEnginePerFrameCB のサイズ比較用

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

void WriteFile(const std::filesystem::path& path, const std::string& content)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << content;
}

float ReadFloatAt(const std::vector<uint8_t>& data, size_t offset)
{
    float v = 0.0f;
    if (offset + sizeof(float) <= data.size()) {
        std::memcpy(&v, data.data() + offset, sizeof(float));
    }
    return v;
}

// ---- 1. PackPropertiesReflected: パース順ではなくリフレクションのオフセットで詰めるか ----
void TestPackPropertiesReflected()
{
    MYE_LOG_INFO("-- PackPropertiesReflected --");
    const char* kBlock =
        "/*@MyEngineProperties\n_A (\"A\", Float) = 1.0\n_B (\"B\", Color) = (2,2,2,2)\n@*/\n";
    const PropertyParseResult parsed = ParseProperties(kBlock);
    Check(parsed.ok, "properties block parses");
    Check(parsed.properties.size() == 2, "2 properties parsed");

    // 作者の cbuffer 宣言順はパース順 (_A, _B) と逆 (_B が offset0, _A が offset16) —
    // PackProperties (パース順オフセット) ならここで値が入れ違う
    std::unordered_map<std::string, ReflectedVarSlot> reflectionVars;
    reflectionVars["_A"] = { 16, 4 };
    reflectionVars["_B"] = { 0, 16 };

    std::unordered_map<std::string, PropValue> values;
    values["_A"] = 5.0f;
    values["_B"] = std::array<float, 4>{ 9.0f, 8.0f, 7.0f, 6.0f };

    std::vector<uint8_t> cbData;
    std::vector<std::string> missing;
    const bool ok = PackPropertiesReflected(parsed, values, reflectionVars, 32, cbData, &missing);
    Check(ok, "PackPropertiesReflected succeeds");
    Check(cbData.size() == 32, "cbData size == 32 (reflection の cbSizeBytes をそのまま使う)");
    Check(missing.empty(), "既知の名前はすべて解決される (missing 空)");
    Check(std::fabs(ReadFloatAt(cbData, 16) - 5.0f) < 1e-6f,
          "_A はリフレクションのオフセット (16) に書かれる (パース順の 0 ではない)");
    Check(std::fabs(ReadFloatAt(cbData, 0) - 9.0f) < 1e-6f
              && std::fabs(ReadFloatAt(cbData, 4) - 8.0f) < 1e-6f,
          "_B はリフレクションのオフセット (0) に書かれる (パース順の 16 ではない)");

    // reflectionVars に無い名前は missing に積まれ、CB には書かれない (該当オフセットは 0 のまま)
    std::unordered_map<std::string, ReflectedVarSlot> partial;
    partial["_A"] = { 0, 4 };
    std::vector<uint8_t> cbData2;
    std::vector<std::string> missing2;
    PackPropertiesReflected(parsed, values, partial, 16, cbData2, &missing2);
    Check(missing2.size() == 1 && missing2[0] == "_B",
          "cbuffer に無い名前 (_B) は missingOut に積まれる");
}

// ---- 2. ShaderManager: LoadSurface の冪等性・ホットリロード・バイトコードキャッシュ ----
const char* kSurfaceFixtureTemplate = R"HLSL(
/*@MyEngineProperties
_Tag ("Tag", Range(0,1)) = %f
@*/
#include "MyEngineSurface.hlsli"
cbuffer MyEnginePerMaterial { float _Tag; };
struct VSIn { float3 pos : POSITION; };
struct VSOut { float4 pos : SV_Position; };
VSOut VSMain(VSIn v)
{
    VSOut o;
    o.pos = mul(mul(float4(v.pos, 1.0f), gWorld), gViewProj);
    return o;
}
float4 PSMain(VSOut i) : SV_Target
{
    return float4(_Tag, _Tag, _Tag, 1.0f);
}
)HLSL";

std::string MakeFixture(float tag)
{
    char buf[2048];
    snprintf(buf, sizeof(buf), kSurfaceFixtureTemplate, static_cast<double>(tag));
    return buf;
}

const char* kBrokenFixture = R"HLSL(
#include "MyEngineSurface.hlsli"
struct VSIn { float3 pos : POSITION; };
struct VSOut { float4 pos : SV_Position; };
VSOut VSMain(VSIn v)
{
    VSOut o;
    o.pos = mul(mul(float4(v.pos, 1.0f), gWorld), gViewProj);
    return o;
}
// PSMain を欠落させる (規約違反 = コンパイル失敗)
)HLSL";

void TestShaderManagerReloadAndCache(GraphicsDevice& device, const std::wstring& engineShaderDir)
{
    MYE_LOG_INFO("-- ShaderManager: hot reload / bytecode cache --");
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / L"mye_surface_material_selftest_sm";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path shaderPath = dir / L"Reload.surface.hlsl";
    WriteFile(shaderPath, MakeFixture(0.25f));

    std::vector<std::wstring> dirs = { dir.wstring(), engineShaderDir };

    // ---- 2a. 同じ名前の LoadSurface を繰り返しても再コンパイルしない ----
    ShaderManager shaders;
    Check(shaders.Init(device, dirs), "shader manager init (reload test)");
    const AssetID id = shaders.LoadSurface("Reload.surface");
    SurfaceProgram* prog = shaders.GetSurface(id);
    Check(prog != nullptr && prog->valid, "Reload.surface compiles (initial)");
    const int missesAfterFirst = shaders.CacheMisses() + shaders.CacheHits();
    shaders.LoadSurface("Reload.surface"); // 2 回目
    Check(shaders.CacheMisses() + shaders.CacheHits() == missesAfterFirst,
          "同名の LoadSurface を繰り返しても再コンパイルしない (cache カウンタ不変)");
    const uint64_t gen1 = prog ? prog->generation : 0;

    // ---- 2b. ホットリロード成功: 内容を変えて再コンパイルすると世代が進む ----
    WriteFile(shaderPath, MakeFixture(0.75f));
    const std::wstring normPath = shaders.ResolveShaderPath("Reload.surface");
    shaders.RequestRecompileForFile(normPath);
    bool swapped = false;
    for (int i = 0; i < 200 && !swapped; ++i) {
        shaders.PollAsyncCompiles();
        prog = shaders.GetSurface(id);
        swapped = (prog && prog->generation != gen1);
        if (!swapped) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    Check(swapped, "ホットリロード成功で世代が進む (旧プログラムの単純維持ではない)");
    Check(prog != nullptr && prog->valid, "ホットリロード後も有効なプログラムのまま");
    const uint64_t gen2 = prog ? prog->generation : 0;

    // ---- 2c. ホットリロード失敗: 旧プログラム維持 (世代・valid が変わらない) ----
    WriteFile(shaderPath, kBrokenFixture);
    shaders.RequestRecompileForFile(normPath);
    for (int i = 0; i < 200; ++i) { // 失敗側は完了確認の signal が無いので固定回数ポーリングして安定を待つ
        shaders.PollAsyncCompiles();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    prog = shaders.GetSurface(id);
    Check(prog != nullptr && prog->valid && prog->generation == gen2,
          "ホットリロード失敗時は旧プログラム (世代・内容) を維持する");

    // ---- 2d. バイトコードキャッシュ: 2 本目の ShaderManager が同じ中身をヒットで読む ----
    WriteFile(shaderPath, MakeFixture(0.5f)); // 壊れたフィクスチャを健全な内容へ戻す
    const fs::path cacheDir = dir / L"cache";
    fs::create_directories(cacheDir, ec);

    ShaderManager cacheA;
    cacheA.SetCacheDir(cacheDir.wstring(), true);
    Check(cacheA.Init(device, dirs), "shader manager init (cache A)");
    const AssetID cacheIdA = cacheA.LoadSurface("Reload.surface");
    SurfaceProgram* progA = cacheA.GetSurface(cacheIdA);
    Check(progA != nullptr && progA->valid && cacheA.CacheMisses() == 1 && cacheA.CacheHits() == 0,
          "1 本目は cache miss でフルコンパイル");

    ShaderManager cacheB;
    cacheB.SetCacheDir(cacheDir.wstring(), true);
    Check(cacheB.Init(device, dirs), "shader manager init (cache B)");
    const AssetID cacheIdB = cacheB.LoadSurface("Reload.surface");
    SurfaceProgram* progB = cacheB.GetSurface(cacheIdB);
    Check(progB != nullptr && progB->valid && cacheB.CacheHits() == 1 && cacheB.CacheMisses() == 0,
          "2 本目は同じキャッシュディレクトリでヒットする (D3DCompile を飛ばす)");
    if (progA && progB) {
        Check(progA->colorPSReflect.vars.contains("_Tag") && progB->colorPSReflect.vars.contains("_Tag"),
              "キャッシュ経由でもリフレクション (MyEnginePerMaterial._Tag) が復元される");
    }

    // 中身が変わればキャッシュを再利用しない (ソースハッシュ照合)
    WriteFile(shaderPath, MakeFixture(0.9f));
    ShaderManager cacheC;
    cacheC.SetCacheDir(cacheDir.wstring(), true);
    Check(cacheC.Init(device, dirs), "shader manager init (cache C)");
    cacheC.LoadSurface("Reload.surface");
    Check(cacheC.CacheMisses() == 1 && cacheC.CacheHits() == 0,
          "ソースが変わった後は古いキャッシュへヒットしない (miss で再コンパイル)");

    fs::remove_all(dir, ec);
}

// ---- 3. MaterialLibrary: .mat.json の遅延 Load・Properties パック・失敗時フォールバック ----
void TestMaterialLibrarySurfaceState(GraphicsDevice& device, const std::wstring& engineShaderDir)
{
    MYE_LOG_INFO("-- MaterialLibrary: GetOrBuildSurfaceState --");
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / L"mye_surface_material_selftest_mat";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // MyEnginePerMaterial の宣言順を Properties ブロック (_Amp, _Tint, _Tex) の逆にして、
    // 「パック済みバイト列がリフレクションのオフセットで正しい」ことを実コンパイル経路でも確かめる
    const char* kToonSurface = R"HLSL(
/*@MyEngineProperties
_Amp ("Amp", Range(0,1)) = 0.2
_Tint ("Tint", Color) = (1,1,1,1)
_Tex ("Tex", 2D) = "white" {}
@*/
#include "MyEngineSurface.hlsli"
cbuffer MyEnginePerMaterial
{
    float4 _Tint;
    float _Amp;
};
Texture2D _Tex;
struct VSIn { float3 pos : POSITION; };
struct VSOut { float4 pos : SV_Position; };
VSOut VSMain(VSIn v)
{
    VSOut o;
    float3 p = v.pos;
    p.y += _Amp * sin(gTime);
    o.pos = mul(mul(float4(p, 1.0f), gWorld), gViewProj);
    return o;
}
float4 PSMain(VSOut i) : SV_Target
{
    float4 tex = _Tex.Sample(gSampler, float2(0.5f, 0.5f));
    return _Tint * tex;
}
)HLSL";
    WriteFile(dir / L"Toon.surface.hlsl", kToonSurface);

    nlohmann::json matJson;
    matJson["engine"] = "MyEngine";
    matJson["material"] = 1;
    matJson["shader"] = "Toon.surface";
    matJson["baseColor"] = nlohmann::json::array({ 1.0, 1.0, 1.0, 1.0 });
    // M79 sub-06: boundsPadding / doubleSided もこの本体マテリアルで検証する
    matJson["boundsPadding"] = 1.5;
    matJson["doubleSided"] = true;
    {
        nlohmann::json props = nlohmann::json::object();
        props["_Amp"] = 0.7;
        props["_Tint"] = nlohmann::json::array({ 0.25, 0.5, 0.75, 1.0 });
        matJson["properties"] = props;
    }
    const fs::path matPath = dir / L"Toon.mat.json";
    WriteFile(matPath, matJson.dump(2));

    nlohmann::json missingMatJson;
    missingMatJson["shader"] = "Missing.surface";
    const fs::path missingMatPath = dir / L"Missing.mat.json";
    WriteFile(missingMatPath, missingMatJson.dump(2));

    std::vector<std::wstring> dirs = { dir.wstring(), engineShaderDir };
    ShaderManager shaders;
    Check(shaders.Init(device, dirs), "shader manager init (material test)");

    RenderResources resources;
    resources.textures.Init(device);

    const AssetID matId = resources.materials.LoadFromFile(matPath.wstring(), resources.textures, dir.wstring());
    Check(!matId.IsNull(), ".mat.json (surface) loads");

    // M79 sub-06: boundsPadding / doubleSided が横テーブルへ読み込まれる
    Check(std::fabs(resources.materials.GetSurfaceBoundsPadding(matId) - 1.5f) < 1e-5f,
          "boundsPadding が .mat.json の値 (1.5) で読み込まれる");
    Check(resources.materials.GetSurfaceDoubleSided(matId), "doubleSided が .mat.json の値 (true) で読み込まれる");

    SurfaceMaterialState* st =
        resources.materials.GetOrBuildSurfaceState(matId, shaders, resources.textures, device);
    Check(st != nullptr && st->isSurfaceShader, "GetOrBuildSurfaceState はサーフェスマテリアルを認識する");
    if (st) {
        Check(st->ready && !st->useErrorFallback, "Toon.surface は正常にコンパイルされ ready になる");
        SurfaceProgram* prog = shaders.GetSurface(st->surfaceProgramId);
        Check(prog != nullptr && prog->valid, "surfaceProgramId が有効なプログラムを指す");

        bool haveTint = false;
        bool haveAmp = false;
        uint32_t tintOffset = 0;
        uint32_t ampOffset = 0;
        if (prog) {
            if (const auto it = prog->colorPSReflect.vars.find("_Tint");
                it != prog->colorPSReflect.vars.end()) {
                haveTint = true;
                tintOffset = it->second.offset;
            }
            if (const auto it = prog->colorPSReflect.vars.find("_Amp");
                it != prog->colorPSReflect.vars.end()) {
                haveAmp = true;
                ampOffset = it->second.offset;
            }
        }
        Check(haveTint && haveAmp, "_Tint/_Amp のリフレクションが取れる");
        if (haveTint) {
            Check(std::fabs(ReadFloatAt(st->perMaterialCB, tintOffset) - 0.25f) < 1e-5f,
                  "_Tint.r が .mat.json の properties 値でパックされる (リフレクションのオフセット)");
        }
        if (haveAmp) {
            Check(std::fabs(ReadFloatAt(st->perMaterialCB, ampOffset) - 0.7f) < 1e-5f,
                  "_Amp が .mat.json の properties 値でパックされる (リフレクションのオフセット)");
        }
        // review-1 #4: MyEnginePerMaterial (_Tint float4 + _Amp float → 32 バイト) だけの
        // サイズか。旧実装は全 cbuffer から cbSize = max(var.cbufSize) を取っており、
        // 同じ翻訳単位の MyEnginePerFrame (最大の cbuffer) のサイズまで膨らんでいた
        Check(st->perMaterialCB.size() == 32,
              "review-1 #4: MyEnginePerMaterial の CB サイズは自身の宣言 (32 バイト) どおり");
        Check(st->perMaterialCB.size() != sizeof(MyEnginePerFrameCB),
              "review-1 #4: MyEnginePerFrame (予約 CB) のサイズまで膨らんでいない");
        Check(st->textures.contains("_Tex"), "_Tex (Tex2D プロパティ) が解決される");
        if (st->textures.contains("_Tex")) {
            Check(st->textures.at("_Tex").value == resources.textures.White().value,
                  "JSON に無い Tex2D プロパティは既定 (white) にフォールバックする");
        }

        // .mat.json を書き換えて再ロードすると再パックされる (revision の変化を検出)
        {
            nlohmann::json props2 = nlohmann::json::object();
            props2["_Amp"] = 0.9;
            props2["_Tint"] = nlohmann::json::array({ 0.25, 0.5, 0.75, 1.0 });
            matJson["properties"] = props2;
        }
        WriteFile(matPath, matJson.dump(2));
        resources.materials.LoadFromFile(matPath.wstring(), resources.textures, dir.wstring());
        SurfaceMaterialState* st2 =
            resources.materials.GetOrBuildSurfaceState(matId, shaders, resources.textures, device);
        Check(st2 != nullptr && haveAmp
                  && std::fabs(ReadFloatAt(st2->perMaterialCB, ampOffset) - 0.9f) < 1e-5f,
              ".mat.json の再読込で Properties の変更が再パックに反映される");
    }

    const AssetID missingMatId =
        resources.materials.LoadFromFile(missingMatPath.wstring(), resources.textures, dir.wstring());
    SurfaceMaterialState* missingSt =
        resources.materials.GetOrBuildSurfaceState(missingMatId, shaders, resources.textures, device);
    Check(missingSt != nullptr && missingSt->isSurfaceShader, "存在しない shader 名もサーフェス扱いになる");
    if (missingSt) {
        Check(!missingSt->ready && missingSt->useErrorFallback,
              "存在しない shader 名は ready=false + useErrorFallback=true (マゼンタ経路)");
        Check(!missingSt->errorMessage.empty(), "失敗理由が errorMessage に残る");
    }

    // 対象外 (forward_lit) のマテリアルは nullptr
    Material plain;
    plain.shader = AssetID{ HashStr("forward_lit") };
    const AssetID plainId = resources.materials.Register("plain", plain);
    SurfaceMaterialState* plainSt =
        resources.materials.GetOrBuildSurfaceState(plainId, shaders, resources.textures, device);
    Check(plainSt == nullptr, "forward_lit マテリアルは GetOrBuildSurfaceState が nullptr を返す (対象外)");

    // M79 sub-06: 負値の boundsPadding は 0 に丸められる (WARN、spec §4.2)
    {
        nlohmann::json negJson;
        negJson["shader"] = "Toon.surface";
        negJson["boundsPadding"] = -3.0;
        const fs::path negPath = dir / L"NegPadding.mat.json";
        WriteFile(negPath, negJson.dump(2));
        const AssetID negId =
            resources.materials.LoadFromFile(negPath.wstring(), resources.textures, dir.wstring());
        Check(std::fabs(resources.materials.GetSurfaceBoundsPadding(negId) - 0.0f) < 1e-5f,
              "負の boundsPadding (-3.0) は 0 に丸められる");
    }

    // M79 sub-06: forward_lit (サーフェスでない) に boundsPadding/doubleSided を書いても効かない
    // (横テーブルへ登録されない。spec §4.2「サーフェスでないマテリアルでは読み込むが効かない」)
    {
        nlohmann::json nonSurfaceJson;
        nonSurfaceJson["shader"] = "forward_lit";
        nonSurfaceJson["boundsPadding"] = 5.0;
        nonSurfaceJson["doubleSided"] = true;
        const fs::path nonSurfacePath = dir / L"NonSurfacePadding.mat.json";
        WriteFile(nonSurfacePath, nonSurfaceJson.dump(2));
        const AssetID nonSurfaceId = resources.materials.LoadFromFile(
            nonSurfacePath.wstring(), resources.textures, dir.wstring());
        Check(std::fabs(resources.materials.GetSurfaceBoundsPadding(nonSurfaceId) - 0.0f) < 1e-5f,
              "forward_lit の boundsPadding は横テーブルに乗らず 0 を返す (非サーフェスに効かない)");
        Check(!resources.materials.GetSurfaceDoubleSided(nonSurfaceId),
              "forward_lit の doubleSided も同様に効かない");
    }

    fs::remove_all(dir, ec);
}

// ---- 4. ForwardPath: 実描画で「サーフェス色エントリ」「forward_lit との混在」
//         「失敗時マゼンタ」を確かめる (縦切りの本体、sub-02 の accept 1/3/5) ----
void TestForwardPathDrawsSurfaceItems(GraphicsDevice& device, const std::wstring& engineShaderDir)
{
    MYE_LOG_INFO("-- ForwardPath: 実描画 (surface color entry / 混在 / 失敗時マゼンタ) --");
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / L"mye_surface_material_selftest_draw";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // 固定色 (0.2,0.8,0.3) を返すだけの最小サーフェスシェーダ。Properties も
    // MyEnginePerMaterial も宣言しない = 「宣言が無いときも安全にバインドを飛ばす」を兼ねて検査する
    const char* kProbeSurface = R"HLSL(
#include "MyEngineSurface.hlsli"
struct VSIn { float3 pos : POSITION; };
struct VSOut { float4 pos : SV_Position; };
VSOut VSMain(VSIn v)
{
    VSOut o;
    o.pos = mul(mul(float4(v.pos, 1.0f), gWorld), gViewProj);
    return o;
}
float4 PSMain(VSOut i) : SV_Target
{
    return float4(0.2f, 0.8f, 0.3f, 1.0f);
}
)HLSL";
    WriteFile(dir / L"RenderProbe.surface.hlsl", kProbeSurface);

    auto writeMat = [&](const wchar_t* fileName, const std::string& shader,
                        const std::array<double, 4>& baseColor) {
        nlohmann::json j;
        j["shader"] = shader;
        j["baseColor"] = nlohmann::json::array(
            { baseColor[0], baseColor[1], baseColor[2], baseColor[3] });
        WriteFile(dir / fileName, j.dump(2));
    };
    writeMat(L"Surface.mat.json", "RenderProbe.surface", { 1.0, 1.0, 1.0, 1.0 });
    writeMat(L"Lit.mat.json", "forward_lit", { 0.6, 0.1, 0.9, 1.0 });
    writeMat(L"Missing.mat.json", "NoSuchThing.surface", { 1.0, 1.0, 1.0, 1.0 });

    // Properties (Color) が実描画に効くかの確認用 (accept 2)。同じシェーダを 2 本の
    // マテリアルで共有し、それぞれ違う _Color を持たせて「マテリアルごとに独立した
    // MyEnginePerMaterial を持つ」ことも一緒に確かめる
    const char* kPropsProbeSurface = R"HLSL(
/*@MyEngineProperties
_Color ("Color", Color) = (1, 1, 1, 1)
@*/
#include "MyEngineSurface.hlsli"
cbuffer MyEnginePerMaterial { float4 _Color; };
struct VSIn { float3 pos : POSITION; };
struct VSOut { float4 pos : SV_Position; };
VSOut VSMain(VSIn v)
{
    VSOut o;
    o.pos = mul(mul(float4(v.pos, 1.0f), gWorld), gViewProj);
    return o;
}
float4 PSMain(VSOut i) : SV_Target
{
    return _Color;
}
)HLSL";
    WriteFile(dir / L"PropsProbe.surface.hlsl", kPropsProbeSurface);
    {
        nlohmann::json j;
        j["shader"] = "PropsProbe.surface";
        nlohmann::json props = nlohmann::json::object();
        props["_Color"] = nlohmann::json::array({ 0.9, 0.1, 0.1, 1.0 });
        j["properties"] = props;
        WriteFile(dir / L"PropsA.mat.json", j.dump(2));
    }
    {
        nlohmann::json j;
        j["shader"] = "PropsProbe.surface";
        nlohmann::json props = nlohmann::json::object();
        props["_Color"] = nlohmann::json::array({ 0.1, 0.9, 0.1, 1.0 });
        j["properties"] = props;
        WriteFile(dir / L"PropsB.mat.json", j.dump(2));
    }

    // PS 側の static 代入の回帰確認 (sub-02 round 1 の指摘)。PSMain が gTime を R へ、
    // posW を gViewProj で再投影した NDC.x を G へ出す。VS と PS は別プログラムなので、
    // MyePSColor が PSMain を呼ぶ前に static を代入し忘れていると R=0 / G=0.5 のまま
    // (gViewProj が未代入=ゼロ行列だと再投影結果もゼロになるため) になる
    const char* kPsStaticsProbeSurface = R"HLSL(
#include "MyEngineSurface.hlsli"
struct VSIn { float3 pos : POSITION; };
struct VSOut { float4 pos : SV_Position; float3 posW : TEXCOORD0; };
VSOut VSMain(VSIn v)
{
    VSOut o;
    float4 worldPos = mul(float4(v.pos, 1.0f), gWorld);
    o.pos = mul(worldPos, gViewProj);
    o.posW = worldPos.xyz;
    return o;
}
float4 PSMain(VSOut i) : SV_Target
{
    float4 reprojClip = mul(float4(i.posW, 1.0f), gViewProj);
    float2 reprojNdc = reprojClip.xy / max(reprojClip.w, 1e-5f);
    return float4(gTime, reprojNdc.x * 0.5f + 0.5f, reprojNdc.y * 0.5f + 0.5f, 1.0f);
}
)HLSL";
    WriteFile(dir / L"PsStaticsProbe.surface.hlsl", kPsStaticsProbeSurface);
    writeMat(L"PsStatics.mat.json", "PsStaticsProbe.surface", { 1.0, 1.0, 1.0, 1.0 });

    // review-1 #2 の再現用: VSMain が Texture2D を読む (t0 を明示指定 = forward_lit_instanced.hlsl の
    // StructuredBuffer<MeshInstance> と同じスロット)。restoreForwardLitBindings が VS t0 を
    // 戻さないと、この直後に描く instanced forward_lit run が「BUFFER のはずが TEXTURE2D」で消える
    const char* kVTexSurface = R"HLSL(
/*@MyEngineProperties
_HeightTex ("Height", 2D) = "white" {}
@*/
#include "MyEngineSurface.hlsli"
Texture2D _HeightTex : register(t0);
struct VSIn { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_Position; };
VSOut VSMain(VSIn v)
{
    VSOut o;
    // 変位量は無視できるほど小さくし (見た目はほぼ不変位)、_HeightTex の参照だけを
    // コンパイラの dead code elimination で消させない
    float h = _HeightTex.SampleLevel(gSampler, v.uv, 0).r;
    float3 p = v.pos;
    p.y += h * 0.0001f;
    o.pos = mul(mul(float4(p, 1.0f), gWorld), gViewProj);
    return o;
}
float4 PSMain(VSOut i) : SV_Target
{
    return float4(0.9f, 0.6f, 0.1f, 1.0f);
}
)HLSL";
    WriteFile(dir / L"VTex.surface.hlsl", kVTexSurface);
    writeMat(L"VTex.mat.json", "VTex.surface", { 1.0, 1.0, 1.0, 1.0 });

    std::vector<std::wstring> dirs = { dir.wstring(), engineShaderDir };
    ShaderManager shaders;
    Check(shaders.Init(device, dirs), "shader manager init (draw test)");

    RenderResources resources;
    resources.Init(device); // meshes (Quad 含む) + textures

    ForwardPath fp;
    Check(fp.Init(device, shaders), "ForwardPath::Init (forward_lit/surface_error 込み)");

    const AssetID surfaceMatId = resources.materials.LoadFromFile(
        (dir / L"Surface.mat.json").wstring(), resources.textures, dir.wstring());
    const AssetID litMatId = resources.materials.LoadFromFile(
        (dir / L"Lit.mat.json").wstring(), resources.textures, dir.wstring());
    const AssetID missingMatId = resources.materials.LoadFromFile(
        (dir / L"Missing.mat.json").wstring(), resources.textures, dir.wstring());
    const AssetID propsAMatId = resources.materials.LoadFromFile(
        (dir / L"PropsA.mat.json").wstring(), resources.textures, dir.wstring());
    const AssetID propsBMatId = resources.materials.LoadFromFile(
        (dir / L"PropsB.mat.json").wstring(), resources.textures, dir.wstring());
    const AssetID psStaticsMatId = resources.materials.LoadFromFile(
        (dir / L"PsStatics.mat.json").wstring(), resources.textures, dir.wstring());
    const AssetID vtexMatId = resources.materials.LoadFromFile(
        (dir / L"VTex.mat.json").wstring(), resources.textures, dir.wstring());
    Check(!surfaceMatId.IsNull() && !litMatId.IsNull() && !missingMatId.IsNull()
              && !propsAMatId.IsNull() && !propsBMatId.IsNull() && !psStaticsMatId.IsNull()
              && !vtexMatId.IsNull(),
          "7 本の .mat.json が読み込める");

    const AssetID quad = resources.meshes.Quad();

    // ---- レンダーターゲット (96x32)。x=16/48/80 がそれぞれ左/中/右のクワッドに当たる ----
    constexpr UINT kWidth = 96;
    constexpr UINT kHeight = 32;
    ID3D11Device* dev = device.Device();
    ID3D11DeviceContext* dc = device.Context();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = kWidth;
    td.Height = kHeight;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc = { 1, 0 };
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> colorTex;
    ComPtr<ID3D11RenderTargetView> colorRtv;
    bool ok = SUCCEEDED(dev->CreateTexture2D(&td, nullptr, colorTex.GetAddressOf()));
    ok = ok && SUCCEEDED(dev->CreateRenderTargetView(colorTex.Get(), nullptr, colorRtv.GetAddressOf()));
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    ok = ok && SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, staging.GetAddressOf()));
    Check(ok, "描画確認用の RTV/staging を作成できる");
    if (!ok) {
        fs::remove_all(dir, ec);
        return;
    }

    // カメラは既定の「原点から +Z を向く」規約 (TestFrustumCulling と同じ)。
    // 直交射影にして NDC⇔ワールド x の対応を線形にし、3 枚のクワッドを重ならずに並べる
    RenderView view;
    XMStoreFloat4x4(&view.view,
                    XMMatrixLookAtLH(XMVectorSet(0, 0, 0, 1), XMVectorSet(0, 0, 1, 1),
                                     XMVectorSet(0, 1, 0, 0)));
    XMStoreFloat4x4(&view.proj, XMMatrixOrthographicLH(6.0f, 3.0f, 0.1f, 100.0f));
    XMStoreFloat4x4(&view.projNoJitter, XMLoadFloat4x4(&view.proj));
    view.width = static_cast<int>(kWidth);
    view.height = static_cast<int>(kHeight);
    view.rtv = colorRtv.Get();
    view.dsv = nullptr;
    view.clearColor[0] = view.clearColor[1] = view.clearColor[2] = 0.0f;
    view.clearColor[3] = 1.0f;
    view.instancingEnabled = 0; // このテストの本題ではないので明示的に切る

    SceneLightData lights;
    lights.ambient = { 1.0f, 1.0f, 1.0f };
    lights.count = 0;

    auto makeItem = [&](AssetID mat, float x) {
        RenderItem item;
        item.mesh = quad;
        item.material = mat;
        XMStoreFloat4x4(&item.world, XMMatrixTranslation(x, 0.0f, 5.0f));
        XMStoreFloat4x4(&item.prevWorld, XMMatrixTranslation(x, 0.0f, 5.0f));
        return item;
    };

    auto readPixel = [&](int px, int py) {
        dc->CopyResource(staging.Get(), colorTex.Get());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        std::array<uint8_t, 4> out = { 0, 0, 0, 0 };
        if (SUCCEEDED(dc->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            const uint8_t* row =
                static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(py) * mapped.RowPitch;
            std::memcpy(out.data(), row + static_cast<size_t>(px) * 4, 4);
            dc->Unmap(staging.Get(), 0);
        }
        return out;
    };
    auto closeTo = [](uint8_t a, uint8_t b) { return std::abs(static_cast<int>(a) - static_cast<int>(b)) <= 3; };

    RenderQueue queueLitOnly;
    queueLitOnly.opaque = { makeItem(litMatId, 0.0f) };
    fp.Render(device, view, queueLitOnly, lights, resources, shaders);
    const std::array<uint8_t, 4> litAlone = readPixel(48, 16);
    Check(!(litAlone[0] == 0 && litAlone[1] == 0 && litAlone[2] == 0),
          "forward_lit 単体は背景 (黒) のままではない (ambient で何かしら灯る)");

    RenderQueue queueMixed;
    queueMixed.opaque = { makeItem(surfaceMatId, -2.0f), makeItem(litMatId, 0.0f) };
    fp.Render(device, view, queueMixed, lights, resources, shaders);
    const std::array<uint8_t, 4> surfacePixel = readPixel(16, 16);
    const std::array<uint8_t, 4> litMixed = readPixel(48, 16);
    Check(closeTo(surfacePixel[0], 51) && closeTo(surfacePixel[1], 204) && closeTo(surfacePixel[2], 76),
          "サーフェス色エントリの固定色 (0.2,0.8,0.3) がそのまま描画される");
    if (!(closeTo(surfacePixel[0], 51) && closeTo(surfacePixel[1], 204) && closeTo(surfacePixel[2], 76))) {
        MYE_LOG_ERROR("    surfacePixel=(%d,%d,%d,%d)", surfacePixel[0], surfacePixel[1],
                     surfacePixel[2], surfacePixel[3]);
    }
    Check(closeTo(litMixed[0], litAlone[0]) && closeTo(litMixed[1], litAlone[1])
              && closeTo(litMixed[2], litAlone[2]),
          "サーフェスアイテムの直後でも forward_lit の見た目が変わらない (バインド前提を壊さない)");

    RenderQueue queueFail;
    queueFail.opaque = { makeItem(missingMatId, 2.0f) };
    fp.Render(device, view, queueFail, lights, resources, shaders);
    const std::array<uint8_t, 4> failPixel = readPixel(80, 16);
    Check(closeTo(failPixel[0], 255) && closeTo(failPixel[1], 0) && closeTo(failPixel[2], 255),
          "存在しない shader 名のマテリアルはマゼンタで描画される (クラッシュしない)");
    if (!(closeTo(failPixel[0], 255) && closeTo(failPixel[1], 0) && closeTo(failPixel[2], 255))) {
        MYE_LOG_ERROR("    failPixel=(%d,%d,%d,%d)", failPixel[0], failPixel[1], failPixel[2],
                     failPixel[3]);
    }

    // ---- accept 2: .mat.json の Properties (Color) の値違いが実描画に効く ----
    RenderQueue queuePropsA;
    queuePropsA.opaque = { makeItem(propsAMatId, 0.0f) };
    fp.Render(device, view, queuePropsA, lights, resources, shaders);
    const std::array<uint8_t, 4> propsAPixel = readPixel(48, 16);
    Check(closeTo(propsAPixel[0], 230) && closeTo(propsAPixel[1], 26) && closeTo(propsAPixel[2], 26),
          "properties._Color = (0.9,0.1,0.1) が実描画に反映される");
    if (!(closeTo(propsAPixel[0], 230) && closeTo(propsAPixel[1], 26) && closeTo(propsAPixel[2], 26))) {
        MYE_LOG_ERROR("    propsAPixel=(%d,%d,%d,%d)", propsAPixel[0], propsAPixel[1], propsAPixel[2],
                     propsAPixel[3]);
    }

    RenderQueue queuePropsB;
    queuePropsB.opaque = { makeItem(propsBMatId, 0.0f) };
    fp.Render(device, view, queuePropsB, lights, resources, shaders);
    const std::array<uint8_t, 4> propsBPixel = readPixel(48, 16);
    Check(closeTo(propsBPixel[0], 26) && closeTo(propsBPixel[1], 230) && closeTo(propsBPixel[2], 26),
          "同じシェーダを共有する別マテリアルは自分の properties._Color = (0.1,0.9,0.1) で描かれる"
          " (MyEnginePerMaterial がマテリアルごとに独立)");
    if (!(closeTo(propsBPixel[0], 26) && closeTo(propsBPixel[1], 230) && closeTo(propsBPixel[2], 26))) {
        MYE_LOG_ERROR("    propsBPixel=(%d,%d,%d,%d)", propsBPixel[0], propsBPixel[1], propsBPixel[2],
                     propsBPixel[3]);
    }

    // ---- 回帰確認 (sub-02 round 1 の must 指摘): 色エントリの PSMain から見た
    //      gTime/gViewProj が「今フレーム」の値になっているか (VS と PS は別プログラムなので、
    //      PS 側で static へ代入し直していないと常に 0/ゼロ行列になる) ----
    view.viewFrameIndex = 30; // gTime = 30/60 = 0.5 (spec §2 の時計)
    RenderQueue queuePsStatics;
    queuePsStatics.opaque = { makeItem(psStaticsMatId, -2.0f) };
    fp.Render(device, view, queuePsStatics, lights, resources, shaders);
    const std::array<uint8_t, 4> psStaticsPixel = readPixel(16, 16);
    // 期待値: R = gTime(0.5)*255 ≈ 127、G = 再投影 NDC.x (world x=-2, ortho半幅3 → ndc=-0.667)
    // を 0..1 へ写した (0.1667)*255 ≈ 42。PS 側が未代入 (0) のままだと R=0 / G≈127 になる
    Check(closeTo(psStaticsPixel[0], 127) && closeTo(psStaticsPixel[1], 42),
          "色エントリの PSMain は gTime/gViewProj に今フレームの値を見る (PS 側 static 代入の回帰)");
    if (!(closeTo(psStaticsPixel[0], 127) && closeTo(psStaticsPixel[1], 42))) {
        MYE_LOG_ERROR("    psStaticsPixel=(%d,%d,%d,%d) / expected R=127,G=42",
                     psStaticsPixel[0], psStaticsPixel[1], psStaticsPixel[2], psStaticsPixel[3]);
    }

    // ---- review-1 #2 (受け入れ条件 13): VS テクスチャを読むサーフェス → instanced forward_lit。
    //      「サーフェス → forward_lit」の順で instanced run を消さないこと (逆順も確認) ----
    {
        RenderView instView = view;
        instView.instancingEnabled = 1;
        instView.viewFrameIndex = 0;

        // 順序A: サーフェス (VTex, x=-2) → forward_lit 2 個 (x=0, x=2。同一マテリアル+メッシュが
        // 連続するので BuildInstanceRuns が 1 run (count=2) にまとめる)
        RenderQueue queueSurfaceThenInstanced;
        queueSurfaceThenInstanced.opaque = { makeItem(vtexMatId, -2.0f), makeItem(litMatId, 0.0f),
                                             makeItem(litMatId, 2.0f) };
        fp.Render(device, instView, queueSurfaceThenInstanced, lights, resources, shaders);
        const std::array<uint8_t, 4> instAfterSurfaceMid = readPixel(48, 16);
        const std::array<uint8_t, 4> instAfterSurfaceRight = readPixel(80, 16);
        Check(!(instAfterSurfaceMid[0] == 0 && instAfterSurfaceMid[1] == 0 && instAfterSurfaceMid[2] == 0)
                  && !(instAfterSurfaceRight[0] == 0 && instAfterSurfaceRight[1] == 0
                       && instAfterSurfaceRight[2] == 0),
              "review-1 #2: VS テクスチャ付きサーフェスの直後でも instanced forward_lit run が消えない"
              " (VS t0 の instance SRV が restoreForwardLitBindings で戻る)");
        if ((instAfterSurfaceMid[0] == 0 && instAfterSurfaceMid[1] == 0 && instAfterSurfaceMid[2] == 0)
            || (instAfterSurfaceRight[0] == 0 && instAfterSurfaceRight[1] == 0
                && instAfterSurfaceRight[2] == 0)) {
            MYE_LOG_ERROR("    instAfterSurfaceMid=(%d,%d,%d) instAfterSurfaceRight=(%d,%d,%d)"
                         " (0,0,0 に近ければ instanced run が消えている)",
                         instAfterSurfaceMid[0], instAfterSurfaceMid[1], instAfterSurfaceMid[2],
                         instAfterSurfaceRight[0], instAfterSurfaceRight[1], instAfterSurfaceRight[2]);
        }

        // 順序を入れ替えても結果が変わらないこと (spec 受け入れ条件 13「描画順に依らない」)。
        // forward_lit 2 個を先に描き、VS テクスチャ付きサーフェスを最後に置く
        RenderQueue queueInstancedThenSurface;
        queueInstancedThenSurface.opaque = { makeItem(litMatId, -2.0f), makeItem(litMatId, 0.0f),
                                             makeItem(vtexMatId, 2.0f) };
        fp.Render(device, instView, queueInstancedThenSurface, lights, resources, shaders);
        const std::array<uint8_t, 4> instBeforeSurfaceLeft = readPixel(16, 16);
        const std::array<uint8_t, 4> instBeforeSurfaceMid = readPixel(48, 16);
        Check(!(instBeforeSurfaceLeft[0] == 0 && instBeforeSurfaceLeft[1] == 0
                && instBeforeSurfaceLeft[2] == 0)
                  && !(instBeforeSurfaceMid[0] == 0 && instBeforeSurfaceMid[1] == 0
                       && instBeforeSurfaceMid[2] == 0),
              "review-1 #2: 描画順を入れ替えて (instanced run → サーフェス) も instanced run は消えない");

        // 同じ instanced run 2 個の色は、直前にサーフェスを挟んだかどうかで変わらない
        // (spec 受け入れ条件 13「描画順に依らず絵が変わらない」の直接確認)
        Check(closeTo(instAfterSurfaceMid[0], instBeforeSurfaceLeft[0])
                  && closeTo(instAfterSurfaceMid[1], instBeforeSurfaceLeft[1])
                  && closeTo(instAfterSurfaceMid[2], instBeforeSurfaceLeft[2]),
              "review-1 #2: instanced run の絵はサーフェスとの描画順に依らず同じ");
    }

    fp.Shutdown();
    fs::remove_all(dir, ec);
}

// ---- 5. サンプル (assets/shaders/ToonFlat.surface.hlsl) がそのままコンパイルできるか ----
void TestSampleToonFlatCompiles(GraphicsDevice& device, const std::wstring& engineShaderDir)
{
    MYE_LOG_INFO("-- サンプル ToonFlat.surface.hlsl のコンパイル --");
    ShaderManager shaders;
    Check(shaders.Init(device, { engineShaderDir }), "shader manager init (sample test)");
    const AssetID id = shaders.LoadSurface("ToonFlat.surface");
    SurfaceProgram* prog = shaders.GetSurface(id);
    Check(prog != nullptr && prog->valid, "assets/shaders/ToonFlat.surface.hlsl がそのままコンパイルできる");
    if (prog && !prog->valid) {
        MYE_LOG_ERROR("    %s", prog->errorMessage.c_str());
    }
    if (prog && prog->valid) {
        Check(prog->colorPSReflect.vars.contains("_BaseColor")
                  && prog->colorPSReflect.vars.contains("_ShadowColor")
                  && prog->colorPSReflect.vars.contains("_Steps"),
              "MyEnginePerMaterial の 3 プロパティが解決される");
        Check(prog->colorPSReflect.resources.contains("_MainTex"), "作者 Texture2D '_MainTex' が解決される");
        Check(prog->propertiesSchema.ok && prog->propertiesSchema.properties.size() == 4,
              "Properties ブロック (_BaseColor/_ShadowColor/_Steps/_MainTex) がパースされる");
    }
}

// ---- 6. M79 sub-06: doubleSided (Cull None) と、描画後のラスタライザ復元。
//         Quad を Y 軸回りに 180° 回して裏面をカメラへ向け、既定 (Cull Back) では消え、
//         doubleSided:true では描かれることを確認する。さらに直後の forward_lit (同じく
//         裏面) が消えたままであることで、Cull None が forward_lit へ漏れていないことも見る ----
void TestForwardPathDoubleSidedCullingAndRestore(GraphicsDevice& device, const std::wstring& engineShaderDir)
{
    MYE_LOG_INFO("-- ForwardPath: doubleSided (Cull None) と描画後のラスタライザ復元 --");
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / L"mye_surface_doublesided_selftest";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    const char* kFixedColorSurface = R"HLSL(
#include "MyEngineSurface.hlsli"
struct VSIn { float3 pos : POSITION; };
struct VSOut { float4 pos : SV_Position; };
VSOut VSMain(VSIn v)
{
    VSOut o;
    o.pos = mul(mul(float4(v.pos, 1.0f), gWorld), gViewProj);
    return o;
}
float4 PSMain(VSOut i) : SV_Target
{
    return float4(0.9f, 0.5f, 0.1f, 1.0f);
}
)HLSL";
    WriteFile(dir / L"FixedColor.surface.hlsl", kFixedColorSurface);
    WriteFile(dir / L"SingleSided.mat.json", "{\"shader\":\"FixedColor.surface\"}");
    WriteFile(dir / L"DoubleSided.mat.json", "{\"shader\":\"FixedColor.surface\",\"doubleSided\":true}");

    ShaderManager shaders;
    Check(shaders.Init(device, { dir.wstring(), engineShaderDir }), "shader manager init (doubleSided)");

    RenderResources resources;
    resources.Init(device);
    const AssetID singleSidedMatId = resources.materials.LoadFromFile(
        (dir / L"SingleSided.mat.json").wstring(), resources.textures, dir.wstring());
    const AssetID doubleSidedMatId = resources.materials.LoadFromFile(
        (dir / L"DoubleSided.mat.json").wstring(), resources.textures, dir.wstring());
    Check(!singleSidedMatId.IsNull() && !doubleSidedMatId.IsNull(),
          "2 本の .mat.json が読み込める (doubleSided)");

    Material litRotated;
    litRotated.shader = AssetID{ HashStr("forward_lit") };
    litRotated.texture = resources.textures.White();
    litRotated.baseColor = { 0.2f, 0.6f, 0.9f, 1.0f };
    const AssetID litRotatedMatId = resources.materials.Register("doublesided_lit_rotated", litRotated);

    ForwardPath fp;
    Check(fp.Init(device, shaders), "ForwardPath::Init (doubleSided)");

    const AssetID quad = resources.meshes.Quad();

    constexpr UINT kWidth = 96;
    constexpr UINT kHeight = 32;
    ID3D11Device* dev = device.Device();
    ID3D11DeviceContext* dc = device.Context();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = kWidth;
    td.Height = kHeight;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc = { 1, 0 };
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> colorTex;
    ComPtr<ID3D11RenderTargetView> colorRtv;
    bool ok = SUCCEEDED(dev->CreateTexture2D(&td, nullptr, colorTex.GetAddressOf()));
    ok = ok && SUCCEEDED(dev->CreateRenderTargetView(colorTex.Get(), nullptr, colorRtv.GetAddressOf()));
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    ok = ok && SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, staging.GetAddressOf()));
    Check(ok, "描画確認用の RTV/staging を作成できる (doubleSided)");
    if (!ok) {
        fs::remove_all(dir, ec);
        return;
    }

    RenderView view;
    XMStoreFloat4x4(&view.view,
                    XMMatrixLookAtLH(XMVectorSet(0, 0, 0, 1), XMVectorSet(0, 0, 1, 1),
                                     XMVectorSet(0, 1, 0, 0)));
    XMStoreFloat4x4(&view.proj, XMMatrixOrthographicLH(6.0f, 3.0f, 0.1f, 100.0f));
    XMStoreFloat4x4(&view.projNoJitter, XMLoadFloat4x4(&view.proj));
    view.width = static_cast<int>(kWidth);
    view.height = static_cast<int>(kHeight);
    view.rtv = colorRtv.Get();
    view.dsv = nullptr;
    view.clearColor[0] = view.clearColor[1] = view.clearColor[2] = 0.0f;
    view.clearColor[3] = 1.0f;
    view.instancingEnabled = 0;

    SceneLightData lights;
    lights.ambient = { 1.0f, 1.0f, 1.0f };
    lights.count = 0;

    // 3 枚とも Y 軸回りに 180° 回して裏面をカメラへ向ける (Quad のローカル法線は -Z、
    // 180° 回転で +Z = カメラから見て裏面になる。既定の CULL_BACK なら消える)
    auto makeRotatedItem = [&](AssetID mat, float x) {
        RenderItem item;
        item.mesh = quad;
        item.material = mat;
        const XMMATRIX w = XMMatrixRotationY(XM_PI) * XMMatrixTranslation(x, 0.0f, 5.0f);
        XMStoreFloat4x4(&item.world, w);
        item.prevWorld = item.world;
        return item;
    };

    auto readPixel = [&](int px, int py) {
        dc->CopyResource(staging.Get(), colorTex.Get());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        std::array<uint8_t, 4> out = { 0, 0, 0, 0 };
        if (SUCCEEDED(dc->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            const uint8_t* row =
                static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(py) * mapped.RowPitch;
            std::memcpy(out.data(), row + static_cast<size_t>(px) * 4, 4);
            dc->Unmap(staging.Get(), 0);
        }
        return out;
    };
    auto isBackground = [](const std::array<uint8_t, 4>& p) {
        return p[0] <= 3 && p[1] <= 3 && p[2] <= 3;
    };

    // 描画順: 裏面の single-sided → 裏面の double-sided → 裏面の forward_lit。
    // 3 番目が消えたままなら、doubleSided の Cull None が forward_lit へ漏れていない証拠になる
    RenderQueue queue;
    queue.opaque = { makeRotatedItem(singleSidedMatId, -2.0f), makeRotatedItem(doubleSidedMatId, 0.0f),
                     makeRotatedItem(litRotatedMatId, 2.0f) };
    fp.Render(device, view, queue, lights, resources, shaders);

    const std::array<uint8_t, 4> singleSidedPixel = readPixel(16, 16);
    const std::array<uint8_t, 4> doubleSidedPixel = readPixel(48, 16);
    const std::array<uint8_t, 4> litRotatedPixel = readPixel(80, 16);

    Check(isBackground(singleSidedPixel), "doubleSided=false (既定) の裏面は従来どおり Cull Back で消える");
    if (!isBackground(singleSidedPixel)) {
        MYE_LOG_ERROR("    singleSidedPixel=(%d,%d,%d,%d)", singleSidedPixel[0], singleSidedPixel[1],
                     singleSidedPixel[2], singleSidedPixel[3]);
    }
    Check(!isBackground(doubleSidedPixel),
          "doubleSided=true の裏面は Cull None で描かれる (固定色 0.9,0.5,0.1 が見える)");
    if (isBackground(doubleSidedPixel)) {
        MYE_LOG_ERROR("    doubleSidedPixel=(%d,%d,%d,%d)", doubleSidedPixel[0], doubleSidedPixel[1],
                     doubleSidedPixel[2], doubleSidedPixel[3]);
    }
    Check(isBackground(litRotatedPixel),
          "doubleSided サーフェスの直後でも forward_lit の Cull Back は元に戻っている"
          " (裏面の forward_lit は消えたまま)");
    if (!isBackground(litRotatedPixel)) {
        MYE_LOG_ERROR("    litRotatedPixel=(%d,%d,%d,%d) (0 に近くなければラスタライザが"
                     " Cull None のまま漏れている)",
                     litRotatedPixel[0], litRotatedPixel[1], litRotatedPixel[2], litRotatedPixel[3]);
    }

    fp.Shutdown();
    fs::remove_all(dir, ec);
}

} // namespace

bool RunSurfaceMaterialSelfTest()
{
    MYE_LOG_INFO("==== M79 surface material self test (sub-02) ====");
    g_fails = 0;

    TestPackPropertiesReflected();

    GraphicsDevice device;
    if (!device.Init(true)) {
        MYE_LOG_ERROR("  FAIL: WARP device init");
        return false;
    }
    const std::wstring engineShaderDir = FindEngineShaderDir();
    Check(!engineShaderDir.empty(), "engine shader dir found (needed for MyEngineSurface.hlsli)");

    if (!engineShaderDir.empty()) {
        TestShaderManagerReloadAndCache(device, engineShaderDir);
        TestMaterialLibrarySurfaceState(device, engineShaderDir);
        TestForwardPathDrawsSurfaceItems(device, engineShaderDir);
        TestForwardPathDoubleSidedCullingAndRestore(device, engineShaderDir); // M79 sub-06
        TestSampleToonFlatCompiles(device, engineShaderDir);
    }

    device.Shutdown();

    if (g_fails != 0) {
        MYE_LOG_ERROR("==== M79 surface material self test: %d FAILURE(S) ====", g_fails);
        return false;
    }
    MYE_LOG_INFO("==== M79 surface material self test: ALL PASS ====");
    return true;
}

} // namespace mye
