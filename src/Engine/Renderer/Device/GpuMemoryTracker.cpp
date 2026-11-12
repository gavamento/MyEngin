#include "Engine/Renderer/Device/GpuMemoryTracker.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <dxgi1_4.h>
#include <wrl/client.h>

#include "Engine/Renderer/Device/GraphicsDevice.h"

namespace mye::gpumem {

namespace {

// このトラッカーをリソースへぶら下げるときのキー (プロセス内で一意ならよい)
// {6B0C1F2E-4E0B-4B8B-9E3A-2F5D7C1A9B41}
const GUID kTrackerGuid = { 0x6b0c1f2e, 0x4e0b, 0x4b8b, { 0x9e, 0x3a, 0x2f, 0x5d, 0x7c, 0x1a, 0x9b, 0x41 } };

class Tracker;

// 生存中のトラッカーの一覧 (双方向リスト)。リソースの破棄はどのスレッドからでも起こりうるので鍵で守る
std::mutex g_mutex;
Tracker* g_head = nullptr;

// リソース 1 個にぶら下がる記録。D3D がリソースを壊すとき最後の Release が来て、自分を一覧から外す
class Tracker final : public IUnknown {
public:
    Tracker(Kind kind, uint64_t bytes, const char* name)
        : kind_(kind)
        , bytes_(bytes)
    {
        SetName(name);
        const std::lock_guard<std::mutex> lock(g_mutex);
        next_ = g_head;
        if (g_head) {
            g_head->previous_ = this;
        }
        g_head = this;
    }

    // ---- IUnknown ----
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (!object) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown)) {
            *object = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return refs_.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    // 以下は g_mutex を持った状態で呼ぶ
    void SetName(const char* name) { std::snprintf(name_, sizeof(name_), "%s", name ? name : ""); }
    Kind KindOf() const { return kind_; }
    uint64_t Bytes() const { return bytes_; }
    const char* Name() const { return name_; }
    Tracker* Next() const { return next_; }

private:
    ~Tracker()
    {
        const std::lock_guard<std::mutex> lock(g_mutex);
        if (previous_) {
            previous_->next_ = next_;
        } else {
            g_head = next_;
        }
        if (next_) {
            next_->previous_ = previous_;
        }
    }

    std::atomic<ULONG> refs_{ 1 };
    Kind kind_;
    uint64_t bytes_;
    char name_[kNameLength] = {};
    Tracker* previous_ = nullptr;
    Tracker* next_ = nullptr;
};

void Attach(ID3D11DeviceChild* resource, Kind kind, uint64_t bytes, const char* name)
{
    // 同じリソースを 2 度 Track したら、前の記録は SetPrivateDataInterface の差し替えで Release される
    Tracker* tracker = new Tracker(kind, bytes, name);
    resource->SetPrivateDataInterface(kTrackerGuid, tracker);
    tracker->Release(); // 以後の参照はリソースが持つ 1 本だけ
}

struct FormatInfo {
    uint32_t bitsPerPixel;
    bool blockCompressed;
};

// 実際に使っているフォーマットだけを表にする。載っていないものは 32 bpp として見積もる
FormatInfo InfoOf(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return { 128, false };
    case DXGI_FORMAT_R32G32B32_FLOAT:
        return { 96, false };
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return { 64, false };
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R8G8_UNORM:
        return { 16, false };
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_A8_UNORM:
        return { 8, false };
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
        return { 4, true }; // 8 バイト / 4x4 ブロック
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return { 8, true }; // 16 バイト / 4x4 ブロック
    default:
        return { 32, false }; // RGBA8 / BGRA8 / R32 / R16G16 / D24S8 / R11G11B10 / R10G10B10A2 など
    }
}

} // namespace

uint64_t TextureBytes(uint32_t width, uint32_t height, uint32_t depth, uint32_t arraySize, uint32_t mipLevels,
                      uint32_t bitsPerPixel, bool blockCompressed)
{
    uint64_t total = 0;
    uint32_t w = (std::max)(width, 1u);
    uint32_t h = (std::max)(height, 1u);
    uint32_t d = (std::max)(depth, 1u);
    for (uint32_t mip = 0; mip < (std::max)(mipLevels, 1u); ++mip) {
        uint64_t pixels = 0;
        if (blockCompressed) {
            // 4x4 ブロックに切り上げる (1x1 の段でも 1 ブロックぶん要る)
            pixels = static_cast<uint64_t>((w + 3) / 4) * ((h + 3) / 4) * 16u;
        } else {
            pixels = static_cast<uint64_t>(w) * h;
        }
        total += pixels * d * bitsPerPixel / 8u;
        w = (std::max)(w / 2, 1u);
        h = (std::max)(h / 2, 1u);
        d = (std::max)(d / 2, 1u);
    }
    return total * (std::max)(arraySize, 1u);
}

