#include "Engine/Renderer/ShaderManager.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>

#include <process.h>

#include <d3dcompiler.h>

#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ShaderCache.h"

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

// #include をシェーダルート群 (優先度順) で解決し、開いたファイルを記録する
// (M3 の依存グラフ用)。プロジェクトが common.hlsli だけ差し替える、といった
// 部分上書きもここで成立する。解決は ShaderManager::ResolveInclude の 1 本に寄せる
// (キャッシュの鮮度判定が同じ規則で解決し直すため。2 本あると判定だけずれる)
class IncludeRecorder : public ID3DInclude {
public:
    using ResolveFn = std::function<std::wstring(const char*, std::vector<char>*)>;
    IncludeRecorder(ResolveFn resolve, std::vector<std::wstring>& outIncludes,
                    std::vector<ShaderCacheEntry::Dependency>& outDeps)
        : resolve_(std::move(resolve)), includes_(outIncludes), deps_(outDeps) {}

    HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR pFileName, LPCVOID,
                           LPCVOID* ppData, UINT* pBytes) override
    {
        std::vector<char> data;
        const std::wstring path = resolve_(pFileName, &data);
        if (path.empty()) {
            return E_FAIL;
        }
        includes_.push_back(path);
        deps_.push_back({ pFileName, WideToUtf8(path), HashBytes(data.data(), data.size()) });
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
    ResolveFn resolve_;
    std::vector<std::wstring>& includes_;
    std::vector<ShaderCacheEntry::Dependency>& deps_;
};

