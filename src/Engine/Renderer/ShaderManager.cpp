#include "Engine/Renderer/ShaderManager.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <d3dcompiler.h>

#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GraphicsDevice.h"

namespace mye {
namespace {

using Microsoft::WRL::ComPtr;

bool ReadFileBytes(const std::wstring& path, std::vector<char>& out)
{
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) {
        return false;
    }
    f.seekg(0, std::ios::end);
    out.resize(static_cast<size_t>(f.tellg()));
    f.seekg(0, std::ios::beg);
    f.read(out.data(), static_cast<std::streamsize>(out.size()));
    return true;
}

// #include を shaderDir 基準で解決し、開いたファイルを記録する (M3 の依存グラフ用)
class IncludeRecorder : public ID3DInclude {
public:
    IncludeRecorder(std::wstring baseDir, std::vector<std::wstring>& outIncludes)
        : baseDir_(std::move(baseDir)), includes_(outIncludes) {}

    HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR pFileName, LPCVOID,
                           LPCVOID* ppData, UINT* pBytes) override
    {
        const std::wstring path = NormalizePathKey(baseDir_ + L"\\" + Utf8ToWide(pFileName));
        std::vector<char> data;
        if (!ReadFileBytes(path, data)) {
            return E_FAIL;
        }
        includes_.push_back(path);
        char* buf = static_cast<char*>(malloc(data.size()));
        if (!buf) {
            return E_OUTOFMEMORY;
        }
        memcpy(buf, data.data(), data.size());
        *ppData = buf;
        *pBytes = static_cast<UINT>(data.size());
        return S_OK;
    }

    HRESULT __stdcall Close(LPCVOID pData) override
    {
        free(const_cast<void*>(pData));
        return S_OK;
    }

private:
    std::wstring baseDir_;
    std::vector<std::wstring>& includes_;
};

// 入力レイアウトを VS リフレクションから構築 (float 成分の semantic を想定)
bool BuildInputLayout(ID3D11Device* device, ID3DBlob* vsBytecode,
                      ComPtr<ID3D11InputLayout>& out)
{
    ComPtr<ID3D11ShaderReflection> reflection;
    if (FAILED(D3DReflect(vsBytecode->GetBufferPointer(), vsBytecode->GetBufferSize(),
                          IID_PPV_ARGS(reflection.GetAddressOf())))) {
        return false;
    }
    D3D11_SHADER_DESC sd = {};
    reflection->GetDesc(&sd);

    std::vector<D3D11_INPUT_ELEMENT_DESC> elems;
    for (UINT i = 0; i < sd.InputParameters; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC pd = {};
        reflection->GetInputParameterDesc(i, &pd);
        if (pd.SystemValueType != D3D_NAME_UNDEFINED) {
            continue; // SV_VertexID 等は頂点バッファ由来ではない
        }
        D3D11_INPUT_ELEMENT_DESC e = {};
        e.SemanticName = pd.SemanticName;
        e.SemanticIndex = pd.SemanticIndex;
        e.InputSlot = 0;
        e.AlignedByteOffset = D3D11_APPEND_ALIGNED_ELEMENT;
        e.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        const int components = (pd.Mask == 1) ? 1 : (pd.Mask <= 3) ? 2 : (pd.Mask <= 7) ? 3 : 4;
        static const DXGI_FORMAT floatFormats[4] = {
            DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT,
            DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT
        };
        e.Format = floatFormats[components - 1];
        elems.push_back(e);
    }
    if (elems.empty()) {
        out.Reset(); // 頂点入力なし (SV_VertexID のみ等) は正常
        return true;
    }
    return SUCCEEDED(device->CreateInputLayout(elems.data(), static_cast<UINT>(elems.size()),
                                               vsBytecode->GetBufferPointer(),
                                               vsBytecode->GetBufferSize(), out.GetAddressOf()));
}

} // namespace

bool ShaderManager::Init(GraphicsDevice& device, std::wstring shaderDir)
{
    device_ = &device;
    dir_ = std::move(shaderDir);
    std::error_code ec;
    if (!std::filesystem::is_directory(dir_, ec)) {
        MYE_LOG_WARN("shader dir not found: %s", WideToUtf8(dir_).c_str());
    }
    return true;
}

AssetID ShaderManager::Load(std::string_view name)
{
    const AssetID id{ HashStr(name) };
    if (programs_.contains(id.value)) {
        return id;
    }
    ShaderProgram prog;
    prog.path = NormalizePathKey(dir_ + L"\\" + Utf8ToWide(name) + L".hlsl");
    if (!CompileProgram(prog.path, prog)) {
        MYE_LOG_ERROR("shader compile failed: %.*s", static_cast<int>(name.size()), name.data());
    }
    programs_.emplace(id.value, std::move(prog));
    return id;
}

