/*----
 ComputeAbiRunner.h  ABI v21 コンピュートバッファ管理・名前バインドディスパッチ (M78e)
 作成者: 秋田蓮音                                09/22/2026
----*/
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

namespace mye {

class GraphicsDevice;
class ShaderManager;
class TextureLibrary;

// ABI v21 コンピュートバッファ管理・名前バインドディスパッチ。
// GameLogic/C# から CreateComputeBuffer→SetComputeBuffer/Float/Texture→DispatchCompute→Release
// の経路を提供する (spec §4.5)。
// 生 D3D 型は Shared に出さない。メモリ確保・解放はエンジン側。
class ComputeAbiRunner
{
public:
    // ABI バッファ数の上限 (spec §4.4 性能制約)
    static constexpr int kMaxAbiBuffers = 64;

    // ---------------------------------------------------------------------------
    // バッファ管理
    // ---------------------------------------------------------------------------

    // 構造化バッファを作成し不透明ハンドルを返す。
    // dev が null または作成失敗は 0。
    // ハンドル形式: 上位 32bit = 世代 (0 は無効)、下位 32bit = スロット index。
    // 世代はプロセス内で単調増加し、解放・シーン遷移後の再利用では一致しない
    uint64_t CreateBuffer(ID3D11Device* dev, uint32_t count, uint32_t stride, uint32_t flags);

    // バッファ解放。無効 ID・二重解放は no-op で落ちない
    void ReleaseBuffer(uint64_t id);

    // ---------------------------------------------------------------------------
    // per-shader ペンディング状態の設定 (Dispatch 時に適用される)
    // ---------------------------------------------------------------------------

    // バッファ名バインド。無効ハンドル、またはシェーダに無い名前は 0
    int SetBuffer(ShaderManager& shaders, const char* shader, const char* bufName,
                  uint64_t bufferId);

    // cbuffer 変数へ値を予約する。未知名・未知シェーダは 0
    int SetFloat(ShaderManager& shaders, const char* shader, const char* propName, float value);
    int SetFloat4(ShaderManager& shaders, const char* shader, const char* propName,
                  float x, float y, float z, float w);

    // AssetID または組み込み名キー (HashStr("white") / HashStr("builtin://white"))。
    // 未解決・未知名は 0
    int SetTextureFromAsset(ShaderManager& shaders, TextureLibrary* texLib,
                            const char* shader, const char* texName, uint64_t assetId);

    // ---------------------------------------------------------------------------
    // ディスパッチ
    // ---------------------------------------------------------------------------

    // シェーダを LoadCompute し、ペンディング状態を適用してから Dispatch する。
    // シェーダ無効・デバイス無し・グループ数 0 は 0 (クラッシュしない)
    int Dispatch(GraphicsDevice& device, ShaderManager& shaders, TextureLibrary* texLib,
                 const char* shader, uint32_t gx, uint32_t gy, uint32_t gz);

    // ---------------------------------------------------------------------------
    // シャットダウン
    // ---------------------------------------------------------------------------

    // 未解放バッファを WARN して全リソースを解放する
    void Shutdown();

private:
    // ---------------------------------------------------------------------------
    // バッファスロット
    // ---------------------------------------------------------------------------
    struct BufferSlot
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer>              buf;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>  srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
        uint32_t generation = 0; // 0 は未発行。解放しても戻さない
        bool     live       = false;
        bool     hasUav     = false;
    };

    // ---------------------------------------------------------------------------
    // per-shader ペンディング状態
    // ---------------------------------------------------------------------------
    struct ShaderState
    {
        // 名前 → ABI バッファハンドル
        std::unordered_map<std::string, uint64_t>             bufferBindings;
        // 名前 → float4 値 (SetComputeFloat は [0] のみ使用)
        std::unordered_map<std::string, std::array<float, 4>> floatValues;
        // 名前 → AssetID (テクスチャ)
        std::unordered_map<std::string, uint64_t>             textureBindings;
    };

    // ---------------------------------------------------------------------------
    // ヘルパ
    // ---------------------------------------------------------------------------

    // ハンドルからスロットポインタを取得 (世代不一致・範囲外は nullptr)
    BufferSlot* ResolveSlot(uint64_t id);

    // shader の ShaderState を取得 (無ければ新規作成)
    ShaderState& GetOrCreateState(const std::string& shader);

    // ---------------------------------------------------------------------------
    // メンバ
    // ---------------------------------------------------------------------------
    // 上限が定数なので COM ポインタを再配置しない固定表にする
    std::array<BufferSlot, kMaxAbiBuffers>           bufSlots_{};
    uint32_t                                         slotCount_ = 0; // 一度でも使ったスロットの高水位
    std::vector<uint32_t>                            freeList_; // 再利用可スロット index
    std::unordered_map<std::string, ShaderState>     shaderStates_;
    uint32_t                                         nextGeneration_ = 1; // Shutdown でも戻さない
};

} // namespace mye
