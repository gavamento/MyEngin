#pragma once
#include <condition_variable>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Core/EntityID.h"
#include "Engine/Renderer/Skeleton.h"

namespace mye {

class GraphicsDevice;
class ShaderManager;

// ---- メッシュ ----

struct MeshVertex {
    DirectX::XMFLOAT3 position = { 0, 0, 0 };
    DirectX::XMFLOAT3 normal = { 0, 1, 0 };
    DirectX::XMFLOAT2 uv = { 0, 0 };
    // スキニング (M18)。末尾配置なので非スキンシェーダ (POSITION/NORMAL/TEXCOORD0 の 3 要素
    // レイアウト) は同じ VB(ストライド 52B)でこの領域を無視でき、二重 VB を避けられる。
    // 非スキンメッシュは weight=0 (スキニング VS 側で恒等フォールバック)。
    uint8_t boneIndices[4] = { 0, 0, 0, 0 };        // BLENDINDICES (R8G8B8A8_UINT)
    DirectX::XMFLOAT4 boneWeights = { 0, 0, 0, 0 }; // BLENDWEIGHT (R32G32B32A32_FLOAT)
};

struct Mesh {
    Microsoft::WRL::ComPtr<ID3D11Buffer> vb;
    Microsoft::WRL::ComPtr<ID3D11Buffer> ib;
    uint32_t indexCount = 0;
    // ローカル空間 AABB (Register 時に頂点から計算)。Focus/ピッキング/サムネイルで使う (M8)
    DirectX::XMFLOAT3 aabbMin = { 0, 0, 0 };
    DirectX::XMFLOAT3 aabbMax = { 0, 0, 0 };
};

// アセット列挙の 1 件 (Asset Browser / 参照ピッカー用、M8)。
// ライブラリはハッシュしか保持しないため、名前を別に覚えて列挙可能にする
struct AssetEntry {
    AssetID id = {};
    std::string name;
};

class MeshLibrary {
public:
    void Init(GraphicsDevice& device) { device_ = &device; }
    AssetID Register(std::string_view name, std::span<const MeshVertex> vertices,
                     std::span<const uint32_t> indices);
    Mesh* Get(AssetID id);
    // 組み込みプリミティブ (いずれも遅延生成・中心原点・単位サイズ基準)
    AssetID Cube();     // 単位キューブ (辺長 1)
    AssetID Sphere();   // UV 球 (半径 0.5)
    AssetID Plane();    // XZ 平面 (1x1, 法線 +Y)
    AssetID Quad();     // XY 平面 (1x1, 法線 +Z)
    AssetID Cylinder(); // 円柱 (半径 0.5, 高さ 1)
    AssetID Capsule();  // カプセル (半径 0.5, 全高 2)

