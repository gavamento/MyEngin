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
// M67: Material::reflectionClass の既定値を kRtReflClassDefault 1 箇所から取るため。
// RtTypes.h は <cstdint> と <DirectXMath.h> しか引かない (どちらも上で取り込み済み)
#include "Engine/Renderer/RayTracing/RtTypes.h"
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
    // M41: メッシュコライダー用の CPU コピー (位置のみ + インデックス)。
    // MeshColliderLibrary が BVH 構築素材に使う (GPU アップロード後も保持)
    std::vector<DirectX::XMFLOAT3> positions;
    std::vector<uint32_t> indices;
    // M46a: レイトレーシングのヒット属性用 (positions と同じ頂点順・同じ長さ)。
    // BLAS の三角形属性 (補間法線 / UV) をここから焼く。+20B/頂点
    std::vector<DirectX::XMFLOAT3> normals;
    std::vector<DirectX::XMFLOAT2> uvs;
};

// アセット列挙の 1 件 (Asset Browser / 参照ピッカー用、M8)。
// ライブラリはハッシュしか保持しないため、名前を別に覚えて列挙可能にする
struct AssetEntry {
    AssetID id = {};
    std::string name;
};

class MeshLibrary {
public:
    // ★M70d (dogfooding #12): 組込みプリミティブ 6 種を**ここで登録し切る**。
    //   遅延生成にすると、Runtime で生きているのは誰かが**副作用**で作ったものだけになり、
    //   「エディタで作った円柱を含むシーンが Runtime では黙って描画されない」が起きる
    //   (使える組込みメッシュが**実行環境で変わる**)。
    //   6 つで合計数百頂点なので遅延にする価値が無い。
    //   ★Init を呼ばない CPU 専用モード (TerrainSelfTest) は 1 本も作らない
    void Init(GraphicsDevice& device)
    {
        device_ = &device;
        Cube();
        Sphere();
        Plane();
        Quad();
        Cylinder();
        Capsule();
        WaterPlane();
    }
    AssetID Register(std::string_view name, std::span<const MeshVertex> vertices,
                     std::span<const uint32_t> indices);
    Mesh* Get(AssetID id);
    // 登録名の逆引き (未登録は nullptr)。モデル由来なら "guid://<16hex>#mesh0#prim0" (M74a) —
    // M60f の凸包クックが「この AssetID の元ファイルはどれか」を知る唯一の手段
    const std::string* NameOf(AssetID id) const;
    // 組み込みプリミティブ (中心原点・単位サイズ基準)。Init が 6 つとも先に登録するので
    // 実行中の初回呼び出しは常にキャッシュヒット (遅延生成の形は CPU 専用モードのために残す)
    AssetID Cube();     // 単位キューブ (辺長 1)
    AssetID Sphere();   // UV 球 (半径 0.5)
    AssetID Plane();    // XZ 平面 (1x1, 法線 +Y)
    AssetID Quad();     // XY 平面 (1x1, 法線 **-Z** = +Z を向く既定カメラから正面が見える)
    AssetID Cylinder(); // 円柱 (半径 0.5, 高さ 1)
    AssetID Capsule();  // カプセル (半径 0.5, 全高 2)
    AssetID WaterPlane(int resolution = 64, float size = 50.0f); // 分割 XZ 水面グリッド

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
    AssetID waterPlane_ = {};
};

// ---- テクスチャ ----

struct Texture {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    int width = 0;
    int height = 0;
    bool srgb = false; // M38a: _SRGB フォーマットでロード済み (ホットリロードで維持)
};

