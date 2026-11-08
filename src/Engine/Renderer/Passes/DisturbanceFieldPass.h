//====================================================================================
//                          DisturbanceFieldPass.h
//  MyEngin/ 秋田蓮音                                                       11/08/2026
//                                          かき乱しフィールド (M96) の 2D 層を GPU のテクスチャ配列へ転送する
//====================================================================================
#pragma once
#include <cstdint>

#include <d3d11.h>
#include <wrl/client.h>

namespace mye {

class GraphicsDevice;
struct SceneLightData;

// 同時に描ける 2D 層の数 (assets\shaders\disturbance_common.hlsli の MYE_DFLD_MAX_LAYERS と一致。規則 9)
constexpr int kDisturbanceMaxLayers = 4;
// CB 全体のバイト数 (同 MYE_DFLD_CB_BYTES と一致。規則 9)
constexpr int kDisturbanceFieldCbBytes = 336;
// コンピュートシェーダ (フロクセル注入) の束縛番号。HLSL の MYE_DFLD_SRV_FLOW / _AUX / _SAMPLER / _CB と一致 (規則 9)。
// 別の段が読むときは、その段の空きスロットへ張る
constexpr int kDisturbanceSrvFlowSlot = 1;
constexpr int kDisturbanceSrvAuxSlot = 2;
constexpr int kDisturbanceSamplerSlot = 1;
constexpr int kDisturbanceCbSlot = 1;

// 地面の霧 (ground_fog.hlsli) の PS 用の束縛番号。全ての PS で共通の固定番号で、RenderSystem が 1 回の描画に 1 回だけ張る。
// HLSL の MYE_GFOG_* と一致 (規則 9)。t20- は地形、b4 は速度、s0-s2 は IBL / 影が使う
constexpr int kGroundFogSrvFlowSlot = 32;
constexpr int kGroundFogSrvAuxSlot = 33;
constexpr int kGroundFogSamplerSlot = 8;
constexpr int kGroundFogCbSlot = 6;
constexpr int kGroundFogViewCbSlot = 7;
// 局所ライト (点・スポット) の CB。HLSL の MYE_GFOG_LIGHT_CB と一致 (規則 9)。b4 は速度が使う
constexpr int kGroundFogLightCbSlot = 8;
// 地面の霧が照らす局所ライトの最大本数 (同 MYE_GFOG_MAX_LIGHTS と一致。規則 9)
constexpr int kGroundFogMaxLights = 8;
// 板の積分の段数 (同 MYE_GFOG_STEPS と一致。規則 9)
constexpr int kGroundFogSteps = 24;
// 霧の位相関数の異方性 g と散乱アルベド (フロクセル注入の既定 FroxelSettings と同じ値 = 面の明るさの基準を揃える)
constexpr float kGroundFogAnisotropy = 0.3f;
constexpr float kGroundFogAlbedo = 0.9f;

// 2D 層 1 枚の転送指示。Engine 層が sim の状態 (または編集中の設定) から詰める。
// ★DisturbanceState を直接受け取らない — Renderer から Engine を参照すると層の依存が逆流する
//   (AcousticVolumeUpload と同じ。詰め替えは RenderSystem 側の仕事)
struct DisturbanceLayerUpload {
    // 状態が無い (編集中・Play 前) とき false。配列は読まず、restDensity の定数として描く
    bool hasState = false;
    int32_t cellsX = 0;
    int32_t cellsZ = 0;
    // 各 cellsX * cellsZ 個、x が最内。hasState のときだけ読む。null のチャネルは 0 として転送する
    const float* flowX = nullptr;
    const float* flowZ = nullptr;
    const float* density = nullptr;
    const float* trail = nullptr;
    const float* ripple = nullptr;
    const float* baseOffset = nullptr; // 列の基準高さ - baseY (起伏に沿う層。null = 0)
    int32_t originCellX = 0;
    int32_t originCellZ = 0;
    float cellSize = 0.25f;
    float baseY = 0.0f;
    float thickness = 1.0f;
    float restDensity = 1.0f;
    bool hasDensity = true; // 濃さチャネルがあるか (無い層は霧を出さない)
    float fogDensity = 2.0f;
    float fogColor[3] = { 1.0f, 1.0f, 1.0f };
    float fogBrightness = 1.0f;
    bool fogSunLight = false; // true: 平行光を霧に当てる (影なし)
    bool fogLocalLights = false; // true: 点・スポットのライトを霧に当てる (最大 kGroundFogMaxLights 本、影なし)
    // 場のエンティティ (index, generation)。どの層がどの場かを、転送を省く判定に使う
    uint32_t entityIndex = 0;
    uint32_t entityGeneration = 0;
};

struct DisturbanceFieldUpload {
    int32_t layerCount = 0;
    DisturbanceLayerUpload layers[kDisturbanceMaxLayers];
    // sim の通番。**前回と同じで層の構成も同じなら転送しない** (sim は 60 Hz だが描画は何千 fps で回る)
    uint64_t serial = 0;
};

// HLSL の DisturbanceLayer (float4 x 5) と同じ並び
struct DisturbanceLayerCB {
    float originSize[4] = {};
    float extent[4] = {};
    float look[4] = {};
    float fogColor[4] = {};
    float texScale[4] = {};
};

// HLSL の cbuffer GroundFogViewCB と同じ並び (1 回の描画ごとに変わる値)
struct GroundFogViewCB {
    float camPos[4] = {};   // xyz = カメラ位置, w = HG の異方性
    float sunDir[4] = {};   // xyz = 平行光の進行方向, w = 1: 平行光あり
    float sunColor[4] = {}; // xyz = 色 × 強度 (リニア), w = 散乱アルベド
    float ambient[4] = {};  // xyz = 環境光 (リニア)
    float invViewProj[16] = {}; // transpose(inverse(view * proj))
};
static_assert(sizeof(GroundFogViewCB) == 128, "HLSL の GroundFogViewCB と一致させること");

// 局所ライト 1 本の縮約した写し (HLSL の GroundFogLight と同じ並び。float4 x 4 = 64 バイト)
struct GroundFogLightCB {
    float posRange[4] = {};   // xyz = 位置, w = range [m]
    float colorKind[4] = {};  // rgb = 色 × 強度, w = 種類 (1 = 点, 2 = スポット)
    float dirInner[4] = {};   // xyz = 光の進行方向 (正規化), w = cosInner
    float outerPad[4] = {};   // x = cosOuter
};
static_assert(sizeof(GroundFogLightCB) == 64, "HLSL の GroundFogLight と一致させること");

// HLSL の cbuffer GroundFogLightCB と同じ並び。header.x = 本数, header.y = 照らす層のビット (bit i = 層 i)
struct GroundFogLightsCB {
    uint32_t header[4] = {};
    GroundFogLightCB lights[kGroundFogMaxLights];
};
static_assert(sizeof(GroundFogLightsCB) == 16 + 64 * kGroundFogMaxLights, "HLSL の GroundFogLightCB と一致させること");

// RenderSystem が詰める、この描画のカメラとライトの値
struct GroundFogViewParams {
    float camPos[3] = {};
    bool hasSun = false;
    float sunDir[3] = { 0.0f, -1.0f, 0.0f };
    float sunColor[3] = {};
    float ambient[3] = {};
    float invViewProj[16] = {}; // transpose(inverse(view * proj))
    // この描画のライト一覧 (局所ライトの選び方の入力)。null = 局所ライトなし
    const SceneLightData* lights = nullptr;
};

// HLSL の cbuffer DisturbanceFieldCB と同じ並び
struct DisturbanceFieldCB {
    uint32_t header[4] = {}; // x = 有効な層の数
    DisturbanceLayerCB layers[kDisturbanceMaxLayers];
};
static_assert(sizeof(DisturbanceFieldCB) == kDisturbanceFieldCbBytes, "HLSL の DisturbanceFieldCB と一致させること");

// 描き手 (フロクセル注入など) へ渡す束。**SRV は null のことがある** (全層が定数のとき等)。
// 読む側は CB の layerCount と、各層の extent.w (テクスチャを読むか) を見る
struct DisturbanceFieldGpu {
    ID3D11ShaderResourceView* flowSRV = nullptr; // Texture2DArray RGBA16F (vx, vz, 濃さ, 跡)
    ID3D11ShaderResourceView* auxSRV = nullptr;  // Texture2DArray RG16F (波紋 h, 基準面のずれ)
    DisturbanceFieldCB cb;
    // 局所ライトで照らす層のビット (bit i = 層 i)。層の CB は変えないので別に持つ
    uint32_t localLightMask = 0;
};

// 局所ライトの選び方 (純粋関数。同じ入力から同じ結果)。lights の点・スポットのうち、影響球が localLightMask の
// 層の板 (AABB) のどれかと交わるものを、ライト一覧の順に最大 kGroundFogMaxLights 本まで out へ詰める。
// 戻り値 = 条件を満たしたライトの総数 (kGroundFogMaxLights を超えた分は落とした本数 = 戻り値 - 8)
int SelectGroundFogLights(const SceneLightData& lights, const DisturbanceFieldCB& layers, uint32_t localLightMask,
                          GroundFogLightsCB& out);

// 2D 層の Texture2DArray の所有者。GPU は読むだけ — sim の状態は CPU (決定論) が持つ。
// 転送は層ごとの UpdateSubresource。配列の寸法は有効な層の最大値で、層ごとの UV 範囲は CB に載せる
class DisturbanceFieldPass {
public:
    // 転送して束を作る (配列の寸法が足りなければ作り直す)。戻り値 = 束が有効か。
    // false = 層が 0 枚 (または作成失敗): 消費側は場が無いときと同じ絵になる
    bool Upload(GraphicsDevice& device, const DisturbanceFieldUpload& src);
    void Release();