// 入力レイアウトを VS リフレクションから構築 (float 成分の semantic を想定)
bool BuildInputLayout(ID3D11Device* device, const std::vector<uint8_t>& vsBytecode,
                      ComPtr<ID3D11InputLayout>& out)
{
    ComPtr<ID3D11ShaderReflection> reflection;
    if (FAILED(D3DReflect(vsBytecode.data(), vsBytecode.size(),
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
        // ボーンインデックス (M18) は uint4 だが VB 側は u8x4 で詰めている。リフレクションの
        // 成分型ではなく semantic 名で特例判定し R8G8B8A8_UINT を割り当てる (MeshVertex と一致)。
        if (std::strcmp(pd.SemanticName, "BLENDINDICES") == 0) {
            e.Format = DXGI_FORMAT_R8G8B8A8_UINT;
        } else {
            e.Format = floatFormats[components - 1]; // BLENDWEIGHT は float4 = 既存経路で処理
        }
        elems.push_back(e);
    }
    if (elems.empty()) {
        out.Reset(); // 頂点入力なし (SV_VertexID のみ等) は正常
        return true;
    }
    return SUCCEEDED(device->CreateInputLayout(elems.data(), static_cast<UINT>(elems.size()),
                                               vsBytecode.data(), vsBytecode.size(),
                                               out.GetAddressOf()));
}

// ---- M79: サーフェスシェーダー (SurfaceProgram) 用の補助 ----

// 頂点シェーダの入力レイアウトを MeshVertex の固定オフセットから作る。
// BuildInputLayout (APPEND_ALIGNED、上) とは別系統: 作者の VSIn は POSITION/NORMAL/TEXCOORD0 の
// 任意の部分集合・任意の順でよく、詰めて並べる (APPEND_ALIGNED) と欠けた要素の分だけ
// 後続のオフセットがずれる — 常に同じ 52 バイトの VB (MeshVertex) を指すので、
// オフセットは semantic 名から固定表で引く
bool BuildSurfaceInputLayout(ID3D11Device* device, const std::vector<uint8_t>& vsBytecode,
                             ComPtr<ID3D11InputLayout>& out, std::string& errorOut)
{
    ComPtr<ID3D11ShaderReflection> reflection;
    if (FAILED(D3DReflect(vsBytecode.data(), vsBytecode.size(),
                          IID_PPV_ARGS(reflection.GetAddressOf())))) {
        errorOut = "入力レイアウト: D3DReflect に失敗";
        return false;
    }
    D3D11_SHADER_DESC sd = {};
    reflection->GetDesc(&sd);

    struct FixedElem {
        const char* semantic;
        UINT offset;
        DXGI_FORMAT format;
    };
    static const FixedElem kFixed[] = {
        { "POSITION", offsetof(MeshVertex, position), DXGI_FORMAT_R32G32B32_FLOAT },
        { "NORMAL", offsetof(MeshVertex, normal), DXGI_FORMAT_R32G32B32_FLOAT },
        { "TEXCOORD", offsetof(MeshVertex, uv), DXGI_FORMAT_R32G32_FLOAT },
    };

    std::vector<D3D11_INPUT_ELEMENT_DESC> elems;
    for (UINT i = 0; i < sd.InputParameters; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC pd = {};
        reflection->GetInputParameterDesc(i, &pd);
        if (pd.SystemValueType != D3D_NAME_UNDEFINED) {
            continue;
        }
        const FixedElem* match = nullptr;
        if (pd.SemanticIndex == 0) {
            for (const FixedElem& fe : kFixed) {
                if (std::strcmp(pd.SemanticName, fe.semantic) == 0) {
                    match = &fe;
                    break;
                }
            }
        }
        if (!match) {
            errorOut = std::string("入力レイアウト: 未対応の VSIn semantic ") + pd.SemanticName
                + " (POSITION/NORMAL/TEXCOORD0 の部分集合のみ)";
            return false;
        }
        D3D11_INPUT_ELEMENT_DESC e = {};
        e.SemanticName = match->semantic;
        e.SemanticIndex = 0;
        e.Format = match->format;
        e.InputSlot = 0;
        e.AlignedByteOffset = match->offset; // MeshVertex 固定オフセット (APPEND_ALIGNED は使わない)
        e.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        elems.push_back(e);
    }
    if (elems.empty()) {
        out.Reset();
        return true;
    }
    if (FAILED(device->CreateInputLayout(elems.data(), static_cast<UINT>(elems.size()),
                                         vsBytecode.data(), vsBytecode.size(),
                                         out.GetAddressOf()))) {
        errorOut = "入力レイアウト: CreateInputLayout に失敗";
        return false;
    }
    return true;
}

// バイトコードから予約 CB / 予約テクスチャ・サンプラ / 作者資源を名前で引ける表を作る。
// ComputeAbiRunner::ReflectBytecode (M78e) と同じ D3DReflect の使い方だが、
// **D3D_SVF_USED では絞らない** — 予約 CB のレイアウト検証 (SelfTest) は宣言された
// 全フィールドのオフセットを見る必要があり、使われているかどうかは関係ない
bool ReflectSurfaceBytecode(const std::vector<uint8_t>& bytecode, SurfaceEntryReflection& out)
{
    if (bytecode.empty()) {
        return false;
    }
    ComPtr<ID3D11ShaderReflection> reflection;
    if (FAILED(D3DReflect(bytecode.data(), bytecode.size(),
                          IID_PPV_ARGS(reflection.GetAddressOf())))) {
        return false;
    }
    D3D11_SHADER_DESC sd = {};
    reflection->GetDesc(&sd);

    for (UINT i = 0; i < sd.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC bd = {};
        reflection->GetResourceBindingDesc(i, &bd);
        SurfaceReflectedResource rr;
        rr.bindSlot = bd.BindPoint;
        rr.type = bd.Type;
        out.resources[bd.Name] = rr;
    }
    for (UINT i = 0; i < sd.ConstantBuffers; ++i) {
        ID3D11ShaderReflectionConstantBuffer* cb = reflection->GetConstantBufferByIndex(i);
        D3D11_SHADER_BUFFER_DESC cbDesc = {};
        cb->GetDesc(&cbDesc);
        uint32_t cbSlot = 0;
        if (const auto it = out.resources.find(cbDesc.Name); it != out.resources.end()) {
            cbSlot = it->second.bindSlot;
        }
        for (UINT j = 0; j < cbDesc.Variables; ++j) {
            ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(j);
            D3D11_SHADER_VARIABLE_DESC varDesc = {};
            var->GetDesc(&varDesc);
            SurfaceReflectedVar rv;
            rv.cbufferName = cbDesc.Name; // M79b-fix (review-1 #4): cbuffer ごとに区別するため
            rv.cbufBindSlot = cbSlot;
            rv.cbufSize = cbDesc.Size;
            rv.offset = varDesc.StartOffset;
            rv.size = varDesc.Size;
            out.vars[varDesc.Name] = rv;
        }
    }
    return true;
}

bool IsProjectIndexedShaderFile(const std::wstring& filename)
{
    return (filename.size() >= 11
            && filename.compare(filename.size() - 10, 10, L".post.hlsl") == 0)
        // M79 sub-04: ".cs.hlsl" は 8 文字 (旧コードは 9 文字比較の off-by-one で常に不一致だった)
        || (filename.size() >= 9
            && filename.compare(filename.size() - 8, 8, L".cs.hlsl") == 0)
        // M79: *.surface.hlsl も assets 全域索引に加える (短名 "Foo.surface")
        || (filename.size() >= 14
            && filename.compare(filename.size() - 13, 13, L".surface.hlsl") == 0);
}

// MyTint.post.hlsl → "MyTint.post" (Load 名)
std::string ProjectShaderShortName(const std::wstring& filename)
{
    if (filename.size() <= 5 || filename.compare(filename.size() - 5, 5, L".hlsl") != 0) {
        return {};
    }
    return WideToUtf8(filename.substr(0, filename.size() - 5));
}

} // namespace

void ShaderManager::SetAssetsRoot(std::wstring root)
{
    assetsRoot_ = std::move(root);
}

void ShaderManager::RebuildProjectShaderIndex()
{
    projectShaders_.clear();
    if (assetsRoot_.empty()) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(assetsRoot_, ec)) {
        return;
    }

    std::unordered_map<std::string, std::wstring> first;
    std::unordered_set<std::string> banned;

    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(assetsRoot_, ec)) {
        if (ec || !entry.is_regular_file(ec)) {
            continue;
        }
        const std::wstring filename = entry.path().filename().wstring();
        if (!IsProjectIndexedShaderFile(filename)) {
            continue;
        }
        const std::string key = ProjectShaderShortName(filename);
        if (key.empty()) {
            continue;
        }
        const std::wstring norm = NormalizePathKey(entry.path().wstring());
        if (banned.contains(key)) {
            continue;
        }
        const auto it = first.find(key);
        if (it != first.end()) {
            banned.insert(key);
            MYE_LOG_ERROR(
                "project shader name conflict (both disabled): '%s' at\n  %s\n  %s",
                key.c_str(), WideToUtf8(it->second).c_str(), WideToUtf8(norm).c_str());
            first.erase(it);
            continue;
        }
        first.emplace(key, norm);
    }

    for (const auto& [k, p] : first) {
        if (!banned.contains(k)) {
            projectShaders_.emplace(k, p);
        }
    }
}