class TextureLibrary {
public:
    void Init(GraphicsDevice& device) { device_ = &device; }
    ~TextureLibrary();
    // png/tga/jpg (stb_image) / dds。srgb=true でアルベド系を _SRGB フォーマットに
    // (サンプル時に HW デコード = リニアパイプライン、M38a)。ノーマル/データ系は false。
    // 既ロードの AssetID は先勝ち (フラグ違いの再要求は無視 — per-asset 指定は M39 .meta で)
    AssetID LoadFile(const std::wstring& path, bool srgb = false);
    AssetID CreateFromEncoded(std::string_view name, const void* bytes, size_t size,
                              bool srgb = false); // GLB 埋め込み等 (再呼び出しで差し替え)
    AssetID CreateSolid(std::string_view name, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    // 生 RGBA8 画素からテクスチャを作る (ファイル実体を持たない生成物用。M58d の地形
    // スプラットマップが最初の利用者 — 重みは `.mterr` の中にしか無い)。
    // **srgb は既定 false**: スプラットの中身は色ではなく重みで、sRGB デコードを掛けると
    // チャンネル和が 255 でなくなる (= レイヤの合計が狂う)。名前が同じなら先勝ち
    AssetID CreateFromRgba8(std::string_view name, const uint8_t* rgba, int w, int h,
                            bool srgb = false, bool mips = true);
    Texture* Get(AssetID id);
    AssetID White(); // 1x1 白 (遅延生成)

    // M23 非同期ロード: 即座に AssetID を返し、白のプレースホルダを cache に入れる。
    // CPU デコード (stb_image) はワーカースレッド、GPU 作成+差し替えは PollAsyncLoads
    // (メインスレッドのセーフポイント) で行う。決定論には無関係 (テクスチャは非ハッシュ)。
    // 毎フレーム同じパスで呼んでも冪等 (ロード済み/処理中は即 return)。
    AssetID RequestLoadFileAsync(const std::wstring& path);
    // メインスレッドで毎フレーム呼ぶ。デコード完了分を GPU テクスチャ化して公開する。
    void PollAsyncLoads();
    // M52c: 決定的スクショ用。進行中の非同期デコードが**全て公開されるまで**待つ。
    // これが無いと「撮影フレームまでにデコードが間に合ったか」= 実時間で絵が変わる。
    // タイムアウト付き (デコード結果が返らない事故でも CI を吊らさない)
    void WaitForAsyncLoads(int timeoutMs = 30000);

    // ファイルパスに対応する AssetID (正規化パスのハッシュ)。ロード有無に関わらず同じ値
    static AssetID IdForFile(const std::wstring& path);

    // M3 ホットリロード: 同じ AssetID のまま中身を差し替える
    bool ReplaceFromFile(AssetID id, const std::wstring& path);

    // 登録済みテクスチャを名前 (パス or 生成名) 順で列挙 (エディタ UI 用)
    std::vector<AssetEntry> Enumerate() const;

private:
    bool CreateFromPixels(Texture& out, const uint8_t* rgba, int w, int h, bool srgb = false,
                          bool mips = true);
    bool LoadDdsInto(Texture& out, const std::wstring& path,
                     bool srgb = false); // M24: BCn/DDS (依存ゼロ)
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
    // 自己発光の強さ (M46i)。放射輝度 = baseColor(リニア) * emissiveIntensity。
    // 0 = 発光なし (既定) — このとき全経路で加算項がちょうど 0 になり従来の絵とビット一致する。
    // ラスタでは Deferred が gbMaterial.b へ kEmissiveMaxIntensity 正規化で符号化するため
    // 上限 kEmissiveMaxIntensity・量子化 1/255 刻み (Forward は CB 直渡しで量子化なし)。
    // レイトレでは RtMaterial.emissive に載り、そのままバウンス先の光源になる
    float emissiveIntensity = 0.0f;
    // M67: 反射に映るときの品質クラス (RtTypes.h の kRtReflClass*、0=Hero … 4=Default)。
    // ★**Material 単位**にしてあるのは、値の出所を .mat.json 1 箇所に閉じるため
    //   (オブジェクト単位の上書きは ECS / シーン / プレハブ override の全部に波及する)。
    //   ReSTIR は「反射に**映る**物体」の重要度で再利用の強さを決めるので、
    //   受け側 (映す面) ではなくヒット側から引く = RtInstance に載せて GPU へ運ぶ。
    // ★欠損・範囲外を 4 (中立) にするのはクランプを避けるため — -1 や 99 の打ち間違いが
    //   0 (Hero) に丸まると「最も保守的で最も重いクラス」に化けて静かにコストが増える
    int32_t reflectionClass = kRtReflClassDefault;
    // ★暗黙パディングを作らないための明示的な詰め物 (値は読まない)。
    //   Material は cooked キャッシュ (ModelCook) に **memcpy で丸ごと書かれ**、
    //   CookedCacheSelfTest が sizeof(Material) 全体を memcmp する。AssetID が uint64 =
    //   アラインメント 8 なので、reflectionClass を足した 60 バイトは 64 に丸められる —
    //   その 4 バイトを暗黙パディングのままにすると値が不定になり、
    //   「同じ入力から作った cooked ファイルのバイト列が run ごとに違う」が生まれる
    int32_t pad0 = 0;
};

// M79 sub-02: .mat.json の shader が "*.surface" 短名のときの遅延解決状態。
// ForwardPath (将来 DeferredPath も) が描画直前に MaterialLibrary::GetOrBuildSurfaceState で
// 取得する。isSurfaceShader=false ならこのマテリアルは対象外 (forward_lit 等、従来経路)
struct SurfaceMaterialState {
    bool isSurfaceShader = false;  // shader 名が "*.surface" 短名か
    bool ready = false;            // 色エントリ (colorVS/colorPS) が有効 = 描画してよい
    bool useErrorFallback = false; // ready=false のとき、surface_error で代替描画すべきか
    std::string errorMessage;      // 失敗理由 (Inspector バナー用、sub-04)
    AssetID surfaceProgramId = {}; // shaders.GetSurface() に渡す ID (ready 時のみ意味を持つ)
    std::vector<uint8_t> perMaterialCB;                    // MyEnginePerMaterial パック済みバイト
    Microsoft::WRL::ComPtr<ID3D11Buffer> perMaterialGpuCB; // ↑を書いた GPU 側 CB
    std::unordered_map<std::string, AssetID> textures;     // 作者 Texture2D 名 → 解決済みテクスチャ

