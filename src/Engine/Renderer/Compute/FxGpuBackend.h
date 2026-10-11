//====================================================================================
//                          FxGpuBackend.h
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          Fx バッファの GPU 実行 (upload・dispatch・遅延読み戻しリング)
//====================================================================================
#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

namespace mye {

class GraphicsDevice;
class ShaderManager;

namespace compute {

// ForEach の Fx バッファを GPU で走らせる。D3D の型はここに閉じ込める (Engine 側は ID と整数だけ見る)。
// バッファは key (ストアのハンドル) ごとに GPU 側を遅延作成し、CPU ミラーは呼び出し側が持つ。
// 読み戻しは OcclusionCuller::PollStats と同じ流儀: 3 本のステージングへ写し、DO_NOT_WAIT で拾う。
// ★読み取り専用の params は SRV のみで作る (SRV+UAV の 4 バイト x 1 個は WARP の debug layer が落ちた)
class FxGpuBackend {
public:
    struct BufferIo {
        std::string_view name;  // HLSL の変数名 (リフレクションの Name で引く)
        uint64_t key = 0;
        uint8_t* cpu = nullptr; // CPU ミラー (count * stride バイト)
        uint32_t count = 0;
        uint32_t stride = 0;
        bool upload = false;    // ミラーが新しい。dispatch の前に GPU へ写す
    };

    struct Request {
        std::string_view kernel;               // "Foo" → Foo.kernel.hlsl
        const void* params = nullptr;
        uint32_t paramsSize = 0;
        uint32_t elementCount = 0;
        std::vector<BufferIo> readOnly;        // 宣言順の RO (t1 から)
        std::vector<BufferIo> readWrite;       // 宣言順の RW (u0 から)
    };

    static constexpr uint32_t kMaxReadWrite = 8;   // cs_5_0 の UAV スロット
    static constexpr uint32_t kMaxReadOnly = 32;
    static constexpr uint32_t kStagingRing = 3;
    static constexpr uint32_t kMinGpuElements = 64; // numthreads と同じ。極小バッファを避ける

    void Bind(GraphicsDevice* device, ShaderManager* shaders);
    // デバイスがあり、ReleaseGpu 中ではない
    bool Available() const;

    // 成功したら serial (1 から単調増加) を返す。0 = 失敗 (バッファは壊さない)
    uint64_t Dispatch(const Request& req, uint64_t* uploadedBytes);

    // GPU が終えた最後の serial。Pending / Done の判定に使う (待たない)
    uint64_t Completed();
    // serial まで待つ (明示ストール)。待てたら true。デバイス消失は完了扱い
    bool WaitSerial(uint64_t serial);
    uint64_t Submitted() const { return submitted_; }

    // key の未反映の読み戻しがあるか (= CPU ミラーが GPU より古い)
    bool HasUnapplied(uint64_t key) const;
    // 完了済みで最新の読み戻しを cpu へ写す。wait=true なら最新のものを待つ。反映したら true
    bool Poll(uint64_t key, uint8_t* cpu, uint32_t count, uint32_t stride, bool wait);
    // 未反映の読み戻しを捨てる (CPU 側が書き換えたとき)
    void Discard(uint64_t key);
    void Forget(uint64_t key);

    // デバイス消失 (M88): GPU オブジェクトを全部手放す。未完了の serial は完了扱いにして Pending を残さない
    void ReleaseGpu();
    // 手放し状態を解く。GPU バッファは次の Dispatch で CPU ミラーから作り直す
    void RecreateGpu();

    uint32_t GpuBufferCount() const { return static_cast<uint32_t>(bufs_.size()); }

private:
    struct GpuBuf {
        Microsoft::WRL::ComPtr<ID3D11Buffer> buf;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
        Microsoft::WRL::ComPtr<ID3D11Buffer> staging[kStagingRing];
        uint64_t stagingSerial[kStagingRing] = {};
        uint32_t writeIndex = 0;
        uint64_t appliedSerial = 0;
        uint32_t count = 0;
        uint32_t stride = 0;
        uint32_t paddedCount = 0;
    };
    struct Pending {
        uint64_t serial = 0;
        Microsoft::WRL::ComPtr<ID3D11Query> query;
    };
    bool EnsureGpuBuffer(const BufferIo& io, GpuBuf*& out, uint64_t* uploadedBytes);
    // リフレクションで見つかった資源の束縛先。FXC は未使用の資源を落として詰めるので、位置ではなく名前で引く
    struct Binding {
        UINT countBindPoint = 0;
        std::vector<std::pair<UINT, int>> srv; // (BindPoint, RO の添字。-1 = params)
        std::vector<std::pair<UINT, int>> uav; // (BindPoint, RW の添字)
    };
    // 名前で req の引数に対応づけ、stride を照合する。未使用で落ちた引数は束縛を飛ばす
    bool Reflect(const void* bytecode, size_t size, const Request& req, Binding& out);

    GraphicsDevice* device_ = nullptr;
    ShaderManager* shaders_ = nullptr;
    bool released_ = false;
    uint64_t submitted_ = 0;
    uint64_t completed_ = 0;
    std::unordered_map<uint64_t, GpuBuf> bufs_;
    std::deque<Pending> pending_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> countCb_;
};

} // namespace compute
} // namespace mye