std::wstring ShaderManager::ResolveShaderPath(std::string_view name) const
{
    return ResolvePath(name);
}

std::vector<std::string> ShaderManager::ProjectShaderNames(std::string_view suffixFilter) const
{
    std::vector<std::string> out;
    for (const auto& [name, path] : projectShaders_) {
        (void)path;
        if (!suffixFilter.empty()
            && (name.size() < suffixFilter.size()
                || name.compare(name.size() - suffixFilter.size(), suffixFilter.size(),
                               suffixFilter)
                       != 0)) {
            continue;
        }
        out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

PropertyParseResult ShaderManager::FetchPropertySchema(std::string_view name) const
{
    if (name.empty()) {
        return ParseProperties(std::string_view{});
    }
    std::string hlslSrc;
    const std::wstring path = ResolvePath(name);
    std::ifstream f(path, std::ios::binary);
    if (f) {
        hlslSrc.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    return ParseProperties(hlslSrc);
}

bool ShaderManager::Init(GraphicsDevice& device, std::vector<std::wstring> shaderDirs)
{
    device_ = &device;
    dirs_ = std::move(shaderDirs);
    std::error_code ec;
    bool anyExists = false;
    for (const std::wstring& d : dirs_) {
        if (std::filesystem::is_directory(d, ec)) {
            anyExists = true;
        }
    }
    if (!anyExists) {
        MYE_LOG_WARN("no shader dir found (%zu root(s) searched)", dirs_.size());
    }
    for (const std::wstring& d : dirs_) {
        MYE_LOG_INFO("shader root: %s", WideToUtf8(d).c_str());
    }
    ReportShadowedBuiltins();
    return true;
}

// 優先度の高いルートが下位ルートの同名シェーダを隠している箇所を報告する。
// 意図的な上書きなら想定どおりだが、テンプレートからコピーされた古い残骸だと
// 「エンジンを更新したのに挙動が古いまま」という分かりにくい壊れ方をするので、
// 内容が一致するか (= 単なる冗長コピー) まで出して判断材料にする
void ShaderManager::ReportShadowedBuiltins() const
{
    if (dirs_.size() < 2) {
        return;
    }
    std::error_code ec;
    for (size_t i = 0; i + 1 < dirs_.size(); ++i) {
        for (const auto& e : std::filesystem::directory_iterator(dirs_[i], ec)) {
            if (!e.is_regular_file(ec)) {
                continue;
            }
            const std::filesystem::path& p = e.path();
            const std::wstring ext = p.extension().wstring();
            if (ext != L".hlsl" && ext != L".hlsli") {
                continue;
            }
            for (size_t j = i + 1; j < dirs_.size(); ++j) {
                const std::filesystem::path lower =
                    std::filesystem::path(dirs_[j]) / p.filename();
                if (!std::filesystem::is_regular_file(lower, ec)) {
                    continue;
                }
                std::vector<char> a;
                std::vector<char> b;
                const bool same = ReadFileBytes(p.wstring(), a)
                    && ReadFileBytes(lower.wstring(), b) && a == b;
                MYE_LOG_WARN("shader override: %s shadows %s (%s)",
                             WideToUtf8(p.wstring()).c_str(),
                             WideToUtf8(lower.wstring()).c_str(),
                             same ? "identical - redundant copy, safe to delete"
                                  : "DIFFERS - intentional override, or a stale copy that will "
                                    "hide engine updates");
                break;
            }
        }
    }
}

std::wstring ShaderManager::ResolvePath(std::string_view name) const
{
    const std::string key(name);
    const auto indexed = projectShaders_.find(key);
    if (indexed != projectShaders_.end()) {
        return indexed->second;
    }
    const std::wstring file = Utf8ToWide(name) + L".hlsl";
    std::error_code ec;
    for (const std::wstring& d : dirs_) {
        const std::wstring candidate = d + L"\\" + file;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return NormalizePathKey(candidate);
        }
    }
    // 未発見。最優先ルート上のパスを返しておくと、後からプロジェクト側に
    // 同名ファイルを置いたときに FileWatcher の照合が成立する
    return dirs_.empty() ? NormalizePathKey(file) : NormalizePathKey(dirs_.front() + L"\\" + file);
}

AssetID ShaderManager::Load(std::string_view name)
{
    const AssetID id{ HashStr(name) };
    if (programs_.contains(id.value)) {
        return id;
    }
    ShaderProgram prog;
    prog.path = ResolvePath(name);
    // Init 前 (ヘッドレス = --selftest 等、M48a) はコンパイルせず ID だけ予約する
    // (ローダの shaders.Load("forward_lit") をウィンドウ / D3D 無しで通すため)
    if (device_ && !CompileProgram(prog.path, prog)) {
        MYE_LOG_ERROR("shader compile failed: %.*s", static_cast<int>(name.size()), name.data());
    }
    programs_.emplace(id.value, std::move(prog));
    return id;
}

AssetID ShaderManager::LoadCompute(std::string_view name)
{
    const AssetID id{ HashStr(name) };
    if (programs_.contains(id.value)) {
        return id;
    }
    ShaderProgram prog;
    prog.isCompute = true;
    prog.path = ResolvePath(name);
    if (device_ && !CompileProgram(prog.path, prog)) { // Init 前は ID 予約のみ (Load と同じ)
        MYE_LOG_ERROR("compute shader compile failed: %.*s", static_cast<int>(name.size()),
                      name.data());
    }
    programs_.emplace(id.value, std::move(prog));
    return id;
}

ShaderProgram* ShaderManager::Get(AssetID id)
{
    auto it = programs_.find(id.value);
    return (it != programs_.end()) ? &it->second : nullptr;
}

AssetID ShaderManager::LoadSurface(std::string_view name)
{
    const AssetID id{ HashStr(name) };
    if (surfacePrograms_.contains(id.value)) {
        return id;
    }
    SurfaceProgram prog;
    prog.path = ResolvePath(name);
    if (device_ && !CompileSurfaceProgram(prog.path, prog)) { // Init 前は ID 予約のみ
        MYE_LOG_ERROR("surface shader compile failed: %.*s", static_cast<int>(name.size()),
                      name.data());
    }
    surfacePrograms_.emplace(id.value, std::move(prog));
    return id;
}

SurfaceProgram* ShaderManager::GetSurface(AssetID id)
{
    auto it = surfacePrograms_.find(id.value);
    return (it != surfacePrograms_.end()) ? &it->second : nullptr;
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
    fresh.isCompute = it->second.isCompute;
    if (!CompileProgram(fresh.path, fresh)) {
        return false;
    }
    fresh.generation = it->second.generation + 1;
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
        const bool isCompute = prog.isCompute;
        async_.push_back({ id, std::async(std::launch::async, [this, path, isCompute] {
                               ShaderProgram fresh;
                               fresh.path = path;
                               fresh.isCompute = isCompute;
                               CompileProgram(path, fresh);
                               return fresh;
                           }) });
    }

    // M79 sub-02: SurfaceProgram も同じ依存グラフ規則で対象にする
    // (作者ファイル・MyEngineSurface.hlsli・MyEngineSurfaceEntries.hlsli のいずれか)
    for (auto& [id, prog] : surfacePrograms_) {
        bool affected = (prog.path == normalizedPath);
        if (!affected) {
            for (const std::wstring& inc : prog.includes) {
                if (inc == normalizedPath) {
                    affected = true;
                    break;
                }
            }
        }
        if (!affected) {
            continue;
        }
        bool alreadyPending = false;
        for (const AsyncSurfaceCompile& ac : asyncSurface_) {
            if (ac.id == id) {
                alreadyPending = true;
                break;
            }
        }
        if (alreadyPending) {
            continue;
        }
        MYE_LOG_INFO("[reload] surface shader recompiling: %s", WideToUtf8(prog.path).c_str());
        const std::wstring path = prog.path;
        const uint64_t previousGeneration = prog.generation;
        asyncSurface_.push_back(
            { id, std::async(std::launch::async, [this, path, previousGeneration] {
                 SurfaceProgram fresh;
                 fresh.path = path;
                 CompileSurfaceProgram(path, fresh, previousGeneration);
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
            ShaderProgram& slot = programs_[async_[i].id];
            fresh.generation = slot.generation + 1;
            slot = std::move(fresh); // セーフポイントでの差し替え (フェーズ 2)
            MYE_LOG_INFO("[reload] shader swapped");
        } else {
            MYE_LOG_WARN("[reload] shader compile failed - keeping previous shader");
        }
        async_.erase(async_.begin() + static_cast<ptrdiff_t>(i));
    }

    for (size_t i = 0; i < asyncSurface_.size();) {
        if (asyncSurface_[i].future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++i;
            continue;
        }
        SurfaceProgram fresh = asyncSurface_[i].future.get();
        if (fresh.valid) {
            surfacePrograms_[asyncSurface_[i].id] = std::move(fresh);
            MYE_LOG_INFO("[reload] surface shader swapped");
        } else {
            MYE_LOG_WARN("[reload] surface shader compile failed - keeping previous shader: %s",
                         fresh.errorMessage.c_str());
        }
        asyncSurface_.erase(asyncSurface_.begin() + static_cast<ptrdiff_t>(i));
    }
}

// フラグは全構成で同一にする (Debug/Release で描画結果に差を作らない)
static constexpr UINT kCompileFlags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;

void ShaderManager::SetCacheDir(std::wstring dir, bool enabled)
{
    cacheDir_ = enabled ? std::move(dir) : std::wstring{};
    if (!cacheDir_.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(cacheDir_, ec);
    }
}

std::wstring ShaderManager::ResolveInclude(const char* name, std::vector<char>* outData) const
{
    std::vector<char> scratch;
    std::vector<char>& data = outData ? *outData : scratch;
    for (const std::wstring& base : dirs_) {
        const std::wstring path = NormalizePathKey(base + L"\\" + Utf8ToWide(name));
        if (ReadFileBytes(path, data)) {
            return path;
        }
    }
    return {};
}

bool ShaderManager::TryLoadCached(const std::wstring& path, const std::vector<char>& source,
                                  ShaderProgram& out)
{
    if (cacheDir_.empty()) {
        return false;
    }
    std::vector<char> raw;
    if (!ReadFileBytes(cacheDir_ + L"\\" + ShaderCacheFileName(path, out.isCompute), raw)) {
        return false;
    }
    ShaderCacheEntry entry;
    if (!DecodeShaderCacheEntry(std::vector<uint8_t>(raw.begin(), raw.end()), entry)
        || entry.isCompute != out.isCompute
        || entry.configKey != ShaderCacheConfigKey(out.isCompute, kCompileFlags)
        || entry.sourceHash != HashBytes(source.data(), source.size())) {
        return false;
    }
    // include は 1 本ずつ「今解決したら同じファイルか」「中身は同じか」を確かめる。
    // ★本体が変わっていなくても、共通 .hlsli (rt_common 等) だけ直した場合はここで落ちる
    std::vector<std::wstring> includes;
    for (const ShaderCacheEntry::Dependency& d : entry.deps) {
        std::vector<char> data;
        const std::wstring now = ResolveInclude(d.requestedName.c_str(), &data);
        if (now.empty() || WideToUtf8(now) != d.resolvedPath
            || HashBytes(data.data(), data.size()) != d.contentHash) {
            return false;
        }
        includes.push_back(now);
    }
    if (!Instantiate(WideToUtf8(path), entry.blobs, out)) {
        return false;
    }
    out.includes = std::move(includes);
    return true;
}

bool ShaderManager::Instantiate(const std::string& pathUtf8,
                                const std::vector<std::vector<uint8_t>>& blobs, ShaderProgram& out)
{
    ID3D11Device* dev = device_->Device();
    out.cs.Reset();
    out.vs.Reset();
    out.ps.Reset();
    out.inputLayout.Reset();
    if (out.isCompute) {
        if (blobs.size() != 1
            || FAILED(dev->CreateComputeShader(blobs[0].data(), blobs[0].size(), nullptr,
                                               out.cs.GetAddressOf()))) {
            MYE_LOG_ERROR("compute shader creation failed: %s", pathUtf8.c_str());
            return false;
        }
        // v21 (M78e): バイトコードを保持して ComputeAbiRunner の D3DReflect 名前バインドを可能にする
        out.csBytecode = blobs[0];
        return true;
    }
    if (blobs.size() != 2
        || FAILED(dev->CreateVertexShader(blobs[0].data(), blobs[0].size(), nullptr,
                                          out.vs.GetAddressOf()))
        || FAILED(dev->CreatePixelShader(blobs[1].data(), blobs[1].size(), nullptr,
                                         out.ps.GetAddressOf()))) {
        MYE_LOG_ERROR("shader object creation failed: %s", pathUtf8.c_str());
        return false;
    }
    if (!BuildInputLayout(dev, blobs[0], out.inputLayout)) {
        MYE_LOG_ERROR("input layout creation failed: %s", pathUtf8.c_str());
        return false;
    }
    return true;
}

bool ShaderManager::CompileProgram(const std::wstring& path, ShaderProgram& out)
{
    std::vector<char> source;
    if (!ReadFileBytes(path, source)) {
        MYE_LOG_ERROR("shader file not found: %s", WideToUtf8(path).c_str());
        return false;
    }
    const std::string pathUtf8 = WideToUtf8(path);

    if (TryLoadCached(path, source, out)) {
        out.valid = true;
        ++cacheHits_;
        MYE_LOG_INFO("shader loaded from cache: %s", pathUtf8.c_str());
        return true;
    }

    out.includes.clear();
    ShaderCacheEntry entry;
    IncludeRecorder includer(
        [this](const char* name, std::vector<char>* data) { return ResolveInclude(name, data); },
        out.includes, entry.deps);

    auto compile = [&](const char* entryPoint, const char* target, std::vector<uint8_t>& bytecode) {
        ComPtr<ID3DBlob> code;
        ComPtr<ID3DBlob> errors;
        const HRESULT hr = D3DCompile(source.data(), source.size(), pathUtf8.c_str(), nullptr,
                                      &includer, entryPoint, target, kCompileFlags, 0,
                                      code.GetAddressOf(), errors.GetAddressOf());
        if (FAILED(hr)) {
            const char* msg = errors ? static_cast<const char*>(errors->GetBufferPointer())
                                     : "(no error output)";
            MYE_LOG_ERROR("HLSL %s (%s):\n%s", entryPoint, pathUtf8.c_str(), msg);
            return false;
        }
        if (errors && errors->GetBufferSize() > 1) {
            MYE_LOG_WARN("HLSL %s (%s):\n%s", entryPoint, pathUtf8.c_str(),
                         static_cast<const char*>(errors->GetBufferPointer()));
        }
        const auto* p = static_cast<const uint8_t*>(code->GetBufferPointer());
        bytecode.assign(p, p + code->GetBufferSize());
        return true;
    };

    if (out.isCompute) {
        entry.blobs.resize(1);
        if (!compile("CSMain", "cs_5_0", entry.blobs[0])) {
            return false;
        }
    } else {
        entry.blobs.resize(2);
        if (!compile("VSMain", "vs_5_0", entry.blobs[0])
            || !compile("PSMain", "ps_5_0", entry.blobs[1])) {
            return false;
        }
    }
    // VS と PS の 2 回のコンパイルが同じ include を 2 度記録するので、依存は重複を落とす
    // (落とさなくても正しいが、鮮度判定で同じファイルを 2 回読むだけになる)
    {
        std::vector<ShaderCacheEntry::Dependency> unique;
        for (const ShaderCacheEntry::Dependency& d : entry.deps) {
            bool seen = false;
            for (const ShaderCacheEntry::Dependency& u : unique) {
                seen = seen
                    || (u.requestedName == d.requestedName && u.resolvedPath == d.resolvedPath);
            }
            if (!seen) {
                unique.push_back(d);
            }
        }
        entry.deps = std::move(unique);
    }
    if (!Instantiate(pathUtf8, entry.blobs, out)) {
        return false;
    }
    out.valid = true;
    ++cacheMisses_;
    MYE_LOG_INFO("shader compiled: %s", pathUtf8.c_str());

    if (!cacheDir_.empty()) {
        entry.configKey = ShaderCacheConfigKey(out.isCompute, kCompileFlags);
        entry.isCompute = out.isCompute;
        entry.sourceHash = HashBytes(source.data(), source.size());
        const std::vector<uint8_t> bytes = EncodeShaderCacheEntry(entry);
        const std::wstring finalPath =
            cacheDir_ + L"\\" + ShaderCacheFileName(path, out.isCompute);
        // ★テンポラリ → rename (CookedCache と同じ理由)。replay_verify は複数プロセスを並列に
        //   起動するので、同じシェーダを同時に書きうる。PID を混ぜた名前なら混線せず、
        //   rename に勝った側の「完全な内容」だけが残る。書けなくても描画には影響しない
        const std::wstring tmpPath = finalPath + L"." + std::to_wstring(_getpid()) + L".tmp";
        bool written = false;
        {
            std::ofstream f(std::filesystem::path(tmpPath), std::ios::binary | std::ios::trunc);
            if (f) {
                f.write(reinterpret_cast<const char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()));
                written = f.good();
            }
        }
        std::error_code ec;
        if (written) {
            std::filesystem::rename(tmpPath, finalPath, ec);
        }
        if (!written || ec) {
            std::filesystem::remove(tmpPath, ec);
        }
    }
    return true;
}

bool ShaderManager::InstantiateSurface(const std::vector<std::vector<uint8_t>>& blobs,
                                       SurfaceProgram& out)
{
    if (blobs.size() != 5) {
        out.errorMessage = "サーフェス規約: キャッシュ形式が不正 (blob 数不一致)";
        return false;
    }
    const std::vector<uint8_t>& colorVsBytecode = blobs[0];
    const std::vector<uint8_t>& colorPsBytecode = blobs[1];
    const std::vector<uint8_t>& shadowVsBytecode = blobs[2];
    const std::vector<uint8_t>& velVsBytecode = blobs[3];
    const std::vector<uint8_t>& velPsBytecode = blobs[4];

    ID3D11Device* dev = device_->Device();
    out.colorVS.Reset();
    out.colorPS.Reset();
    out.colorInputLayout.Reset();
    out.shadowVS.Reset();
    out.shadowInputLayout.Reset();
    out.velocityVS.Reset();
    out.velocityPS.Reset();
    out.velocityInputLayout.Reset();

    if (FAILED(dev->CreateVertexShader(colorVsBytecode.data(), colorVsBytecode.size(), nullptr,
                                       out.colorVS.GetAddressOf()))
        || FAILED(dev->CreatePixelShader(colorPsBytecode.data(), colorPsBytecode.size(), nullptr,
                                         out.colorPS.GetAddressOf()))
        || FAILED(dev->CreateVertexShader(shadowVsBytecode.data(), shadowVsBytecode.size(),
                                          nullptr, out.shadowVS.GetAddressOf()))
        || FAILED(dev->CreateVertexShader(velVsBytecode.data(), velVsBytecode.size(), nullptr,
                                          out.velocityVS.GetAddressOf()))
        || FAILED(dev->CreatePixelShader(velPsBytecode.data(), velPsBytecode.size(), nullptr,
                                         out.velocityPS.GetAddressOf()))) {
        out.errorMessage = "サーフェス規約: シェーダオブジェクト作成に失敗";
        MYE_LOG_ERROR("%s", out.errorMessage.c_str());
        return false;
    }

    std::string layoutErr;
    if (!BuildSurfaceInputLayout(dev, colorVsBytecode, out.colorInputLayout, layoutErr)
        || !BuildSurfaceInputLayout(dev, shadowVsBytecode, out.shadowInputLayout, layoutErr)
        || !BuildSurfaceInputLayout(dev, velVsBytecode, out.velocityInputLayout, layoutErr)) {
        out.errorMessage = "サーフェス規約: " + layoutErr;
        MYE_LOG_ERROR("%s", out.errorMessage.c_str());
        return false;
    }

    ReflectSurfaceBytecode(colorVsBytecode, out.colorVSReflect);
    ReflectSurfaceBytecode(colorPsBytecode, out.colorPSReflect);
    ReflectSurfaceBytecode(shadowVsBytecode, out.shadowVSReflect);
    ReflectSurfaceBytecode(velVsBytecode, out.velocityVSReflect);
    ReflectSurfaceBytecode(velPsBytecode, out.velocityPSReflect);
    return true;
}

bool ShaderManager::TryLoadCachedSurface(const std::wstring& path, const std::vector<char>& combined,
                                         SurfaceProgram& out)
{
    if (cacheDir_.empty()) {
        return false;
    }
    std::vector<char> raw;
    if (!ReadFileBytes(cacheDir_ + L"\\" + SurfaceShaderCacheFileName(path), raw)) {
        return false;
    }
    ShaderCacheEntry entry;
    if (!DecodeShaderCacheEntry(std::vector<uint8_t>(raw.begin(), raw.end()), entry)
        || !entry.isSurface
        || entry.configKey != SurfaceShaderCacheConfigKey(kCompileFlags)
        || entry.sourceHash != HashBytes(combined.data(), combined.size())) {
        return false;
    }
    std::vector<std::wstring> includes;
    for (const ShaderCacheEntry::Dependency& d : entry.deps) {
        std::vector<char> data;
        const std::wstring now = ResolveInclude(d.requestedName.c_str(), &data);
        if (now.empty() || WideToUtf8(now) != d.resolvedPath
            || HashBytes(data.data(), data.size()) != d.contentHash) {
            return false;
        }
        includes.push_back(now);
    }
    if (!InstantiateSurface(entry.blobs, out)) {
        return false;
    }
    out.includes = std::move(includes);
    return true;
}

// M79: 作者ソース + MyEngineSurfaceEntries.hlsli (生成エントリ) を 1 つの翻訳単位として
// 5 エントリ (色 VS/PS・影 VS・速度 VS/PS) を個別コンパイルする。
// バイトコードキャッシュのキーは「生成エントリ込みのソース全体」(combined) のハッシュ
// + 依存 include の中身ハッシュ (spec §4.4)。previousGeneration+1 を成功時の世代番号にする
bool ShaderManager::CompileSurfaceProgram(const std::wstring& path, SurfaceProgram& out,
                                          uint64_t previousGeneration)
{
    std::vector<char> authorSource;
    if (!ReadFileBytes(path, authorSource)) {
        out.errorMessage = "shader file not found: " + WideToUtf8(path);
        MYE_LOG_ERROR("%s", out.errorMessage.c_str());
        return false;
    }
    const std::string pathUtf8 = WideToUtf8(path);

    // Properties ブロックのパース (M79 sub-02)。失敗 = シェーダ全体を無効 (spec §4.1 失敗時表)
    out.propertiesSchema =
        ParseProperties(std::string_view(authorSource.data(), authorSource.size()));
    if (!out.propertiesSchema.ok) {
        out.errorMessage = "Properties パース失敗: " + out.propertiesSchema.errorMessage;
        MYE_LOG_ERROR("HLSL surface (%s): %s", pathUtf8.c_str(), out.errorMessage.c_str());
        return false;
    }

    // 作者ソースの直後に生成エントリの include を足す。#include なので IncludeRecorder の
    // 依存グラフに自然に乗り、MyEngineSurfaceEntries.hlsli の編集もホットリロード対象になる
    static constexpr char kEntriesInclude[] = "\n#include \"MyEngineSurfaceEntries.hlsli\"\n";
    std::vector<char> combined(authorSource);
    combined.insert(combined.end(), kEntriesInclude,
                    kEntriesInclude + (sizeof(kEntriesInclude) - 1));

    if (TryLoadCachedSurface(path, combined, out)) {
        out.valid = true;
        out.generation = previousGeneration + 1;
        ++cacheHits_;
        MYE_LOG_INFO("surface shader loaded from cache: %s", pathUtf8.c_str());
        return true;
    }

    out.includes.clear();
    ShaderCacheEntry entry; // deps 収集 + (キャッシュ有効時) 書き出しに使う
    IncludeRecorder includer(
        [this](const char* name, std::vector<char>* data) { return ResolveInclude(name, data); },
        out.includes, entry.deps);

    auto compile = [&](const char* entryPoint, const char* target,
                       std::vector<uint8_t>& bytecode, std::string& errOut) {
        ComPtr<ID3DBlob> code;
        ComPtr<ID3DBlob> errors;
        const HRESULT hr = D3DCompile(combined.data(), combined.size(), pathUtf8.c_str(), nullptr,
                                      &includer, entryPoint, target, kCompileFlags, 0,
                                      code.GetAddressOf(), errors.GetAddressOf());
        if (FAILED(hr)) {
            errOut = errors ? static_cast<const char*>(errors->GetBufferPointer())
                            : "(no error output)";
            return false;
        }
        if (errors && errors->GetBufferSize() > 1) {
            MYE_LOG_WARN("HLSL %s (%s):\n%s", entryPoint, pathUtf8.c_str(),
                         static_cast<const char*>(errors->GetBufferPointer()));
        }
        const auto* p = static_cast<const uint8_t*>(code->GetBufferPointer());
        bytecode.assign(p, p + code->GetBufferSize());
        return true;
    };

    std::vector<uint8_t> colorVsBytecode;
    std::vector<uint8_t> colorPsBytecode;
    std::vector<uint8_t> shadowVsBytecode;
    std::vector<uint8_t> velVsBytecode;
    std::vector<uint8_t> velPsBytecode;
    std::string err;

    const bool ok = compile("MyeVSColor", "vs_5_0", colorVsBytecode, err)
        && compile("MyePSColor", "ps_5_0", colorPsBytecode, err)
        && compile("MyeVSShadow", "vs_5_0", shadowVsBytecode, err)
        && compile("MyeVSVelocity", "vs_5_0", velVsBytecode, err)
        && compile("MyePSVelocity", "ps_5_0", velPsBytecode, err);

    if (!ok) {
        out.errorMessage =
            "サーフェス規約: VSOut VSMain(VSIn v) / float4 PSMain(VSOut i) : SV_Target / "
            "VSOut.pos : SV_Position\n" + err;
        MYE_LOG_ERROR("HLSL surface (%s):\n%s", pathUtf8.c_str(), out.errorMessage.c_str());
        return false;
    }

    // VS/PS 5 回のコンパイルが同じ include を何度も記録するので重複を落とす
    // (落とさなくても正しいが、鮮度判定・キャッシュ書き出しが同じファイルを何度も読むだけになる)
    {
        std::vector<ShaderCacheEntry::Dependency> uniqueDeps;
        for (const ShaderCacheEntry::Dependency& d : entry.deps) {
            bool seen = false;
            for (const ShaderCacheEntry::Dependency& u : uniqueDeps) {
                seen = seen
                    || (u.requestedName == d.requestedName && u.resolvedPath == d.resolvedPath);
            }
            if (!seen) {
                uniqueDeps.push_back(d);
            }
        }
        entry.deps = std::move(uniqueDeps);
        std::vector<std::wstring> uniqueIncludes;
        for (const std::wstring& inc : out.includes) {
            if (std::find(uniqueIncludes.begin(), uniqueIncludes.end(), inc)
                == uniqueIncludes.end()) {
                uniqueIncludes.push_back(inc);
            }
        }
        out.includes = std::move(uniqueIncludes);
    }

    if (!InstantiateSurface({ colorVsBytecode, colorPsBytecode, shadowVsBytecode, velVsBytecode,
                             velPsBytecode },
                           out)) {
        return false;
    }

    out.valid = true;
    out.generation = previousGeneration + 1;
    ++cacheMisses_;
    MYE_LOG_INFO("surface shader compiled: %s", pathUtf8.c_str());

    if (!cacheDir_.empty()) {
        entry.configKey = SurfaceShaderCacheConfigKey(kCompileFlags);
        entry.isCompute = false;
        entry.isSurface = true;
        entry.sourceHash = HashBytes(combined.data(), combined.size());
        entry.blobs = { colorVsBytecode, colorPsBytecode, shadowVsBytecode, velVsBytecode,
                       velPsBytecode };
        const std::vector<uint8_t> bytes = EncodeShaderCacheEntry(entry);
        const std::wstring finalPath = cacheDir_ + L"\\" + SurfaceShaderCacheFileName(path);
        // ★テンポラリ → rename (CompileProgram と同じ理由。replay_verify の並列起動対策)
        const std::wstring tmpPath = finalPath + L"." + std::to_wstring(_getpid()) + L".tmp";
        bool written = false;
        {
            std::ofstream f(std::filesystem::path(tmpPath), std::ios::binary | std::ios::trunc);
            if (f) {
                f.write(reinterpret_cast<const char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()));
                written = f.good();
            }
        }
        std::error_code ec;
        if (written) {
            std::filesystem::rename(tmpPath, finalPath, ec);
        }
        if (!written || ec) {
            std::filesystem::remove(tmpPath, ec);
        }
    }
    return true;
}

} // namespace mye