    // ---- 内部用 (再パック要否の判定。MaterialLibrary::GetOrBuildSurfaceState だけが読み書きする) ----
    uint64_t builtFromRevision = 0;
    uint64_t builtFromGeneration = 0;
};

class MaterialLibrary {
public:
    AssetID Register(std::string_view name, const Material& mat);
    // 名前を持たない一時マテリアル (エディタのプレビュー用、M53)。Enumerate は names_ を
    // 舐めるので、ここで入れたものは参照ピッカー (MeshRenderer.material 等) に現れない。
    // key は呼び出し側が決める固定値 (同じ key への再登録は上書き)
    AssetID RegisterAnonymous(uint64_t key, const Material& mat);
    Material* Get(AssetID id);
    AssetID Default(ShaderManager& shaders, TextureLibrary& textures); // 灰色 forward_lit (遅延生成)

    // .mat.json ファイルマテリアル (M17)。AssetID = 正規化パスのハッシュ (テクスチャ/アニメと同方式)。
    // 名前ハッシュの Register とは別系統だが同じ materials_ に入り Get で解決できる。
    // JSON: {shader, baseColor[4], metallic, roughness, texture, normalMap, transparent}。
    // texture/normalMap は assetsRoot 相対パス (空なら texture=White / normal=なし)。
    static AssetID HashForPath(const std::wstring& path);
    AssetID LoadFromFile(const std::wstring& path, TextureLibrary& textures,
                         const std::wstring& assetsRoot); // 失敗時 null

    // JSON テキストから Material を組むだけ (ライブラリには登録しない、M53)。
    // LoadFromFile と**同じ本体**を通るので、Inspector のプレビューは「保存したらこう見える」と
    // 必ず一致する。テキストを受けるのはこのヘッダに nlohmann を持ち込まないため
    static bool MaterialFromJsonText(std::string_view text, TextureLibrary& textures,
                                     const std::wstring& assetsRoot, Material& out);

    // 登録済みマテリアルを名前順で列挙 (エディタ UI 用)
    std::vector<AssetEntry> Enumerate() const;

    // M79 sub-02: shader が "*.surface" 短名のマテリアルの遅延解決 (Load・Properties パック・
    // Tex2D 解決)。対象外 (forward_lit 等) のマテリアルは nullptr を返す — 呼び出し側は
    // 従来どおり mat->shader を ShaderManager::Get() へ渡す経路を使うこと。
    // 変化 (JSON 再読込・シェーダの世代) が無ければ再パックせず前回の結果を返す
    SurfaceMaterialState* GetOrBuildSurfaceState(AssetID materialId, ShaderManager& shaders,
                                                 TextureLibrary& textures, GraphicsDevice& device);

    // M79 sub-06: 視錐台カリング / CSM キャスター AABB 集約の余白 [m]。サーフェスでない
    // マテリアル (横テーブルに未登録) は 0 を返す — padding は非サーフェスに効かない (spec §4.1)。
    // ジョブ並列のカリングステージの中で呼ばないこと (直列のステージ 1 で解決してキャッシュする)
    float GetSurfaceBoundsPadding(AssetID materialId) const;
    // 両面描画 (Cull None) か。サーフェスでなければ false
    bool GetSurfaceDoubleSided(AssetID materialId) const;

private:
    std::unordered_map<uint64_t, Material> materials_;
    std::unordered_map<uint64_t, std::string> names_;
    AssetID default_ = {};

    // ---- M79 sub-02: サーフェスマテリアルの横テーブル ----
    // .mat.json の生テキスト由来 (ShaderManager 不要で作れる部分)。LoadFromFile のたびに
    // shaderName が "*.surface" なら更新し、そうでなければ消す (シェーダを forward_lit へ
    // 戻した場合に古い側テーブルが残らないように)
    struct SurfaceMaterialSource {
        std::string shaderName;
        std::string propertiesJson; // "properties" オブジェクトの JSON テキスト ("{}" = 無し)
        std::wstring assetsRoot;
        uint64_t revision = 0; // LoadFromFile のたびに増分 (JSON 内容の変化の検出に使う)
        // M79 sub-06: 視錐台カリング / CSM キャスター AABB の余白 [m] (.mat.json boundsPadding、
        // 欠損 0・負値は 0 に丸め)。両面描画 (Cull None、.mat.json doubleSided、欠損 false)
        float boundsPadding = 0.0f;
        bool doubleSided = false;
    };
    std::unordered_map<uint64_t, SurfaceMaterialSource> surfaceSources_;
    std::unordered_map<uint64_t, SurfaceMaterialState> surfaceStates_;
    uint64_t nextSurfaceRevision_ = 1;
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
