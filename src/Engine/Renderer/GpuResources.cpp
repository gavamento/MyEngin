#include "Engine/Renderer/GpuResources.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

#include "nlohmann/json.hpp"

#include "Engine/Core/AssetGuidResolver.h"
#include "Engine/Core/AssetKeyResolver.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/ImportMetaResolver.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GraphicsDevice.h"

#include "stb/stb_image.h"

namespace mye {

using Microsoft::WRL::ComPtr;

namespace {

// names_ (ハッシュ→名前) を名前順の列挙結果に変換する共通ヘルパ
std::vector<AssetEntry> EnumerateNames(const std::unordered_map<uint64_t, std::string>& names)
{
    std::vector<AssetEntry> out;
    out.reserve(names.size());
    for (const auto& [id, name] : names) {
        out.push_back({ AssetID{ id }, name });
    }
    std::sort(out.begin(), out.end(),
              [](const AssetEntry& a, const AssetEntry& b) { return a.name < b.name; });
    return out;
}

// ---- DDS (M24: BCn 圧縮テクスチャ、依存ゼロ) ----
constexpr uint32_t kDdsMagic = 0x20534444; // "DDS "

constexpr uint32_t MakeFourCC(char a, char b, char c, char d)
{
    return static_cast<uint32_t>(static_cast<uint8_t>(a))
        | (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8)
        | (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16)
        | (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

#pragma pack(push, 1)
struct DdsPixelFormat {
    uint32_t size, flags, fourCC, rgbBitCount, rMask, gMask, bMask, aMask;
};
struct DdsHeader {
    uint32_t size, flags, height, width, pitchOrLinearSize, depth, mipMapCount;
    uint32_t reserved1[11];
    DdsPixelFormat ddspf;
    uint32_t caps, caps2, caps3, caps4, reserved2;
};
struct DdsHeaderDx10 {
    uint32_t dxgiFormat, resourceDimension, miscFlag, arraySize, miscFlags2;
};
#pragma pack(pop)

bool HasDdsExt(const std::wstring& path)
{
    return path.size() >= 4
        && (path.compare(path.size() - 4, 4, L".dds") == 0
            || path.compare(path.size() - 4, 4, L".DDS") == 0);
}

// FourCC / DX10 ヘッダから DXGI フォーマットを決める。
// blockBytes > 0 = BCn (4x4 ブロックのバイト数)、blockBytes == 0 = 非圧縮 (bppBytes を使う)
DXGI_FORMAT DdsResolveFormat(const DdsHeader& h, const DdsHeaderDx10* dx10, uint32_t& blockBytes,
                             uint32_t& bppBytes)
{
    blockBytes = 0;
    bppBytes = 0;
    if (dx10) {
        const DXGI_FORMAT f = static_cast<DXGI_FORMAT>(dx10->dxgiFormat);
        switch (f) {
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
            blockBytes = 8;
            return f;
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            blockBytes = 16;
            return f;
        // M38b: 非圧縮 (cubemap / HDR 環境マップ用)
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            bppBytes = 4;
            return f;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            bppBytes = 8;
            return f;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            bppBytes = 16;
            return f;
        default:
            return DXGI_FORMAT_UNKNOWN;
        }
    }
    const uint32_t fc = h.ddspf.fourCC;
    if (fc == MakeFourCC('D', 'X', 'T', '1')) {
        blockBytes = 8;
        return DXGI_FORMAT_BC1_UNORM;
    }
    if (fc == MakeFourCC('D', 'X', 'T', '3')) {
        blockBytes = 16;
        return DXGI_FORMAT_BC2_UNORM;
    }
    if (fc == MakeFourCC('D', 'X', 'T', '5')) {
        blockBytes = 16;
        return DXGI_FORMAT_BC3_UNORM;
    }
    if (fc == MakeFourCC('A', 'T', 'I', '2') || fc == MakeFourCC('B', 'C', '5', 'U')) {
        blockBytes = 16;
        return DXGI_FORMAT_BC5_UNORM;
    }
    // M38b: レガシー非圧縮 RGBA8 (fourCC 無し・32bit RGB マスク)
    if ((h.ddspf.flags & 0x40u) != 0 && h.ddspf.rgbBitCount == 32) { // DDPF_RGB
        bppBytes = 4;
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    return DXGI_FORMAT_UNKNOWN;
}

constexpr uint32_t kDdsCaps2Cubemap = 0x200;         // DDSCAPS2_CUBEMAP
constexpr uint32_t kDx10MiscTextureCube = 0x4;       // D3D11_RESOURCE_MISC_TEXTURECUBE

} // namespace

// ---------------------------------------------------------------- MeshLibrary

AssetID MeshLibrary::Register(std::string_view name, std::span<const MeshVertex> vertices,
                              std::span<const uint32_t> indices)
{
    // 同名の再登録は差し替え (モデルのホットリロード経路。AssetID は不変 = 参照透過)
    const AssetID id{ HashStr(name) };

    Mesh mesh;
    // Init 前 (ヘッドレス = --selftest 等、M48a) は GPU バッファを作らず CPU 側
    // (AABB / positions / indices) だけ登録する — ローダをウィンドウ / D3D 無しで通すため。
    // 実アプリは必ず Init 済みなのでこの分岐には入らない
    if (device_) {
        D3D11_BUFFER_DESC vbd = {};
        vbd.ByteWidth = static_cast<UINT>(vertices.size_bytes());
        vbd.Usage = D3D11_USAGE_IMMUTABLE;
        vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA vinit = { vertices.data(), 0, 0 };

        D3D11_BUFFER_DESC ibd = {};
        ibd.ByteWidth = static_cast<UINT>(indices.size_bytes());
        ibd.Usage = D3D11_USAGE_IMMUTABLE;
        ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA iinit = { indices.data(), 0, 0 };

        ID3D11Device* dev = device_->Device();
        if (FAILED(dev->CreateBuffer(&vbd, &vinit, mesh.vb.GetAddressOf()))
            || FAILED(dev->CreateBuffer(&ibd, &iinit, mesh.ib.GetAddressOf()))) {
            MYE_LOG_ERROR("mesh buffer creation failed: %.*s", static_cast<int>(name.size()),
                          name.data());
            return {};
        }
    }
    mesh.indexCount = static_cast<uint32_t>(indices.size());

    // M41: メッシュコライダー用 CPU コピー (位置 + インデックス)
    // M46a: レイトレのヒット属性用に法線 / UV も同じ頂点順で保持する
    mesh.positions.reserve(vertices.size());
    mesh.normals.reserve(vertices.size());
    mesh.uvs.reserve(vertices.size());
    for (const MeshVertex& mv : vertices) {
        mesh.positions.push_back(mv.position);
        mesh.normals.push_back(mv.normal);
        mesh.uvs.push_back(mv.uv);
    }
    mesh.indices.assign(indices.begin(), indices.end());

    // ローカル AABB を頂点から計算 (M8: Focus/ピッキング/サムネイル)
    if (!vertices.empty()) {
        DirectX::XMFLOAT3 lo = vertices[0].position;
        DirectX::XMFLOAT3 hi = vertices[0].position;
        for (const MeshVertex& v : vertices) {
            lo.x = (v.position.x < lo.x) ? v.position.x : lo.x;
            lo.y = (v.position.y < lo.y) ? v.position.y : lo.y;
            lo.z = (v.position.z < lo.z) ? v.position.z : lo.z;
            hi.x = (v.position.x > hi.x) ? v.position.x : hi.x;
            hi.y = (v.position.y > hi.y) ? v.position.y : hi.y;
            hi.z = (v.position.z > hi.z) ? v.position.z : hi.z;
        }
        mesh.aabbMin = lo;
        mesh.aabbMax = hi;
    }

    meshes_[id.value] = std::move(mesh);
    names_[id.value].assign(name);
    return id;
}

Mesh* MeshLibrary::Get(AssetID id)
{
    auto it = meshes_.find(id.value);
    return (it != meshes_.end()) ? &it->second : nullptr;
}

const std::string* MeshLibrary::NameOf(AssetID id) const
{
    const auto it = names_.find(id.value);
    return (it != names_.end()) ? &it->second : nullptr;
}

std::vector<AssetEntry> MeshLibrary::Enumerate() const
{
    return EnumerateNames(names_);
}

AssetID MeshLibrary::Cube()
{
    if (!cube_.IsNull()) {
        return cube_;
    }
    // 単位キューブ (中心原点、辺長 1)。面ごとに法線/UV 付き 24 頂点。
    // 巻き順は時計回り (DX 既定の front)
    const float h = 0.5f;
    const MeshVertex v[] = {
        // +X
        { { h, -h, -h }, { 1, 0, 0 }, { 0, 1 } }, { { h, h, -h }, { 1, 0, 0 }, { 0, 0 } },
        { { h, h, h }, { 1, 0, 0 }, { 1, 0 } },   { { h, -h, h }, { 1, 0, 0 }, { 1, 1 } },
        // -X
        { { -h, -h, h }, { -1, 0, 0 }, { 0, 1 } }, { { -h, h, h }, { -1, 0, 0 }, { 0, 0 } },
        { { -h, h, -h }, { -1, 0, 0 }, { 1, 0 } }, { { -h, -h, -h }, { -1, 0, 0 }, { 1, 1 } },
        // +Y
        { { -h, h, -h }, { 0, 1, 0 }, { 0, 1 } }, { { -h, h, h }, { 0, 1, 0 }, { 0, 0 } },
        { { h, h, h }, { 0, 1, 0 }, { 1, 0 } },   { { h, h, -h }, { 0, 1, 0 }, { 1, 1 } },
        // -Y
        { { -h, -h, h }, { 0, -1, 0 }, { 0, 1 } }, { { -h, -h, -h }, { 0, -1, 0 }, { 0, 0 } },
        { { h, -h, -h }, { 0, -1, 0 }, { 1, 0 } }, { { h, -h, h }, { 0, -1, 0 }, { 1, 1 } },
        // +Z
        { { h, -h, h }, { 0, 0, 1 }, { 0, 1 } },  { { h, h, h }, { 0, 0, 1 }, { 0, 0 } },
        { { -h, h, h }, { 0, 0, 1 }, { 1, 0 } },  { { -h, -h, h }, { 0, 0, 1 }, { 1, 1 } },
        // -Z
        { { -h, -h, -h }, { 0, 0, -1 }, { 0, 1 } }, { { -h, h, -h }, { 0, 0, -1 }, { 0, 0 } },
        { { h, h, -h }, { 0, 0, -1 }, { 1, 0 } },   { { h, -h, -h }, { 0, 0, -1 }, { 1, 1 } },
    };
    std::vector<uint32_t> indices;
    indices.reserve(36);
    for (uint32_t f = 0; f < 6; ++f) {
        const uint32_t b = f * 4;
        indices.insert(indices.end(), { b, b + 1, b + 2, b, b + 2, b + 3 });
    }
    cube_ = Register("builtin://cube", v, indices);
    return cube_;
}

namespace {

constexpr float kPi = 3.14159265358979323846f;

// 巻き順の規約 (Cube と同じ): 三角形 (v0,v1,v2) は cross(v1-v0, v2-v0) が外向き法線と
// 同じ向きになるよう張る (= DX 既定の front)。以下のヘルパは全てこれに従う。

// UV 球の緯度帯 [phi0, phi1] を生成して verts/idx に追記する (中心 center, 半径 r)。
// phi=0 が +Y 極、phi=pi が -Y 極。stacks=緯度分割、slices=経度分割。
void AppendSphereBand(std::vector<MeshVertex>& verts, std::vector<uint32_t>& idx,
                      const DirectX::XMFLOAT3& center, float r, int stacks, int slices, float phi0,
                      float phi1)
{
    const uint32_t base = static_cast<uint32_t>(verts.size());
    for (int i = 0; i <= stacks; ++i) {
        const float phi = phi0 + (phi1 - phi0) * (static_cast<float>(i) / stacks);
        const float sp = std::sin(phi);
        const float cp = std::cos(phi);
        for (int j = 0; j <= slices; ++j) {
            const float theta = 2.0f * kPi * (static_cast<float>(j) / slices);
            const DirectX::XMFLOAT3 n = { sp * std::cos(theta), cp, sp * std::sin(theta) };
            MeshVertex v;
            v.position = { center.x + r * n.x, center.y + r * n.y, center.z + r * n.z };
            v.normal = n;
            v.uv = { static_cast<float>(j) / slices, phi / kPi };
            verts.push_back(v);
        }
    }
    const uint32_t stride = static_cast<uint32_t>(slices + 1);
    for (int i = 0; i < stacks; ++i) {
        for (int j = 0; j < slices; ++j) {
            const uint32_t a = base + static_cast<uint32_t>(i) * stride + j;
            const uint32_t b = base + static_cast<uint32_t>(i + 1) * stride + j;
            const uint32_t c = a + 1;
            const uint32_t d = b + 1;
            idx.insert(idx.end(), { a, c, b, c, d, b });
        }
    }
}

// 円柱側面 (y=yBottom..yTop, 半径 r) を追記。法線は放射方向。
void AppendCylinderSide(std::vector<MeshVertex>& verts, std::vector<uint32_t>& idx, float r,
                        float yBottom, float yTop, int slices)
{
    const uint32_t base = static_cast<uint32_t>(verts.size());
    for (int j = 0; j <= slices; ++j) {
        const float theta = 2.0f * kPi * (static_cast<float>(j) / slices);
        const float ct = std::cos(theta);
        const float st = std::sin(theta);
        const DirectX::XMFLOAT3 n = { ct, 0.0f, st };
        const float u = static_cast<float>(j) / slices;
        MeshVertex top;
        top.position = { r * ct, yTop, r * st };
        top.normal = n;
        top.uv = { u, 0.0f };
        MeshVertex bot;
        bot.position = { r * ct, yBottom, r * st };
        bot.normal = n;
        bot.uv = { u, 1.0f };
        verts.push_back(top);
        verts.push_back(bot);
    }
    for (int j = 0; j < slices; ++j) {
        const uint32_t a = base + static_cast<uint32_t>(j) * 2;     // top_j
        const uint32_t b = a + 1;                                   // bottom_j
        const uint32_t c = base + static_cast<uint32_t>(j + 1) * 2; // top_{j+1}
        const uint32_t d = c + 1;                                   // bottom_{j+1}
        idx.insert(idx.end(), { a, c, b, b, c, d });
    }
}

// 上下フタ (中心 (0,y,0), 法線 (0,ny,0), ny=+1/-1) を扇状に追記。
void AppendCap(std::vector<MeshVertex>& verts, std::vector<uint32_t>& idx, float r, float y, float ny,
               int slices)
{
    const uint32_t center = static_cast<uint32_t>(verts.size());
    MeshVertex c;
    c.position = { 0.0f, y, 0.0f };
    c.normal = { 0.0f, ny, 0.0f };
    c.uv = { 0.5f, 0.5f };
    verts.push_back(c);
    const uint32_t ring = static_cast<uint32_t>(verts.size());
    for (int j = 0; j <= slices; ++j) {
        const float theta = 2.0f * kPi * (static_cast<float>(j) / slices);
        const float ct = std::cos(theta);
        const float st = std::sin(theta);
        MeshVertex v;
        v.position = { r * ct, y, r * st };
        v.normal = { 0.0f, ny, 0.0f };
        v.uv = { 0.5f + 0.5f * ct, 0.5f + 0.5f * st };
        verts.push_back(v);
    }
    for (int j = 0; j < slices; ++j) {
        const uint32_t r0 = ring + static_cast<uint32_t>(j);
        const uint32_t r1 = r0 + 1;
        if (ny > 0.0f) {
            idx.insert(idx.end(), { center, r1, r0 }); // +Y 外向き
        } else {
            idx.insert(idx.end(), { center, r0, r1 }); // -Y 外向き
        }
    }
}

} // namespace

AssetID MeshLibrary::Sphere()
{
    if (!sphere_.IsNull()) {
        return sphere_;
    }
    std::vector<MeshVertex> verts;
    std::vector<uint32_t> idx;
    AppendSphereBand(verts, idx, { 0, 0, 0 }, 0.5f, 16, 24, 0.0f, kPi);
    sphere_ = Register("builtin://sphere", verts, idx);
    return sphere_;
}

AssetID MeshLibrary::Plane()
{
    if (!plane_.IsNull()) {
        return plane_;
    }
    // XZ 平面 1x1、法線 +Y (地面向き)
    const float h = 0.5f;
    const MeshVertex v[] = {
        { { -h, 0, -h }, { 0, 1, 0 }, { 0, 0 } },
        { { -h, 0, h }, { 0, 1, 0 }, { 0, 1 } },
        { { h, 0, h }, { 0, 1, 0 }, { 1, 1 } },
        { { h, 0, -h }, { 0, 1, 0 }, { 1, 0 } },
    };
    const uint32_t idx[] = { 0, 1, 2, 0, 2, 3 };
    plane_ = Register("builtin://plane", v, idx);
    return plane_;
}

AssetID MeshLibrary::Quad()
{
    if (!quad_.IsNull()) {
        return quad_;
    }
    // XY 平面 1x1、法線 -Z (Unity 同様、+Z を向く既定カメラから正面が見える)
    const float h = 0.5f;
    const MeshVertex v[] = {
        { { -h, -h, 0 }, { 0, 0, -1 }, { 0, 1 } },
        { { -h, h, 0 }, { 0, 0, -1 }, { 0, 0 } },
        { { h, h, 0 }, { 0, 0, -1 }, { 1, 0 } },
        { { h, -h, 0 }, { 0, 0, -1 }, { 1, 1 } },
    };
    const uint32_t idx[] = { 0, 1, 2, 0, 2, 3 };
    quad_ = Register("builtin://quad", v, idx);
    return quad_;
}

AssetID MeshLibrary::Cylinder()
{
    if (!cylinder_.IsNull()) {
        return cylinder_;
    }
    std::vector<MeshVertex> verts;
    std::vector<uint32_t> idx;
    const float r = 0.5f;
    const int slices = 24;
    AppendCylinderSide(verts, idx, r, -0.5f, 0.5f, slices);
    AppendCap(verts, idx, r, 0.5f, 1.0f, slices);   // 上フタ
    AppendCap(verts, idx, r, -0.5f, -1.0f, slices); // 下フタ
    cylinder_ = Register("builtin://cylinder", verts, idx);
    return cylinder_;
}

AssetID MeshLibrary::Capsule()
{
    if (!capsule_.IsNull()) {
        return capsule_;
    }
    // 円柱 (y=-0.5..0.5) + 上下半球 (中心 y=±0.5, 半径 0.5) → 全高 2
    std::vector<MeshVertex> verts;
    std::vector<uint32_t> idx;
    const float r = 0.5f;
    const int slices = 24;
    AppendCylinderSide(verts, idx, r, -0.5f, 0.5f, slices);
    AppendSphereBand(verts, idx, { 0, 0.5f, 0 }, r, 8, slices, 0.0f, kPi * 0.5f);     // 上半球
    AppendSphereBand(verts, idx, { 0, -0.5f, 0 }, r, 8, slices, kPi * 0.5f, kPi);     // 下半球
    capsule_ = Register("builtin://capsule", verts, idx);
    return capsule_;
}

// ---------------------------------------------------------------- TextureLibrary

bool TextureLibrary::CreateFromPixels(Texture& out, const uint8_t* rgba, int w, int h, bool srgb,
                                      bool mips)
{
    if (!device_) {
        return false; // Init 前 = ヘッドレス (M48a)。GPU 生成の隘路はここと LoadDdsInto の 2 本
    }
    // 既定はフルミップチェーン + GenerateMips (.meta の generateMips=off で mip0 のみ、M39b)
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = static_cast<UINT>(w);
    td.Height = static_cast<UINT>(h);
    td.MipLevels = mips ? 0 : 1; // 0 = full chain
    td.ArraySize = 1;
    // M38a: アルベド系は _SRGB (サンプル時 HW デコード = リニアパイプライン)
    td.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc = { 1, 0 };
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (mips) {
        td.BindFlags |= D3D11_BIND_RENDER_TARGET; // GenerateMips の要件
        td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    }

    ID3D11Device* dev = device_->Device();
    if (FAILED(dev->CreateTexture2D(&td, nullptr, out.tex.ReleaseAndGetAddressOf()))) {
        return false;
    }
    device_->Context()->UpdateSubresource(out.tex.Get(), 0, nullptr, rgba,
                                          static_cast<UINT>(w * 4), 0);
    if (FAILED(dev->CreateShaderResourceView(out.tex.Get(), nullptr,
                                             out.srv.ReleaseAndGetAddressOf()))) {
        return false;
    }
    if (mips) {
        device_->Context()->GenerateMips(out.srv.Get());
    }
    out.width = w;
    out.height = h;
    out.srgb = srgb;
    return true;
}

AssetID TextureLibrary::IdForFile(const std::wstring& path)
{
    // M30c: 移動/リネーム済みアセットは .meta の GUID がキーになる (未移動は path-hash と同値)
    return AssetID{ assetkey::Resolve(NormalizePathKey(path)) };
}

AssetID TextureLibrary::LoadFile(const std::wstring& path, bool srgb)
{
    const AssetID id = IdForFile(path);
    if (textures_.contains(id.value)) {
        return id; // 先勝ち (フラグ違いは無視 — per-asset 指定は .meta の srgb on/off で)
    }
    // M39b: .meta のインポート設定が呼び出し側ヒントを上書きする (auto = ヒントのまま)
    importmeta::TextureImportSettings imp;
    importmeta::Resolve(path, imp);
    const bool effSrgb = (imp.srgb == 1) ? true : (imp.srgb == 2) ? false : srgb;
    const bool mips = imp.generateMips != 0;

    const std::string utf8 = WideToUtf8(path);
    Texture t;
    if (HasDdsExt(path)) {
        // M24: BCn/DDS は decode 不要 (GPU が直接サンプルする)。ヘッダを読んで直接テクスチャ化
        // (mips は DDS に焼成済みのため generateMips は非適用)
        if (!LoadDdsInto(t, path, effSrgb)) {
            return {};
        }
        textures_.emplace(id.value, std::move(t));
        names_[id.value] = utf8;
        return id;
    }
    // M51j: DDS 一括クック済みの配布ビルド対応。**元画像が存在しないときだけ**、同名の
    // .dds を同じ AssetID で読む (開発環境ではソースが常にあるので挙動は 1 ビットも
    // 変わらない)。srgb / mips は元パスの .meta で解決済み (effSrgb — .meta は配布へ残す)
    {
        std::error_code fbEc;
        if (!std::filesystem::exists(path, fbEc)) {
            const std::wstring sibling =
                std::filesystem::path(path).replace_extension(L".dds").wstring();
            if (std::filesystem::exists(sibling, fbEc)) {
                if (!LoadDdsInto(t, sibling, effSrgb)) {
                    return {};
                }
                MYE_LOG_INFO("texture served from cooked dds: %s", utf8.c_str());
                textures_.emplace(id.value, std::move(t));
                names_[id.value] = utf8;
                return id;
            }
        }
    }
    int w = 0, h = 0, comp = 0;
    stbi_uc* pixels = stbi_load(utf8.c_str(), &w, &h, &comp, 4);
    if (!pixels) {
        MYE_LOG_ERROR("texture load failed: %s (%s)", utf8.c_str(), stbi_failure_reason());
        return {};
    }
    const bool ok = CreateFromPixels(t, pixels, w, h, effSrgb, mips);
    stbi_image_free(pixels);
    if (!ok) {
        MYE_LOG_ERROR("texture creation failed: %s", utf8.c_str());
        return {};
    }
    textures_.emplace(id.value, std::move(t));
    names_[id.value] = utf8;
    return id;
}

// UNORM → 対応する _SRGB フォーマット (M38a)。sRGB 変種の無いもの (BC5=ノーマル等) はそのまま
static DXGI_FORMAT ToSrgbFormat(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_BC1_UNORM: return DXGI_FORMAT_BC1_UNORM_SRGB;
    case DXGI_FORMAT_BC2_UNORM: return DXGI_FORMAT_BC2_UNORM_SRGB;
    case DXGI_FORMAT_BC3_UNORM: return DXGI_FORMAT_BC3_UNORM_SRGB;
    case DXGI_FORMAT_BC7_UNORM: return DXGI_FORMAT_BC7_UNORM_SRGB;
    case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    default: return f;
    }
}

// DDS (BC1/BC2/BC3/BC5/BC7) をヘッダ解析して直接 GPU テクスチャ化する。DirectXTex 不要 —
// BCn は D3D11 がネイティブにサンプルするため、圧縮ブロックをそのまま subresource に渡すだけ。
bool TextureLibrary::LoadDdsInto(Texture& out, const std::wstring& path, bool srgb)
{
    if (!device_) {
        return false; // Init 前 = ヘッドレス (M48a)。CreateFromPixels と並ぶ GPU 生成の隘路
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        MYE_LOG_ERROR("dds open failed: %s", WideToUtf8(path).c_str());
        return false;
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
    if (bytes.size() < 4 + sizeof(DdsHeader)) {
        MYE_LOG_ERROR("dds too small: %s", WideToUtf8(path).c_str());
        return false;
    }
    uint32_t magic = 0;
    std::memcpy(&magic, bytes.data(), 4);
    if (magic != kDdsMagic) {
        MYE_LOG_ERROR("not a DDS file: %s", WideToUtf8(path).c_str());
        return false;
    }
    DdsHeader hdr;
    std::memcpy(&hdr, bytes.data() + 4, sizeof(DdsHeader));
    size_t offset = 4 + sizeof(DdsHeader);

    DdsHeaderDx10 dx10s;
    const DdsHeaderDx10* dx10 = nullptr;
    if (hdr.ddspf.fourCC == MakeFourCC('D', 'X', '1', '0')) {
        if (bytes.size() < offset + sizeof(DdsHeaderDx10)) {
            MYE_LOG_ERROR("dds: DX10 header truncated: %s", WideToUtf8(path).c_str());
            return false;
        }
        std::memcpy(&dx10s, bytes.data() + offset, sizeof(DdsHeaderDx10));
        dx10 = &dx10s;
        offset += sizeof(DdsHeaderDx10);
    }

    uint32_t blockBytes = 0;
    uint32_t bppBytes = 0;
    DXGI_FORMAT fmt = DdsResolveFormat(hdr, dx10, blockBytes, bppBytes);
    if (srgb) {
        fmt = ToSrgbFormat(fmt); // M38a: アルベド DDS は _SRGB 変種でサンプル時デコード
    }
    if (fmt == DXGI_FORMAT_UNKNOWN) {
        MYE_LOG_ERROR("dds: unsupported format (fourCC=0x%08X): %s", hdr.ddspf.fourCC,
                      WideToUtf8(path).c_str());
        return false;
    }

    // M38b: cubemap (caps2 / DX10 miscFlag)。データは face-major (面 0 の全 mip → 面 1 ...) —
    // D3D11 の Texture2DArray subresource 順と同じなのでそのまま並べる
    const bool isCube = ((hdr.caps2 & kDdsCaps2Cubemap) != 0)
        || (dx10 != nullptr && (dx10->miscFlag & kDx10MiscTextureCube) != 0);
    const uint32_t faces = isCube ? 6u : 1u;

    const uint32_t mipCount = (hdr.mipMapCount > 0) ? hdr.mipMapCount : 1;
    std::vector<D3D11_SUBRESOURCE_DATA> subs(static_cast<size_t>(faces) * mipCount);
    size_t cursor = offset;
    for (uint32_t f2 = 0; f2 < faces; ++f2) {
        uint32_t w = hdr.width;
        uint32_t h = hdr.height;
        for (uint32_t m = 0; m < mipCount; ++m) {
            uint32_t rowPitch = 0;
            uint32_t rows = 0;
            if (blockBytes > 0) { // BCn
                rowPitch = ((w + 3) / 4) * blockBytes;
                rows = (h + 3) / 4;
            } else { // 非圧縮
                rowPitch = w * bppBytes;
                rows = h;
            }
            const size_t mipSize = static_cast<size_t>(rowPitch) * rows;
            if (cursor + mipSize > bytes.size()) {
                MYE_LOG_ERROR("dds: data truncated at face %u mip %u: %s", f2, m,
                              WideToUtf8(path).c_str());
                return false;
            }
            D3D11_SUBRESOURCE_DATA& sd = subs[static_cast<size_t>(f2) * mipCount + m];
            sd.pSysMem = bytes.data() + cursor;
            sd.SysMemPitch = rowPitch;
            sd.SysMemSlicePitch = static_cast<UINT>(mipSize);
            cursor += mipSize;
            w = (w > 1) ? w / 2 : 1;
            h = (h > 1) ? h / 2 : 1;
        }
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = hdr.width;
    td.Height = hdr.height;
    td.MipLevels = mipCount;
    td.ArraySize = faces;
    td.Format = fmt;
    td.SampleDesc = { 1, 0 };
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = isCube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0u;

    ID3D11Device* dev = device_->Device();
    if (FAILED(dev->CreateTexture2D(&td, subs.data(), out.tex.ReleaseAndGetAddressOf()))) {
        MYE_LOG_ERROR("dds CreateTexture2D failed: %s", WideToUtf8(path).c_str());
        return false;
    }
    if (FAILED(dev->CreateShaderResourceView(out.tex.Get(), nullptr,
                                             out.srv.ReleaseAndGetAddressOf()))) {
        return false;
    }
    out.width = static_cast<int>(hdr.width);
    out.height = static_cast<int>(hdr.height);
    out.srgb = srgb;
    return true;
}

TextureLibrary::~TextureLibrary()
{
    if (workerStarted_) {
        {
            std::lock_guard<std::mutex> lk(asyncMutex_);
            workerStop_ = true;
        }
        asyncCv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }
}

void TextureLibrary::EnsureWorker()
{
    if (workerStarted_) {
        return;
    }
    workerStarted_ = true;
    worker_ = std::thread([this] { AsyncWorker(); });
}

// ワーカースレッド: CPU デコードのみ (GPU 作成には触れない)。
void TextureLibrary::AsyncWorker()
{
    for (;;) {
        DecodeJob job;
        {
            std::unique_lock<std::mutex> lk(asyncMutex_);
            asyncCv_.wait(lk, [this] { return workerStop_ || !jobQueue_.empty(); });
            if (workerStop_ && jobQueue_.empty()) {
                return;
            }
            job = std::move(jobQueue_.front());
            jobQueue_.pop_front();
        }
        DecodeResult r;
        r.id = job.id;
        int w = 0, h = 0, comp = 0;
        stbi_uc* pixels = stbi_load(job.utf8Path.c_str(), &w, &h, &comp, 4);
        if (pixels) {
            r.pixels.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
            r.w = w;
            r.h = h;
            r.ok = true;
            stbi_image_free(pixels);
        }
        {
            std::lock_guard<std::mutex> lk(asyncMutex_);
            doneQueue_.push_back(std::move(r));
        }
    }
}

AssetID TextureLibrary::RequestLoadFileAsync(const std::wstring& path)
{
    // DDS は既に圧縮済みで decode 不要 (ワーカーの stbi は非対応) → 同期ロードで即座に公開
    if (HasDdsExt(path)) {
        return LoadFile(path);
    }
    const AssetID id = IdForFile(path);
    if (pending_.count(id.value)) {
        return id; // デコード中
    }
    if (textures_.contains(id.value)) {
        return id; // 既にロード済み (実体 or 公開済み)
    }
    // プレースホルダ (白を共有) を cache に入れておき、ロード中も Get() が有効な SRV を返せるように
    if (Texture* w = Get(White())) {
        Texture ph;
        ph.tex = w->tex; // ComPtr 共有 (参照カウント)
        ph.srv = w->srv;
        ph.width = 1;
        ph.height = 1;
        textures_.emplace(id.value, std::move(ph));
    }
    pending_.insert(id.value);
    names_[id.value] = WideToUtf8(path);
    EnsureWorker();
    {
        std::lock_guard<std::mutex> lk(asyncMutex_);
        jobQueue_.push_back({ id.value, WideToUtf8(path) });
    }
    asyncCv_.notify_one();
    return id;
}

void TextureLibrary::PollAsyncLoads()
{
    std::vector<DecodeResult> done;
    {
        std::lock_guard<std::mutex> lk(asyncMutex_);
        if (doneQueue_.empty()) {
            return;
        }
        done.swap(doneQueue_);
    }
    for (DecodeResult& r : done) {
        pending_.erase(r.id);
        if (!r.ok) {
            MYE_LOG_WARN("async texture decode failed (id=%016llx)",
                         static_cast<unsigned long long>(r.id));
            continue; // プレースホルダのまま残す (再投入せず hammering を防ぐ)
        }
        // M39b: 非同期経路 (サムネイル等) も .meta の srgb on を尊重する — 先勝ちキャッシュに
        // 非 sRGB で入ると 3D 側が色褪せるため。auto は従来どおり UNORM (ImGui 直表示向け)
        importmeta::TextureImportSettings imp;
        auto nameIt = names_.find(r.id);
        if (nameIt != names_.end()) {
            importmeta::Resolve(Utf8ToWide(nameIt->second), imp);
        }
        Texture t;
        if (CreateFromPixels(t, r.pixels.data(), r.w, r.h, imp.srgb == 1,
                             imp.generateMips != 0)) {
            textures_[r.id] = std::move(t); // プレースホルダを実体に差し替え
        }
    }
}

// M52c: 決定的スクショの前提づくり。pending_ はメインスレッド専用なので
// ここ (メインスレッド) から見れば「まだ公開されていないテクスチャの集合」そのもの。
// ワーカーは成功・失敗どちらでも必ず結果を積み、PollAsyncLoads が両方で pending_ から
// 消すので、このループは必ず終わる (それでも保険のタイムアウトを持つ)
void TextureLibrary::WaitForAsyncLoads(int timeoutMs)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        PollAsyncLoads();
        if (pending_.empty()) {
            return;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            MYE_LOG_WARN("WaitForAsyncLoads: timed out with %zu texture(s) still pending",
                         pending_.size());
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

AssetID TextureLibrary::CreateFromEncoded(std::string_view name, const void* bytes, size_t size,
                                          bool srgb)
{
    // 再呼び出しは差し替え (モデルリロード時に埋め込みテクスチャを更新するため)
    const AssetID id{ HashStr(name) };
    int w = 0, h = 0, comp = 0;
    stbi_uc* pixels = stbi_load_from_memory(static_cast<const stbi_uc*>(bytes),
                                            static_cast<int>(size), &w, &h, &comp, 4);
    if (!pixels) {
        MYE_LOG_ERROR("embedded texture decode failed: %.*s", static_cast<int>(name.size()), name.data());
        return {};
    }
    Texture t;
    const bool ok = CreateFromPixels(t, pixels, w, h, srgb);
    stbi_image_free(pixels);
    if (!ok) {
        return {};
    }
    textures_[id.value] = std::move(t);
    names_[id.value].assign(name);
    return id;
}

AssetID TextureLibrary::CreateSolid(std::string_view name, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    const AssetID id{ HashStr(name) };
    if (textures_.contains(id.value)) {
        return id;
    }
    const uint8_t pixel[4] = { r, g, b, a };
    Texture t;
    if (!CreateFromPixels(t, pixel, 1, 1)) {
        return {};
    }
    textures_.emplace(id.value, std::move(t));
    names_[id.value].assign(name);
    return id;
}

AssetID TextureLibrary::CreateFromRgba8(std::string_view name, const uint8_t* rgba, int w, int h,
                                        bool srgb, bool mips)
{
    if (rgba == nullptr || w <= 0 || h <= 0) {
        return {};
    }
    const AssetID id{ HashStr(name) };
    if (textures_.contains(id.value)) {
        return id; // 先勝ち (同名の再生成は差し替えない — 呼び手が名前に版を混ぜる)
    }
    Texture t;
    if (!CreateFromPixels(t, rgba, w, h, srgb, mips)) {
        return {}; // device_ 未設定 (ヘッドレス) もここに落ちる
    }
    textures_.emplace(id.value, std::move(t));
    names_[id.value].assign(name);
    return id;
}

std::vector<AssetEntry> TextureLibrary::Enumerate() const
{
    return EnumerateNames(names_);
}

Texture* TextureLibrary::Get(AssetID id)
{
    auto it = textures_.find(id.value);
    return (it != textures_.end()) ? &it->second : nullptr;
}

AssetID TextureLibrary::White()
{
    if (white_.IsNull()) {
        white_ = CreateSolid("builtin://white", 255, 255, 255, 255);
    }
    return white_;
}

bool TextureLibrary::ReplaceFromFile(AssetID id, const std::wstring& path)
{
    auto it = textures_.find(id.value);
    if (it == textures_.end()) {
        return false;
    }
    // ホットリロードは sRGB を維持 (M38a)。ただし .meta の on/off 指定があれば従う
    // (Import Settings 適用の即時反映経路もここ、M39b)
    importmeta::TextureImportSettings imp;
    importmeta::Resolve(path, imp);
    const bool srgb = (imp.srgb == 1) ? true : (imp.srgb == 2) ? false : it->second.srgb;
    const bool mips = imp.generateMips != 0;
    Texture fresh;
    if (HasDdsExt(path)) {
        if (!LoadDdsInto(fresh, path, srgb)) {
            return false;
        }
        it->second = std::move(fresh);
        return true;
    }
    const std::string utf8 = WideToUtf8(path);
    int w = 0, h = 0, comp = 0;
    stbi_uc* pixels = stbi_load(utf8.c_str(), &w, &h, &comp, 4);
    if (!pixels) {
        MYE_LOG_ERROR("texture reload failed: %s (%s)", utf8.c_str(), stbi_failure_reason());
        return false;
    }
    const bool ok = CreateFromPixels(fresh, pixels, w, h, srgb, mips);
    stbi_image_free(pixels);
    if (!ok) {
        return false;
    }
    it->second = std::move(fresh); // AssetID は不変のまま実体を差し替え (spec 8.2)
    return true;
}

// ---------------------------------------------------------------- MaterialLibrary

AssetID MaterialLibrary::Register(std::string_view name, const Material& mat)
{
    const AssetID id{ HashStr(name) };
    materials_[id.value] = mat; // 同名は上書き (リロード用)
    names_[id.value].assign(name);
    return id;
}

AssetID MaterialLibrary::RegisterAnonymous(uint64_t key, const Material& mat)
{
    const AssetID id{ key };
    materials_[id.value] = mat;
    // names_ には**入れない** — Enumerate = 参照ピッカーに出さないための唯一の仕掛け (M53)
    return id;
}

Material* MaterialLibrary::Get(AssetID id)
{
    auto it = materials_.find(id.value);
    return (it != materials_.end()) ? &it->second : nullptr;
}

std::vector<AssetEntry> MaterialLibrary::Enumerate() const
{
    return EnumerateNames(names_);
}

AssetID MaterialLibrary::Default(ShaderManager& shaders, TextureLibrary& textures)
{
    if (!default_.IsNull()) {
        return default_;
    }
    Material m;
    m.shader = AssetID{ HashStr("forward_lit") }; // ShaderManager::Load 済み前提
    m.texture = textures.White();
    m.baseColor = { 0.7f, 0.7f, 0.7f, 1.0f };
    (void)shaders;
    default_ = Register("builtin://default_material", m);
    return default_;
}

AssetID MaterialLibrary::HashForPath(const std::wstring& path)
{
    // M30c: 移動/リネーム済みアセットは .meta の GUID がキーになる (未移動は path-hash と同値)
    return AssetID{ assetkey::Resolve(NormalizePathKey(path)) };
}

// JSON オブジェクト → Material。ファイル読み (LoadFromFile) と Inspector のプレビュー
// (MaterialFromJsonText) の**唯一の本体**。フィールドを足すときはここだけ触ること (M53)
static void ParseMaterialJson(const nlohmann::json& root, TextureLibrary& textures,
                              const std::wstring& assetsRoot, Material& m)
{
    m.shader = AssetID{ HashStr(root.value("shader", std::string("forward_lit"))) };
    if (root.contains("baseColor") && root["baseColor"].is_array()) {
        const nlohmann::json& c = root["baseColor"];
        float* dst = &m.baseColor.x;
        for (size_t i = 0; i < c.size() && i < 4; ++i) {
            dst[i] = c[i].get<float>();
        }
    }
    m.metallic = root.value("metallic", 0.0f);
    m.roughness = root.value("roughness", 0.5f);
    m.transparent = root.value("transparent", false) ? 1 : 0;
    // M46i: 自己発光。欠損 = 0 = 発光なしなので、既存の .mat.json は挙動不変
    m.emissiveIntensity = root.value("emissive", 0.0f);

    // texture/normalMap のサブ参照 (M39a で GUID 化):
    //   数値 = GUID (assetguid::ResolvePath で現在パスへ解決 — リネーム/移動に追従)
    //   文字列 = 従来の assetsRoot 相対パス (後方互換読み)。空文字/0 は「なし」
    auto resolveTex = [&](const char* key, bool srgb) -> AssetID {
        if (!root.contains(key)) {
            return {};
        }
        const nlohmann::json& node = root[key];
        if (node.is_number_unsigned() || node.is_number_integer()) {
            const uint64_t guid = node.get<uint64_t>();
            if (guid == 0) {
                return {};
            }
            const std::wstring full = assetguid::ResolvePath(guid);
            if (full.empty()) {
                MYE_LOG_WARN("material texture guid %llu unresolved",
                             static_cast<unsigned long long>(guid));
                return {};
            }
            return textures.LoadFile(full, srgb);
        }
        if (node.is_string()) {
            const std::string rel = node.get<std::string>();
            if (rel.empty()) {
                return {};
            }
            return textures.LoadFile(assetsRoot + L"\\" + Utf8ToWide(rel), srgb);
        }
        return {};
    };
    // M38a: アルベドは sRGB デコード、ノーマルマップはリニアのまま
    const AssetID baseTex = resolveTex("texture", true);
    m.texture = baseTex.IsNull() ? textures.White() : baseTex;
    m.normalTex = resolveTex("normalMap", false);
}

bool MaterialLibrary::MaterialFromJsonText(std::string_view text, TextureLibrary& textures,
                                           const std::wstring& assetsRoot, Material& out)
{
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception& ex) {
        MYE_LOG_WARN("material json parse failed: %s", ex.what());
        return false;
    }
    if (!root.is_object()) {
        return false;
    }
    out = Material{};
    ParseMaterialJson(root, textures, assetsRoot, out);
    return true;
}

AssetID MaterialLibrary::LoadFromFile(const std::wstring& path, TextureLibrary& textures,
                                      const std::wstring& assetsRoot)
{
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) {
        return {};
    }
    nlohmann::json root;
    try {
        f >> root;
    } catch (const nlohmann::json::exception& ex) {
        MYE_LOG_WARN("material parse failed: %s (%s)", WideToUtf8(path).c_str(), ex.what());
        return {};
    }

    Material m;
    ParseMaterialJson(root, textures, assetsRoot, m);

    const AssetID id = HashForPath(path);
    materials_[id.value] = m;

    std::string name = root.value("name", std::string());
    if (name.empty()) {
        name = WideToUtf8(std::filesystem::path(path).stem().wstring()); // "X.mat.json" -> "X.mat"
        const std::string suf = ".mat";
        if (name.size() > suf.size()
            && name.compare(name.size() - suf.size(), suf.size(), suf) == 0) {
            name.resize(name.size() - suf.size());
        }
    }
    names_[id.value] = name;
    return id;
}

} // namespace mye