ShaderProgram* ShaderManager::Get(AssetID id)
{
    auto it = programs_.find(id.value);
    return (it != programs_.end()) ? &it->second : nullptr;
}

bool ShaderManager::Recompile(AssetID id)
{
    auto it = programs_.find(id.value);
    if (it == programs_.end()) {
        return false;
    }
    // 成功時のみ差し替え (spec 8.1: 失敗時は旧シェーダを保持)
    ShaderProgram fresh;
    fresh.path = it->second.path;
    if (!CompileProgram(fresh.path, fresh)) {
        return false;
    }
    it->second = std::move(fresh);
    return true;
}

void ShaderManager::RequestRecompileForFile(const std::wstring& normalizedPath)
{
    for (auto& [id, prog] : programs_) {
        bool affected = (prog.path == normalizedPath);
        if (!affected) {
            for (const std::wstring& inc : prog.includes) {
                if (inc == normalizedPath) {
                    affected = true; // include 依存グラフ経由 (spec 8.1)
                    break;
                }
            }
        }
        if (!affected) {
            continue;
        }
        bool alreadyPending = false;
        for (const AsyncCompile& ac : async_) {
            if (ac.id == id) {
                alreadyPending = true;
                break;
            }
        }
        if (alreadyPending) {
            continue;
        }
        MYE_LOG_INFO("[reload] shader recompiling: %s", WideToUtf8(prog.path).c_str());
        // D3D11 デバイスはフリースレッド (Create* は別スレッド可)。
        // コンパイル失敗時も ShaderProgram (valid=false) が返り、差し替えはされない
        const std::wstring path = prog.path;
        async_.push_back({ id, std::async(std::launch::async, [this, path] {
                               ShaderProgram fresh;
                               fresh.path = path;
                               CompileProgram(path, fresh);
                               return fresh;
                           }) });
    }
}

void ShaderManager::PollAsyncCompiles()
{
    for (size_t i = 0; i < async_.size();) {
        if (async_[i].future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++i;
            continue;
        }
        ShaderProgram fresh = async_[i].future.get();
        if (fresh.valid) {
            programs_[async_[i].id] = std::move(fresh); // セーフポイントでの差し替え (フェーズ 2)
            MYE_LOG_INFO("[reload] shader swapped");
        } else {
            MYE_LOG_WARN("[reload] shader compile failed - keeping previous shader");
        }
        async_.erase(async_.begin() + static_cast<ptrdiff_t>(i));
    }
}

bool ShaderManager::CompileProgram(const std::wstring& path, ShaderProgram& out)
{
    std::vector<char> source;
    if (!ReadFileBytes(path, source)) {
        MYE_LOG_ERROR("shader file not found: %s", WideToUtf8(path).c_str());
        return false;
    }

    // フラグは全構成で同一にする (Debug/Release で描画結果に差を作らない)
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const std::string pathUtf8 = WideToUtf8(path);

    out.includes.clear();
    IncludeRecorder includer(dir_, out.includes);

    auto compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& bytecode) {
        ComPtr<ID3DBlob> errors;
        const HRESULT hr = D3DCompile(source.data(), source.size(), pathUtf8.c_str(), nullptr,
                                      &includer, entry, target, flags, 0,
                                      bytecode.GetAddressOf(), errors.GetAddressOf());
        if (FAILED(hr)) {
            const char* msg = errors ? static_cast<const char*>(errors->GetBufferPointer())
                                     : "(no error output)";
            MYE_LOG_ERROR("HLSL %s (%s):\n%s", entry, pathUtf8.c_str(), msg);
            return false;
        }
        if (errors && errors->GetBufferSize() > 1) {
            MYE_LOG_WARN("HLSL %s (%s):\n%s", entry, pathUtf8.c_str(),
                         static_cast<const char*>(errors->GetBufferPointer()));
        }
        return true;
    };

    ComPtr<ID3DBlob> vsCode, psCode;
    if (!compile("VSMain", "vs_5_0", vsCode) || !compile("PSMain", "ps_5_0", psCode)) {
        return false;
    }

    ID3D11Device* dev = device_->Device();
    if (FAILED(dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(),
                                       nullptr, out.vs.GetAddressOf()))
        || FAILED(dev->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(),
                                         nullptr, out.ps.GetAddressOf()))) {
        MYE_LOG_ERROR("shader object creation failed: %s", pathUtf8.c_str());
        return false;
    }
    if (!BuildInputLayout(dev, vsCode.Get(), out.inputLayout)) {
        MYE_LOG_ERROR("input layout creation failed: %s", pathUtf8.c_str());
        return false;
    }
    out.valid = true;
    MYE_LOG_INFO("shader compiled: %s", pathUtf8.c_str());
    return true;
}

} // namespace mye
