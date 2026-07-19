#include "Engine/Renderer/GpuResources.h"

#include <vector>

#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GraphicsDevice.h"

#include "stb/stb_image.h"

namespace mye {

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------- MeshLibrary

AssetID MeshLibrary::Register(std::string_view name, std::span<const MeshVertex> vertices,
                              std::span<const uint32_t> indices)
{
    // 同名の再登録は差し替え (モデルのホットリロード経路。AssetID は不変 = 参照透過)
    const AssetID id{ HashStr(name) };

    Mesh mesh;
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
        MYE_LOG_ERROR("mesh buffer creation failed: %.*s", static_cast<int>(name.size()), name.data());
        return {};
    }
    mesh.indexCount = static_cast<uint32_t>(indices.size());
    meshes_[id.value] = std::move(mesh);
    return id;
}

Mesh* MeshLibrary::Get(AssetID id)
{
    auto it = meshes_.find(id.value);
    return (it != meshes_.end()) ? &it->second : nullptr;
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

// ---------------------------------------------------------------- TextureLibrary

bool TextureLibrary::CreateFromPixels(Texture& out, const uint8_t* rgba, int w, int h)
{
    // フルミップチェーン + GenerateMips
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = static_cast<UINT>(w);
    td.Height = static_cast<UINT>(h);
    td.MipLevels = 0; // full chain
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc = { 1, 0 };
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

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
    device_->Context()->GenerateMips(out.srv.Get());
    out.width = w;
    out.height = h;
    return true;
}

AssetID TextureLibrary::IdForFile(const std::wstring& path)
{
    return AssetID{ HashStr(WideToUtf8(NormalizePathKey(path))) };
}

AssetID TextureLibrary::LoadFile(const std::wstring& path)
{
    const AssetID id = IdForFile(path);
    if (textures_.contains(id.value)) {
        return id;
    }
    const std::string utf8 = WideToUtf8(path);
    int w = 0, h = 0, comp = 0;
    stbi_uc* pixels = stbi_load(utf8.c_str(), &w, &h, &comp, 4);
    if (!pixels) {
        MYE_LOG_ERROR("texture load failed: %s (%s)", utf8.c_str(), stbi_failure_reason());
        return {};
    }
    Texture t;
    const bool ok = CreateFromPixels(t, pixels, w, h);
    stbi_image_free(pixels);
    if (!ok) {
        MYE_LOG_ERROR("texture creation failed: %s", utf8.c_str());
        return {};
    }
    textures_.emplace(id.value, std::move(t));
    return id;
}

AssetID TextureLibrary::CreateFromEncoded(std::string_view name, const void* bytes, size_t size)
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
    const bool ok = CreateFromPixels(t, pixels, w, h);
    stbi_image_free(pixels);
    if (!ok) {
        return {};
    }
    textures_[id.value] = std::move(t);
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
    return id;
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
    const std::string utf8 = WideToUtf8(path);
    int w = 0, h = 0, comp = 0;
    stbi_uc* pixels = stbi_load(utf8.c_str(), &w, &h, &comp, 4);
    if (!pixels) {
        MYE_LOG_ERROR("texture reload failed: %s (%s)", utf8.c_str(), stbi_failure_reason());
        return false;
    }
    Texture fresh;
    const bool ok = CreateFromPixels(fresh, pixels, w, h);
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
    return id;
}

Material* MaterialLibrary::Get(AssetID id)
{
    auto it = materials_.find(id.value);
    return (it != materials_.end()) ? &it->second : nullptr;
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

} // namespace mye