void TrackTexture2D(ID3D11Texture2D* texture, Kind kind, const char* name)
{
    if (!texture) {
        return;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    const FormatInfo info = InfoOf(desc.Format);
    // MipLevels は生成後の実数が返る (0 指定 = 全段でも、GetDesc は確定した段数を返す)
    const uint64_t bytes = TextureBytes(desc.Width, desc.Height, 1, desc.ArraySize, desc.MipLevels,
                                        info.bitsPerPixel, info.blockCompressed)
                           * (std::max)(desc.SampleDesc.Count, 1u);
    Attach(texture, kind, bytes, name);
}

void TrackTexture3D(ID3D11Texture3D* texture, Kind kind, const char* name)
{
    if (!texture) {
        return;
    }
    D3D11_TEXTURE3D_DESC desc = {};
    texture->GetDesc(&desc);
    const FormatInfo info = InfoOf(desc.Format);
    Attach(texture, kind,
           TextureBytes(desc.Width, desc.Height, desc.Depth, 1, desc.MipLevels, info.bitsPerPixel,
                        info.blockCompressed),
           name);
}

void TrackBuffer(ID3D11Buffer* buffer, Kind kind, const char* name)
{
    if (!buffer) {
        return;
    }
    D3D11_BUFFER_DESC desc = {};
    buffer->GetDesc(&desc);
    Attach(buffer, kind, desc.ByteWidth, name);
}

void Rename(ID3D11DeviceChild* resource, const char* name)
{
    if (!resource) {
        return;
    }
    IUnknown* attached = nullptr;
    UINT size = sizeof(attached);
    // GetPrivateData は IUnknown を AddRef して返す
    if (FAILED(resource->GetPrivateData(kTrackerGuid, &size, &attached)) || !attached) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(g_mutex);
        static_cast<Tracker*>(attached)->SetName(name);
    }
    attached->Release();
}

Totals GetTotals()
{
    Totals totals;
    const std::lock_guard<std::mutex> lock(g_mutex);
    for (const Tracker* tracker = g_head; tracker; tracker = tracker->Next()) {
        const int kind = static_cast<int>(tracker->KindOf());
        totals.bytes[kind] += tracker->Bytes();
        ++totals.count[kind];
    }
    return totals;
}

int CopyLargest(Entry* out, int maxEntries)
{
    if (!out || maxEntries <= 0) {
        return 0;
    }
    int count = 0;
    const std::lock_guard<std::mutex> lock(g_mutex);
    // out を「大きい順に並んだ上位 maxEntries 件」に保ちながら 1 周する (一覧全体の写しを作らない)
    for (const Tracker* tracker = g_head; tracker; tracker = tracker->Next()) {
        const uint64_t bytes = tracker->Bytes();
        if (count == maxEntries && bytes <= out[count - 1].bytes) {
            continue;
        }
        int at = (count < maxEntries) ? count++ : count - 1;
        while (at > 0 && out[at - 1].bytes < bytes) {
            out[at] = out[at - 1];
            --at;
        }
        out[at].kind = tracker->KindOf();
        out[at].bytes = bytes;
        std::snprintf(out[at].name, sizeof(out[at].name), "%s", tracker->Name());
    }
    return count;
}

VideoMemory QueryVideoMemory(const GraphicsDevice& device)
{
    VideoMemory result;
    ID3D11Device* d3d = device.Device();
    if (!d3d) {
        return result;
    }
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
    if (FAILED(d3d->QueryInterface(IID_PPV_ARGS(dxgiDevice.GetAddressOf())))
        || FAILED(dxgiDevice->GetAdapter(adapter.GetAddressOf())) || FAILED(adapter.As(&adapter3))) {
        return result;
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
    if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
        return result;
    }
    result.valid = true;
    result.usageBytes = info.CurrentUsage;
    result.budgetBytes = info.Budget;
    return result;
}

} // namespace mye::gpumem
