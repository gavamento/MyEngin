#pragma once
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Core/EntityID.h"

namespace mye {

class GraphicsDevice;
class ShaderManager;

// ---- メッシュ ----

struct MeshVertex {
    DirectX::XMFLOAT3 position = { 0, 0, 0 };
    DirectX::XMFLOAT3 normal = { 0, 1, 0 };
    DirectX::XMFLOAT2 uv = { 0, 0 };
};

struct Mesh {
    Microsoft::WRL::ComPtr<ID3D11Buffer> vb;
    Microsoft::WRL::ComPtr<ID3D11Buffer> ib;
    uint32_t indexCount = 0;
};

class MeshLibrary {
public:
    void Init(GraphicsDevice& device) { device_ = &device; }
    AssetID Register(std::string_view name, std::span<const MeshVertex> vertices,
                     std::span<const uint32_t> indices);
    Mesh* Get(AssetID id);
    AssetID Cube(); // 単位キューブ (遅延生成)

private:
    GraphicsDevice* device_ = nullptr;
    std::unordered_map<uint64_t, Mesh> meshes_;
    AssetID cube_ = {};
};

// ---- テクスチャ ----

struct Texture {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    int width = 0;
    int height = 0;
};

class TextureLibrary {
public:
    void Init(GraphicsDevice& device) { device_ = &device; }
    AssetID LoadFile(const std::wstring& path);                                  // png/tga/jpg (stb_image)
    AssetID CreateFromEncoded(std::string_view name, const void* bytes, size_t size); // GLB 埋め込み等
    AssetID CreateSolid(std::string_view name, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    Texture* Get(AssetID id);
    AssetID White(); // 1x1 白 (遅延生成)

    // M3 ホットリロード: 同じ AssetID のまま中身を差し替える
    bool ReplaceFromFile(AssetID id, const std::wstring& path);

private:
    bool CreateFromPixels(Texture& out, const uint8_t* rgba, int w, int h);
    GraphicsDevice* device_ = nullptr;
    std::unordered_map<uint64_t, Texture> textures_;
    AssetID white_ = {};
};

// ---- マテリアル ----

struct Material {
    AssetID shader = {};  // ShaderManager のプログラム
    AssetID texture = {}; // ベースカラーテクスチャ (無ければ White)
    DirectX::XMFLOAT4 baseColor = { 1, 1, 1, 1 };
    int32_t transparent = 0; // 0=opaque, 1=alpha blend
};

class MaterialLibrary {
public:
    AssetID Register(std::string_view name, const Material& mat);
    Material* Get(AssetID id);
    AssetID Default(ShaderManager& shaders, TextureLibrary& textures); // 灰色 forward_lit (遅延生成)

private:
    std::unordered_map<uint64_t, Material> materials_;
    AssetID default_ = {};
};

// Renderer リソース一式 (Engine 層へはこの束で渡す)
struct RenderResources {
    MeshLibrary meshes;
    TextureLibrary textures;
    MaterialLibrary materials;

    void Init(GraphicsDevice& device)
    {
        meshes.Init(device);
        textures.Init(device);
    }
};

} // namespace mye
