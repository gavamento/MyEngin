#include "Engine/Renderer/GpuTimer.h"

#include "Engine/Renderer/GraphicsDevice.h"

namespace mye {

bool GpuTimer::Init(GraphicsDevice& device)
{
    for (Frame& f : frames_) {
        D3D11_QUERY_DESC qd = {};
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(device.Device()->CreateQuery(&qd, f.disjoint.GetAddressOf()))) {
            return false;
        }
        qd.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(device.Device()->CreateQuery(&qd, f.begin.GetAddressOf()))
            || FAILED(device.Device()->CreateQuery(&qd, f.end.GetAddressOf()))) {
            return false;
        }
    }
    return true;
}

void GpuTimer::Begin(GraphicsDevice& device)
{
    Frame& f = frames_[current_];
    ID3D11DeviceContext* dc = device.Context();

    // このスロットの過去の計測を回収 (kFrames 回前 — 通常は完了済み)
    skip_ = false;
    if (f.pending) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
        UINT64 t0 = 0, t1 = 0;
        if (dc->GetData(f.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK
            && dc->GetData(f.begin.Get(), &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK
            && dc->GetData(f.end.Get(), &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
            if (!dj.Disjoint && dj.Frequency > 0 && t1 >= t0) {
                lastMs_ = static_cast<float>(static_cast<double>(t1 - t0)
                                             / static_cast<double>(dj.Frequency) * 1000.0);
            }
            f.pending = false;
        } else {
            // まだ GPU が終えていない — このスロットの再利用を見送る (計測を 1 回スキップ)
            skip_ = true;
            return;
        }
    }

    dc->Begin(f.disjoint.Get());
    dc->End(f.begin.Get()); // TIMESTAMP は End のみ
}

void GpuTimer::End(GraphicsDevice& device)
{
    if (skip_) {
        return;
    }
    Frame& f = frames_[current_];
    ID3D11DeviceContext* dc = device.Context();
    dc->End(f.end.Get());
    dc->End(f.disjoint.Get());
    f.pending = true;
    current_ = (current_ + 1) % kFrames;
}

} // namespace mye