    // 登録済みメッシュを名前順で列挙 (エディタ UI 用)
    std::vector<AssetEntry> Enumerate() const;

private:
    GraphicsDevice* device_ = nullptr;
    std::unordered_map<uint64_t, Mesh> meshes_;
    std::unordered_map<uint64_t, std::string> names_;
    AssetID cube_ = {};
    AssetID sphere_ = {};
    AssetID plane_ = {};
    AssetID quad_ = {};
    AssetID cylinder_ = {};
    AssetID capsule_ = {};
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
    ~TextureLibrary();
    AssetID LoadFile(const std::wstring& path);                                  // png/tga/jpg (stb_image)
    AssetID CreateFromEncoded(std::string_view name, const void* bytes, size_t size); // GLB 埋め込み等 (再呼び出しで差し替え)
    AssetID CreateSolid(std::string_view name, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    Texture* Get(AssetID id);
    AssetID White(); // 1x1 白 (遅延生成)

    // M23 非同期ロード: 即座に AssetID を返し、白のプレースホルダを cache に入れる。
    // CPU デコード (stb_image) はワーカースレッド、GPU 作成+差し替えは PollAsyncLoads
    // (メインスレッドのセーフポイント) で行う。決定論には無関係 (テクスチャは非ハッシュ)。
    // 毎フレーム同じパスで呼んでも冪等 (ロード済み/処理中は即 return)。
    AssetID RequestLoadFileAsync(const std::wstring& path);
    // メインスレッドで毎フレーム呼ぶ。デコード完了分を GPU テクスチャ化して公開する。
    void PollAsyncLoads();

    // ファイルパスに対応する AssetID (正規化パスのハッシュ)。ロード有無に関わらず同じ値
    static AssetID IdForFile(const std::wstring& path);

    // M3 ホットリロード: 同じ AssetID のまま中身を差し替える
    bool ReplaceFromFile(AssetID id, const std::wstring& path);

    // 登録済みテクスチャを名前 (パス or 生成名) 順で列挙 (エディタ UI 用)
    std::vector<AssetEntry> Enumerate() const;

private:
    bool CreateFromPixels(Texture& out, const uint8_t* rgba, int w, int h);
    bool LoadDdsInto(Texture& out, const std::wstring& path); // M24: BCn/DDS (依存ゼロ)
    void EnsureWorker();
    void AsyncWorker();

    GraphicsDevice* device_ = nullptr;
    std::unordered_map<uint64_t, Texture> textures_;
    std::unordered_map<uint64_t, std::string> names_;
    AssetID white_ = {};

    // ---- 非同期ロード (M23) ----
    struct DecodeJob {
        uint64_t id = 0;
        std::string utf8Path;
    };
    struct DecodeResult {
        uint64_t id = 0;
        std::vector<uint8_t> pixels; // RGBA8
        int w = 0;
        int h = 0;
        bool ok = false;
    };
    std::thread worker_;
    std::mutex asyncMutex_;
    std::condition_variable asyncCv_;
    std::deque<DecodeJob> jobQueue_;       // asyncMutex_ で保護
    std::vector<DecodeResult> doneQueue_;  // asyncMutex_ で保護
    std::unordered_set<uint64_t> pending_; // メインスレッド専用 (二重投入ガード)
    bool workerStop_ = false;              // asyncMutex_ で保護
    bool workerStarted_ = false;           // メインスレッド専用
};

// ---- マテリアル ----

struct Material {
    AssetID shader = {};    // ShaderManager のプログラム
    AssetID texture = {};   // ベースカラーテクスチャ (無ければ White)
    AssetID normalTex = {}; // ノーマルマップ (M17.3、無ければフラット法線)
    DirectX::XMFLOAT4 baseColor = { 1, 1, 1, 1 };
    float metallic = 0.0f;  // 0=誘電体 1=金属 (PBR、M17)
    float roughness = 0.5f; // 0=鏡面 1=拡散
    int32_t transparent = 0; // 0=opaque, 1=alpha blend
    int32_t pad = 0;
};

class MaterialLibrary {
public:
    AssetID Register(std::string_view name, const Material& mat);
    Material* Get(AssetID id);
    AssetID Default(ShaderManager& shaders, TextureLibrary& textures); // 灰色 forward_lit (遅延生成)

    // .mat.json ファイルマテリアル (M17)。AssetID = 正規化パスのハッシュ (テクスチャ/アニメと同方式)。
    // 名前ハッシュの Register とは別系統だが同じ materials_ に入り Get で解決できる。
    // JSON: {shader, baseColor[4], metallic, roughness, texture, normalMap, transparent}。
    // texture/normalMap は assetsRoot 相対パス (空なら texture=White / normal=なし)。
    static AssetID HashForPath(const std::wstring& path);
    AssetID LoadFromFile(const std::wstring& path, TextureLibrary& textures,
                         const std::wstring& assetsRoot); // 失敗時 null

    // 登録済みマテリアルを名前順で列挙 (エディタ UI 用)
    std::vector<AssetEntry> Enumerate() const;

private:
    std::unordered_map<uint64_t, Material> materials_;
    std::unordered_map<uint64_t, std::string> names_;
    AssetID default_ = {};
};

// Renderer リソース一式 (Engine 層へはこの束で渡す)
struct RenderResources {
    MeshLibrary meshes;
    TextureLibrary textures;
    MaterialLibrary materials;
    SkinnedModelLibrary skinnedModels; // スケルトン + クリップ (M18)

    void Init(GraphicsDevice& device)
    {
        meshes.Init(device);
        textures.Init(device);
    }
};

} // namespace mye
