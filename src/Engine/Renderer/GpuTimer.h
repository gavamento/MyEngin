#pragma once
#include <d3d11.h>
#include <wrl/client.h>

namespace mye {

class GraphicsDevice;

// GPU 時間計測 (TIMESTAMP クエリペア + DISJOINT)。
// 読み出しレイテンシ吸収のため 3 フレームのリングで運用する (パイプラインストールなし)
class GpuTimer {
public:
    bool Init(GraphicsDevice& device);
    void Begin(GraphicsDevice& device);
    void End(GraphicsDevice& device);

    // 直近の完了フレームの計測値 (ms)。まだ無ければ 0
    float Milliseconds() const { return lastMs_; }

private:
    static constexpr int kFrames = 6; // tick 毎計測 (フレームあたり複数 tick) に耐える深さ
    struct Frame {
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
        Microsoft::WRL::ComPtr<ID3D11Query> begin;
        Microsoft::WRL::ComPtr<ID3D11Query> end;
        bool pending = false;
    };
    Frame frames_[kFrames];
    int current_ = 0;
    bool skip_ = false;
    float lastMs_ = 0.0f;
};

} // namespace mye
