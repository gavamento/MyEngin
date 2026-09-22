/*----
 ComputeAbiRunner.cpp  ABI v21 コンピュートバッファ管理・名前バインドディスパッチ (M78e)
 作成者: 秋田蓮音                                09/22/2026
----*/
#include "Engine/Renderer/ComputeAbiRunner.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include <d3dcompiler.h>

#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Renderer/GpuBufferUtil.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ShaderManager.h"

// EngineAPI.h の usageFlags 定数と同値
#define MYE_COMPUTE_BUFFER_UAV 0x02u

namespace mye {
namespace {

// ハンドルの上位/下位 32bit
inline uint32_t HandleGen(uint64_t h)   { return static_cast<uint32_t>(h >> 32); }
inline uint32_t HandleIndex(uint64_t h) { return static_cast<uint32_t>(h & 0xFFFFFFFFull); }
inline uint64_t MakeHandle(uint32_t gen, uint32_t idx)
{
    return (static_cast<uint64_t>(gen) << 32) | static_cast<uint64_t>(idx);
}

// シェーダのバイトコードからリソースバインド情報を一括収集する
struct ReflectedResource
{
    uint32_t               bindSlot = 0;
    D3D_SHADER_INPUT_TYPE  type     = D3D_SIT_CBUFFER;
};

// シェーダのバイトコードから cbuffer 変数のオフセット情報を収集する
struct ReflectedVar
{
    uint32_t cbufBindSlot = 0; // 所属 cbuffer の bind slot
    uint32_t cbufSize     = 0; // cbuffer 全体のバイト数
    uint32_t offset       = 0; // 変数の cbuffer 内オフセット
    uint32_t size         = 0; // 変数のバイト数
};

struct ReflectResult
{
    // リソース名 → バインドスロット & 型
    std::unordered_map<std::string, ReflectedResource> resources;
    // 変数名 → cbuffer 内の位置
    std::unordered_map<std::string, ReflectedVar>      vars;
};

static bool IsUavType(D3D_SHADER_INPUT_TYPE type)
{
    return type == D3D_SIT_UAV_RWTYPED || type == D3D_SIT_UAV_RWSTRUCTURED
        || type == D3D_SIT_UAV_RWBYTEADDRESS || type == D3D_SIT_UAV_APPEND_STRUCTURED
        || type == D3D_SIT_UAV_CONSUME_STRUCTURED
        || type == D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER;
}

static bool IsBufferResource(D3D_SHADER_INPUT_TYPE type)
{
    return IsUavType(type) || type == D3D_SIT_STRUCTURED || type == D3D_SIT_BYTEADDRESS
        || type == D3D_SIT_TBUFFER;
}

// HashStr("white") と TextureLibrary::White() の "builtin://white"
static bool IsBuiltinWhiteKey(uint64_t assetId)
{
    return assetId == HashStr("white") || assetId == HashStr("builtin://white");
}

// バイトコードをリフレクションして resources / vars を埋める
static bool ReflectBytecode(const std::vector<uint8_t>& bytecode, ReflectResult& out)
{
    if (bytecode.empty()) return false;

    Microsoft::WRL::ComPtr<ID3D11ShaderReflection> pReflect;
    if (FAILED(D3DReflect(bytecode.data(), bytecode.size(),
                          IID_PPV_ARGS(pReflect.GetAddressOf()))))
    {
        return false;
    }

    D3D11_SHADER_DESC sd;
    pReflect->GetDesc(&sd);

    // バインドされているリソース (SRV / UAV / CBV) を列挙
    for (UINT i = 0; i < sd.BoundResources; ++i)
    {
        D3D11_SHADER_INPUT_BIND_DESC bd;
        pReflect->GetResourceBindingDesc(i, &bd);
        ReflectedResource rr;
        rr.bindSlot = bd.BindPoint;
        rr.type     = bd.Type;
        out.resources[bd.Name] = rr;
    }

    // 定数バッファ内の変数を列挙
    for (UINT i = 0; i < sd.ConstantBuffers; ++i)
    {
        ID3D11ShaderReflectionConstantBuffer* cb = pReflect->GetConstantBufferByIndex(i);
        D3D11_SHADER_BUFFER_DESC cbDesc;
        cb->GetDesc(&cbDesc);

        // cbuffer 自体の bind slot を resources から引く
        uint32_t cbSlot = 0;
        if (auto it = out.resources.find(cbDesc.Name); it != out.resources.end())
            cbSlot = it->second.bindSlot;

        for (UINT j = 0; j < cbDesc.Variables; ++j)
        {
            ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(j);
            D3D11_SHADER_VARIABLE_DESC varDesc;
            var->GetDesc(&varDesc);
            if ((varDesc.uFlags & D3D_SVF_USED) == 0) continue; // 未使用変数はスキップ

            ReflectedVar rv;
            rv.cbufBindSlot = cbSlot;
            rv.cbufSize     = cbDesc.Size;
            rv.offset       = varDesc.StartOffset;
            rv.size         = varDesc.Size;
            out.vars[varDesc.Name] = rv;
        }
    }

    return true;
}

// LoadCompute してリフレクションする。無効シェーダは false
static bool ReflectShader(ShaderManager& shaders, const char* shader, ReflectResult& out)
{
    if (!shader || !shader[0]) return false;
    const AssetID id = shaders.LoadCompute(shader);
    ShaderProgram* prog = shaders.Get(id);
    if (!prog || !prog->valid || prog->csBytecode.empty()) return false;
    return ReflectBytecode(prog->csBytecode, out);
}

} // namespace

// ---------------------------------------------------------------------------
// バッファ管理
// ---------------------------------------------------------------------------

ComputeAbiRunner::BufferSlot* ComputeAbiRunner::ResolveSlot(uint64_t id)
{
    if (id == 0) return nullptr;
    const uint32_t idx = HandleIndex(id);
    const uint32_t gen = HandleGen(id);
    if (idx >= slotCount_) return nullptr;
    BufferSlot& slot = bufSlots_[idx];
    if (!slot.live || slot.generation != gen || gen == 0) return nullptr;
    return &slot;
}

uint64_t ComputeAbiRunner::CreateBuffer(ID3D11Device* dev,
                                        uint32_t count, uint32_t stride, uint32_t flags)
{
    if (!dev || count == 0 || stride == 0)
    {
        MYE_LOG_WARN("ComputeAbiRunner::CreateBuffer: デバイス null またはパラメータ 0。失敗。");
        return 0;
    }
    if (static_cast<int>(slotCount_) - static_cast<int>(freeList_.size()) >= kMaxAbiBuffers)
    {
        MYE_LOG_ERROR("ComputeAbiRunner: ABI バッファ上限 %d に達しています。", kMaxAbiBuffers);
        return 0;
    }

    // スロット確保 (固定表。vector 再配置で COM ポインタを動かさない)
    uint32_t idx;
    if (!freeList_.empty())
    {
        idx = freeList_.back();
        freeList_.pop_back();
    }
    else
    {
        idx = slotCount_++;
    }

    if (idx >= static_cast<uint32_t>(kMaxAbiBuffers))
    {
        MYE_LOG_ERROR("ComputeAbiRunner: スロット index %u が上限外。", idx);
        return 0;
    }

    BufferSlot& slot = bufSlots_[idx];
    slot.buf.Reset();
    slot.srv.Reset();
    slot.uav.Reset();
    slot.hasUav = (flags & MYE_COMPUTE_BUFFER_UAV) != 0;

    // ComPtr::operator& は ComPtrRef を返し、ポインタへ変換するときに Release する。
    // アドレスが必要なときは addressof を使う。
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>* pUav =
        slot.hasUav ? std::addressof(slot.uav) : nullptr;

    if (!gpubuf::CreateStructured(dev, stride, count, nullptr, 0,
                                  slot.buf, pUav, std::addressof(slot.srv)))
    {
        MYE_LOG_ERROR("ComputeAbiRunner: StructuredBuffer 作成失敗 (count=%u, stride=%u)。",
                      count, stride);
        // スロットは空き状態のまま返す
        slot.generation = 0;
        freeList_.push_back(idx);
        return 0;
    }

    // 世代は単調増加。0 は無効ハンドルに予約する
    uint32_t gen = nextGeneration_++;
    if (gen == 0)
    {
        gen = nextGeneration_++;
    }
    slot.generation = gen;
    slot.live = true;
    return MakeHandle(gen, idx);
}

void ComputeAbiRunner::ReleaseBuffer(uint64_t id)
{
    BufferSlot* slot = ResolveSlot(id);
    if (!slot) return; // 無効 ID・二重解放は no-op

    slot->buf.Reset();
    slot->srv.Reset();
    slot->uav.Reset();
    slot->live = false; // 世代は残す。同じ index の再確保は別ハンドルになる
    freeList_.push_back(HandleIndex(id));
}

// ---------------------------------------------------------------------------
// per-shader ペンディング状態の設定
// ---------------------------------------------------------------------------

ComputeAbiRunner::ShaderState& ComputeAbiRunner::GetOrCreateState(const std::string& shader)
{
    return shaderStates_[shader];
}

int ComputeAbiRunner::SetBuffer(ShaderManager& shaders, const char* shader, const char* bufName,
                                uint64_t bufferId)
{
    if (!shader || !shader[0] || !bufName || !bufName[0]) return 0;
    if (!ResolveSlot(bufferId))
    {
        MYE_LOG_WARN("ComputeAbiRunner::SetBuffer: 無効なバッファハンドル。");
        return 0;
    }
    ReflectResult reflect;
    if (!ReflectShader(shaders, shader, reflect)) return 0;
    const auto it = reflect.resources.find(bufName);
    if (it == reflect.resources.end() || !IsBufferResource(it->second.type)) return 0;
    GetOrCreateState(shader).bufferBindings[bufName] = bufferId;
    return 1;
}

int ComputeAbiRunner::SetFloat(ShaderManager& shaders, const char* shader, const char* propName,
                               float value)
{
    if (!shader || !shader[0] || !propName || !propName[0]) return 0;
    ReflectResult reflect;
    if (!ReflectShader(shaders, shader, reflect) || !reflect.vars.contains(propName)) return 0;
    auto& entry = GetOrCreateState(shader).floatValues[propName];
    entry[0] = value;
    entry[1] = 0.0f;
    entry[2] = 0.0f;
    entry[3] = 0.0f;
    return 1;
}

int ComputeAbiRunner::SetFloat4(ShaderManager& shaders, const char* shader, const char* propName,
                                 float x, float y, float z, float w)
{
    if (!shader || !shader[0] || !propName || !propName[0]) return 0;
    ReflectResult reflect;
    if (!ReflectShader(shaders, shader, reflect) || !reflect.vars.contains(propName)) return 0;
    auto& entry = GetOrCreateState(shader).floatValues[propName];
    entry[0] = x;
    entry[1] = y;
    entry[2] = z;
    entry[3] = w;
    return 1;
}

int ComputeAbiRunner::SetTextureFromAsset(ShaderManager& shaders, TextureLibrary* texLib,
                                           const char* shader, const char* texName,
                                           uint64_t assetId)
{
    if (!shader || !shader[0] || !texName || !texName[0] || assetId == 0) return 0;
    const bool known = IsBuiltinWhiteKey(assetId)
        || (texLib && texLib->Get(AssetID{ assetId }) != nullptr);
    if (!known) return 0;
    ReflectResult reflect;
    if (!ReflectShader(shaders, shader, reflect)) return 0;
    const auto it = reflect.resources.find(texName);
    if (it == reflect.resources.end() || it->second.type != D3D_SIT_TEXTURE) return 0;
    GetOrCreateState(shader).textureBindings[texName] = assetId;
    return 1;
}

// ---------------------------------------------------------------------------
// ディスパッチ
// ---------------------------------------------------------------------------

int ComputeAbiRunner::Dispatch(GraphicsDevice& device, ShaderManager& shaders,
                               TextureLibrary* texLib,
                               const char* shader, uint32_t gx, uint32_t gy, uint32_t gz)
{
    if (!shader || gx == 0 || gy == 0 || gz == 0)
    {
        MYE_LOG_WARN("ComputeAbiRunner::Dispatch: 無効なパラメータ (shader=%s, groups=%u,%u,%u)。",
                     shader ? shader : "(null)", gx, gy, gz);
        return 0;
    }

    ID3D11Device*        dev = device.Device();
    ID3D11DeviceContext* dc  = device.Context();
    if (!dev || !dc) return 0;

    // シェーダのロードと有効性確認
    const AssetID id   = shaders.LoadCompute(shader);
    ShaderProgram* prog = shaders.Get(id);
    if (!prog || !prog->valid || !prog->cs)
    {
        MYE_LOG_ERROR("ComputeAbiRunner: シェーダ '%s' が無効。Dispatch をスキップ。", shader);
        return 0;
    }

    // リフレクション (バイトコードがあれば名前バインド; なければ no-op)
    ReflectResult reflect;
    const bool hasReflect = ReflectBytecode(prog->csBytecode, reflect);

    // ペンディング状態の取得
    auto stIt = shaderStates_.find(shader);

    // 名前バインドはリフレクションの bind slot だけを使う (先着順のフォールバックはしない)
    constexpr int kMaxSrvSlots = 16;
    constexpr int kMaxUavSlots = 8; // D3D11 CS の UAV レジスタ上限
    ID3D11ShaderResourceView* srvs[kMaxSrvSlots] = {};
    ID3D11UnorderedAccessView* uavs[kMaxUavSlots] = {};

    if (stIt != shaderStates_.end() && hasReflect)
    {
        const ShaderState& state = stIt->second;

        for (const auto& [name, bufferId] : state.bufferBindings)
        {
            BufferSlot* bslot = ResolveSlot(bufferId);
            if (!bslot) continue;
            const auto it = reflect.resources.find(name);
            if (it == reflect.resources.end()) continue;
            const uint32_t slot = it->second.bindSlot;
            if (IsUavType(it->second.type))
            {
                if (!bslot->uav || slot >= static_cast<uint32_t>(kMaxUavSlots)) continue;
                uavs[slot] = bslot->uav.Get();
            }
            else if (bslot->srv && slot < static_cast<uint32_t>(kMaxSrvSlots))
            {
                srvs[slot] = bslot->srv.Get();
            }
        }

        for (const auto& [name, assetId] : state.textureBindings)
        {
            const auto it = reflect.resources.find(name);
            if (it == reflect.resources.end() || it->second.type != D3D_SIT_TEXTURE) continue;
            Texture* tex = nullptr;
            if (texLib)
            {
                if (IsBuiltinWhiteKey(assetId))
                    tex = texLib->Get(texLib->White());
                else
                    tex = texLib->Get(AssetID{ assetId });
            }
            if (!tex || !tex->srv)
            {
                MYE_LOG_WARN("ComputeAbiRunner: テクスチャ '%s' (assetId=%llu) を解決できません。",
                             name.c_str(), static_cast<unsigned long long>(assetId));
                continue;
            }
            const uint32_t slot = it->second.bindSlot;
            if (slot < static_cast<uint32_t>(kMaxSrvSlots))
                srvs[slot] = tex->srv.Get();
        }
    }
    dc->CSSetShaderResources(0, kMaxSrvSlots, srvs);
    dc->CSSetUnorderedAccessViews(0, kMaxUavSlots, uavs, nullptr);

    // --- 定数バッファ (float / float4 値を名前バインドで Pack) ---
    // cbuffer ごとに blob を作り CSSetConstantBuffers で渡す
    constexpr int kMaxCbSlots = 4;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cbBufs[kMaxCbSlots];

    if (stIt != shaderStates_.end() && hasReflect)
    {
        const ShaderState& state = stIt->second;

        // cbuffer slot ごとにデータ blob を構築
        struct CbBlobInfo { uint32_t slot = 0; std::vector<uint8_t> data; };
        std::unordered_map<uint32_t, CbBlobInfo> cbBlobs;

        for (const auto& [name, vals] : state.floatValues)
        {
            auto it = reflect.vars.find(name);
            if (it == reflect.vars.end()) continue;
            const ReflectedVar& rv = it->second;

            auto& blob = cbBlobs[rv.cbufBindSlot];
            blob.slot  = rv.cbufBindSlot;
            if (blob.data.size() < rv.cbufSize)
                blob.data.resize(rv.cbufSize, 0);

            // float/float4: コピーするバイト数は min(rv.size, 16)
            const uint32_t copyBytes = std::min(rv.size, static_cast<uint32_t>(16));
            if (rv.offset + copyBytes <= rv.cbufSize)
                std::memcpy(blob.data.data() + rv.offset, vals.data(), copyBytes);
        }

        // cbuffer 作成・アップロード
        for (auto& [cbSlot, blobInfo] : cbBlobs)
        {
            if (blobInfo.data.empty() || cbSlot >= static_cast<uint32_t>(kMaxCbSlots)) continue;

            // サイズを 16 倍数に切り上げ
            const UINT aligned = (static_cast<UINT>(blobInfo.data.size()) + 15u) & ~15u;
            blobInfo.data.resize(aligned, 0);

            // 作成
            D3D11_BUFFER_DESC bd = {};
            bd.ByteWidth      = aligned;
            bd.Usage          = D3D11_USAGE_DYNAMIC;
            bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(dev->CreateBuffer(&bd, nullptr, cbBufs[cbSlot].GetAddressOf())))
                continue;

            D3D11_MAPPED_SUBRESOURCE mapped = {};
            if (SUCCEEDED(dc->Map(cbBufs[cbSlot].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                std::memcpy(mapped.pData, blobInfo.data.data(), blobInfo.data.size());
                dc->Unmap(cbBufs[cbSlot].Get(), 0);
            }
        }
    }

    // 定数バッファを設定
    {
        ID3D11Buffer* rawCbs[kMaxCbSlots] = {};
        for (int i = 0; i < kMaxCbSlots; ++i)
            rawCbs[i] = cbBufs[i].Get();
        dc->CSSetConstantBuffers(0, kMaxCbSlots, rawCbs);
    }

    // --- CS セット & Dispatch ---
    dc->CSSetShader(prog->cs.Get(), nullptr, 0);
    dc->Dispatch(gx, gy, gz);

    // --- Unbind ---
    dc->CSSetShader(nullptr, nullptr, 0);
    {
        ID3D11ShaderResourceView*  nullSrv[kMaxSrvSlots] = {};
        ID3D11UnorderedAccessView* nullUav[kMaxUavSlots] = {};
        ID3D11Buffer*              nullCb[kMaxCbSlots]   = {};
        dc->CSSetShaderResources(0, kMaxSrvSlots, nullSrv);
        dc->CSSetUnorderedAccessViews(0, kMaxUavSlots, nullUav, nullptr);
        dc->CSSetConstantBuffers(0, kMaxCbSlots, nullCb);
    }

    return 1;
}

// ---------------------------------------------------------------------------
// シャットダウン
// ---------------------------------------------------------------------------

void ComputeAbiRunner::Shutdown()
{
    int leaked = 0;
    for (uint32_t i = 0; i < slotCount_; ++i)
    {
        BufferSlot& slot = bufSlots_[i];
        if (slot.live)
        {
            ++leaked;
            slot.buf.Reset();
            slot.srv.Reset();
            slot.uav.Reset();
            slot.live = false;
        }
    }
    slotCount_ = 0;
    if (leaked > 0)
    {
        MYE_LOG_WARN("ComputeAbiRunner: シャットダウン時に %d 個の未解放 ABI バッファを回収。",
                     leaked);
    }
    freeList_.clear();
    shaderStates_.clear();
}

} // namespace mye