    // 地面の霧が読む CB / SRV / サンプラを PS の固定スロットへ張る (VS にも同じ番号で張る = サーフェスシェーダの MyeFieldSample 用)。
    // field が null / 層 0 枚なら何も張らず false。
    // 張ったら描画の最後に UnbindGroundFog で剥がすこと (他のビューの描画に残さない)
    bool BindGroundFog(GraphicsDevice& device, const DisturbanceFieldGpu* field, const GroundFogViewParams& view);
    void UnbindGroundFog(GraphicsDevice& device);

    const DisturbanceFieldGpu& Gpu() const { return gpu_; }
    // 直近の転送にかかった CPU 時間 [ms]。転送を省いたフレームは 0 (= 「速い」ではなく「していない」)
    float LastUploadMs() const { return uploadMs_; }

private:
    // 配列を作り直す (許容寸法はこの値以上)
    bool EnsureArrays(GraphicsDevice& device, int32_t width, int32_t height);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> flowTex_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> auxTex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> flowSrv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> auxSrv_;
    int32_t texWidth_ = 0;
    int32_t texHeight_ = 0;
    bool CreateGroundFogResources(GraphicsDevice& device);

    Microsoft::WRL::ComPtr<ID3D11Buffer> groundFogLayersCb_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> groundFogViewCb_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> groundFogLightsCb_;
    bool lightOverflowWarned_ = false; // 8 本を超えた WARN を出したか (超過が解消したら戻す)
    Microsoft::WRL::ComPtr<ID3D11SamplerState> groundFogSampler_;
    // 層ごとの基準高さのずれの最小 / 最大 (板を包む範囲)。転送したときに更新する
    float offsetRange_[kDisturbanceMaxLayers][2] = {};
    bool createFailed_ = false; // 同じ寸法で一度落ちたら毎フレーム再試行してログを溢れさせない
    int32_t failedWidth_ = 0;
    int32_t failedHeight_ = 0;

    // 最後に転送した内容の鍵。通番と、層の構成 (誰の・どの寸法の・状態の有無) が同じなら省く
    struct LayerKey {
        uint32_t entityIndex = 0;
        uint32_t entityGeneration = 0;
        int32_t cellsX = 0;
        int32_t cellsZ = 0;
        bool hasState = false;
        bool operator==(const LayerKey& o) const
        {
            return entityIndex == o.entityIndex && entityGeneration == o.entityGeneration && cellsX == o.cellsX
                   && cellsZ == o.cellsZ && hasState == o.hasState;
        }
    };
    LayerKey uploadedKeys_[kDisturbanceMaxLayers];
    int32_t uploadedCount_ = 0;
    uint64_t uploadedSerial_ = 0;
    bool everUploaded_ = false;
    DisturbanceFieldGpu gpu_;
    float uploadMs_ = 0.0f;
};

} // namespace mye
