//====================================================================================
//                          OcclusionCullPass.h
//  MyEngin/ 秋田蓮音                                                       10/09/2026
//                                          GPU オクルージョンカリング (2 フェーズ、max-Z HZB)
//====================================================================================
#pragma once
#include <cstdint>
#include <vector>

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Core/Ecs/EntityID.h"
#include "Engine/Renderer/Device/GpuTimer.h"
#include "Engine/Renderer/Passes/HzbPass.h"

namespace mye {

class GraphicsDevice;
class ShaderManager;

// 判定対象の項目 1 個。同じ描画コマンドの項目は連続して並べる
struct OcclusionItemIn {
    EntityID entity = {}; // 可視ビットの添字 (entity.index) と履歴の世代照合に使う
    float bmin[3] = { 0.0f, 0.0f, 0.0f };
    float bmax[3] = { 0.0f, 0.0f, 0.0f };
    bool alwaysDraw = false; // AABB が保守的でない等で判定せずフェーズ 1 で常に描く
};

// 描画コマンド 1 個 = インスタンス run 1 本、または単発 1 個
struct OcclusionCmdIn {
    uint32_t firstItem = 0;
    uint32_t itemCount = 0;
    uint32_t instanceBase = 0; // run の world 行列の開始位置
    bool isInstanced = false;
    uint32_t indexCount = 0;
    uint32_t startIndex = 0;
};

// GPU が数えた統計。数フレーム遅れのステージング読みで、描画判断には使わない
struct OcclusionStats {
    bool valid = false;
    int phase1Draws = 0;
    int phase2Draws = 0;
    int occluded = 0;
};

// viewKey ごとの履歴 (max-Z ピラミッド、可視ビット) と、フレーム単位の作業バッファを持つ。
//
// 流れ (DeferredPath が呼ぶ順):
//   Begin → SelectPhase1 → [フェーズ 1 を ArgsBuffer の phase 0 で描く] →
//   TestPhase2 → [フェーズ 2 を phase 1 で描く]
// 失敗 (リソース作成、ピラミッド作成) は 1 度ログを出して全ビューで OFF にする (Shutdown で復帰)。
// OFF の間 Begin は false を返し、呼び出し側は従来の描画に落ちる。
class OcclusionCuller {
public:
    static constexpr int kViewSlots = 4; // viewKey 0..3。0 は履歴を持たない = 使わない

    bool Init(GraphicsDevice& device, ShaderManager& shaders);
    void Shutdown();

    bool IsDisabled() const { return disabled_; }
    // selftest 用: 次の Begin でリソース作成が失敗したことにする
    void InjectCreateFailureForTest(bool on) { injectFailure_ = on; }

    struct FrameDesc {
        uint32_t viewKey = 0;
        uint32_t serial = 0; // ビュー別の描画通番。前回 + 1 でなければ履歴を捨てる
        int width = 0;
        int height = 0;
        DirectX::XMFLOAT4X4 viewProjT = {}; // transpose(view * proj)。GBuffer と同じジッタ込み
        uint32_t worldCount = 0;            // インスタンスバッファの行列数
    };
    // 項目とコマンドを GPU へ上げ、このフレームの作業を準備する。false = このフレームは使えない
    bool Begin(GraphicsDevice& device, ShaderManager& shaders, const FrameDesc& desc,
               const std::vector<OcclusionItemIn>& items, const std::vector<OcclusionCmdIn>& cmds);
    // フェーズ 1 の引数を書く。前フレームに可視だった項目 (と常に描く項目) を詰める
    void SelectPhase1(GraphicsDevice& device, ShaderManager& shaders);
    // depthSRV から max-Z を作り、全項目を判定してフェーズ 2 の引数を書く。
    // 呼ぶ前に深度を RTV/DSV から外しておくこと (CS が深度を SRV で読む)
    void TestPhase2(GraphicsDevice& device, ShaderManager& shaders, ID3D11ShaderResourceView* depthSRV);

    ID3D11Buffer* ArgsBuffer() const { return args_.Get(); }
    ID3D11ShaderResourceView* RemapSRV() const { return remapSrv_.Get(); }
    uint32_t CmdCount() const { return cmdCount_; }
    uint32_t WorldCount() const { return worldCount_; }
    // phase 0/1 のコマンド cmdIdx の DrawIndexedInstancedIndirect 引数のバイト位置
    uint32_t ArgsByteOffset(int phase, uint32_t cmdIdx) const
    {
        return (static_cast<uint32_t>(phase) * cmdCount_ + cmdIdx) * kArgsStride;
    }
    // フェーズ phase の remap 領域の先頭 (VS の PerObject に渡す値の元)
    uint32_t RemapRegion(int phase) const { return static_cast<uint32_t>(phase) * worldCount_; }

    // 直近に読めた統計 (2 フレーム遅れ)
    OcclusionStats Stats(uint32_t viewKey) const;
    // 直近フレームの判定 + ピラミッド構築の GPU 時間 [ms]
    float GpuMs() const;

    static constexpr uint32_t kArgsStride = 20; // DrawIndexedInstancedIndirect の引数 5 個

private:
    struct ViewState {
        HzbPass pyramid;
        bool pyramidInit = false;
        Microsoft::WRL::ComPtr<ID3D11Buffer> visBuf;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> visUav;
        uint32_t visCapacity = 0;
        // 履歴 (PrevRenderWorldStore と同じ流儀: 世代 + 描いたときの通番)
        std::vector<uint32_t> generation; // entity.generation + 1 (0 = 未使用)
        std::vector<uint32_t> slotSerial;
        uint32_t lastSerial = 0;
        int width = 0;
        int height = 0;
        bool valid = false;
        // 統計のステージングリング
        Microsoft::WRL::ComPtr<ID3D11Buffer> staging[3];
        uint32_t writeCount = 0;
        OcclusionStats stats;
    };

    bool Fail(const char* why);
    bool EnsureView(GraphicsDevice& device, ShaderManager& shaders, ViewState& vs, uint32_t slotCapacity);
    bool EnsureScratch(GraphicsDevice& device, uint32_t itemCount, uint32_t cmdCount,
                       uint32_t worldCount);
    void Dispatch(GraphicsDevice& device, ID3D11ComputeShader* cs, int mode, int phase,
                  ID3D11ShaderResourceView* hzb, ViewState& vs);
    void ReadStats(GraphicsDevice& device, ViewState& vs);

    AssetID cullCS_ = {};
    bool disabled_ = false;
    bool injectFailure_ = false;
    bool initialized_ = false;

    ViewState views_[kViewSlots];
    uint32_t activeView_ = 0;
    FrameDesc frame_ = {};
    uint32_t cmdCount_ = 0;
    uint32_t worldCount_ = 0;

    // フレーム単位の作業バッファ (ビュー間で使い回す。GPU は命令順に処理するので衝突しない)
    Microsoft::WRL::ComPtr<ID3D11Buffer> items_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> itemsSrv_;
    uint32_t itemCapacity_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cmds_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cmdsSrv_;
    uint32_t cmdCapacity_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Buffer> args_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> argsUav_;
    uint32_t argsCapacity_ = 0; // コマンド数 (1 フェーズぶん)
    Microsoft::WRL::ComPtr<ID3D11Buffer> remap_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> remapUav_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> remapSrv_;
    uint32_t remapCapacity_ = 0; // 行列数 (1 フェーズぶん)
    Microsoft::WRL::ComPtr<ID3D11Buffer> stats_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> statsUav_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    GpuTimer selectTimer_;
    GpuTimer testTimer_;
};

} // namespace mye
