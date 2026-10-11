//====================================================================================
//                          ScriptCompute.h
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          Compute.ForEach の C++ 糖衣 (バッファ・ForEach・ジョブ)
//====================================================================================
#pragma once
// ScriptAPI.h の末尾から include される。GameLogic.dll 側で完結し、境界は ABI v29 の Compute* スロットだけを越える。
// 型の対応表: 1 要素の C++ の型 T は kernels の HLSL struct と同じレイアウト (4 バイト詰め)。
// カーネルの struct は Generated/Kernels.gen.h の MyeKernels::<Name>:: から使える。
//
//   COMPUTE_KERNELS(Swarm, "ParticleUpdate");
//   auto buf = Compute::CreateBuffer<Particle>(ctx, count, BufferKind::State);
//   buf.Write(ctx, particles);
//   Compute::Job job = Compute::ForEach(ctx, "ParticleUpdate", Execution::Auto, params, buf, obstacles);
//   job.Wait(ctx);
//   buf.Read(ctx, particles);
//   Compute::ForEach(ctx, particles, "ParticleUpdate", Execution::Auto, params);   // 配列を直接渡す (一時の State バッファ)
//
// ★State は常に CPU で計算する決定的なバッファで、スナップショットとワールドハッシュに入る (FIELD にハンドルを持ってよい)。
// ★Fx は GPU で計算しうる演出用のバッファ。Fx のジョブハンドル / Status / Target / Read の結果 / バッファのハンドルは
//   実行先 (CPU か GPU か) で値が変わりうるので、FIELD に保存したり sim の分岐 (バッファの作成・解放を含む) に使ってはいけない。
//   Fx の結果は DebugDrawLine や NoHash の描画コンポーネントへ書く用途に限る。
// ★ForEach できるのは COMPUTE_KERNELS で宣言したカーネルだけ。宣言のないカーネルは何も実行せずに失敗 (Job の handle が 0)。
#include <stddef.h>
#include <stdint.h>
#include <type_traits>
#include <vector>

#include "Shared/EngineAPI.h"

// バッファの種別 (ABI の kind と同値)
enum class BufferKind : uint32_t { State = 0, Fx = 1 };
// 実行先の指定 (ABI の execution と同値)。Auto は State なら CPU、Fx なら GPU を優先して決まる
enum class Execution : uint32_t { Auto = 0, Cpu = 1, Gpu = 2 };

namespace Compute {

enum class JobStatus : int32_t { Done = 0, Pending = 1, Failed = 2, Skipped = 3 };
enum class JobTarget : int32_t { None = 0, Cpu = 1, Gpu = 2 };

// ForEach の結果。handle == 0 は失敗。同期で終わった CPU 実行は定数、GPU の非同期ジョブだけが連番
struct Job {
    uint64_t handle = 0;

    bool Ok() const { return handle != 0; }
    JobStatus Status(const MyeUpdateContext& ctx) const
    {
        return static_cast<JobStatus>(ctx.api->ComputeJobStatus(ctx.api->engine, handle));
    }
    JobTarget Target(const MyeUpdateContext& ctx) const
    {
        return static_cast<JobTarget>(ctx.api->ComputeJobTarget(ctx.api->engine, handle));
    }
    // GPU のジョブの完了まで待つ (明示的なストール)。失敗したジョブは false
    bool Wait(const MyeUpdateContext& ctx) const { return ctx.api->ComputeJobWait(ctx.api->engine, handle) != 0; }
};

// 要素 T のバッファ 1 本。値としてコピーしてよい (ハンドルと要素数だけを持つ)。FIELD に持つなら Handle() の uint64_t を保存し、
// FromHandle で戻す
template <typename T>
class Buffer {
    static_assert(std::is_trivially_copyable_v<T>, "buffer elements must be trivially copyable");
    static_assert(sizeof(T) % 4 == 0 && sizeof(T) <= 2048, "buffer element size must be a multiple of 4 and <= 2048");

public:
    Buffer() = default;
    static Buffer FromHandle(uint64_t handle, uint32_t count)
    {
        Buffer b;
        b.handle_ = handle;
        b.count_ = count;
        return b;
    }

    bool Valid() const { return handle_ != 0; }
    uint64_t Handle() const { return handle_; }
    uint32_t Count() const { return count_; }

    // 先頭の count 要素を書く (count がバッファの要素数を超えると何も書かずに false)
    bool Write(const MyeUpdateContext& ctx, const T* src, uint32_t count) const
    {
        return ctx.api->ComputeWriteBuffer(ctx.api->engine, handle_, src, count) != 0;
    }
    bool Write(const MyeUpdateContext& ctx, const std::vector<T>& src) const
    {
        return Write(ctx, src.data(), static_cast<uint32_t>(src.size()));
    }
    // 先頭の count 要素を読む。Fx は最新の完了済みの結果 (wait = true で GPU の完了を待つ)
    bool Read(const MyeUpdateContext& ctx, T* dst, uint32_t count, bool wait = false) const
    {
        return ctx.api->ComputeReadBuffer(ctx.api->engine, handle_, dst, count, wait ? 1 : 0) != 0;
    }
    bool Read(const MyeUpdateContext& ctx, std::vector<T>& dst, bool wait = false) const
    {
        return Read(ctx, dst.data(), static_cast<uint32_t>(dst.size()), wait);
    }
    // 解放して無効にする。持ち主のエンティティが破棄されたときも tick 末に自動で解放される
    void Release(const MyeUpdateContext& ctx)
    {
        if (handle_ != 0) {
            ctx.api->ComputeReleaseBuffer(ctx.api->engine, handle_);
        }
        handle_ = 0;
        count_ = 0;
    }

private:
    uint64_t handle_ = 0;
    uint32_t count_ = 0;
};

// 要素 count 個のバッファを作る。持ち主は呼び出し元のスクリプトのエンティティ。失敗は Valid() == false
template <typename T>
Buffer<T> CreateBuffer(const MyeUpdateContext& ctx, uint32_t count, BufferKind kind)
{
    const uint64_t handle = ctx.api->ComputeCreateBuffer(ctx.api->engine, ctx.self, count, static_cast<uint32_t>(sizeof(T)),
                                                         static_cast<uint32_t>(kind));
    return handle != 0 ? Buffer<T>::FromHandle(handle, count) : Buffer<T>();
}

// kernel を buffers[0] の要素数ぶん実行する。buffers は宣言順 (RW / RO 混在)、params は P の先頭から sizeof(P) バイト
template <typename P, typename... Bufs>
Job ForEach(const MyeUpdateContext& ctx, const char* kernel, Execution execution, const P& params, const Bufs&... buffers)
{
    static_assert(std::is_trivially_copyable_v<P>, "kernel params must be trivially copyable");
    static_assert(sizeof...(Bufs) > 0, "ForEach needs at least one buffer");
    const uint64_t handles[] = { buffers.Handle()... };
    return Job{ ctx.api->ComputeForEach(ctx.api->engine, kernel, static_cast<uint32_t>(execution), &params,
                                         static_cast<uint32_t>(sizeof(P)), handles,
                                         static_cast<uint32_t>(sizeof...(Bufs)), nullptr) };
}

// 配列を直接渡す糖衣。一時の State バッファへ写して実行し、成功したら結果を書き戻す (同期で完了する)。
// extra は 2 本目以降のバッファ。失敗したときは data に触らない
template <typename T, typename P, typename... Bufs>
Job ForEach(const MyeUpdateContext& ctx, T* data, uint32_t count, const char* kernel, Execution execution, const P& params,
            const Bufs&... extra)
{
    Buffer<T> temp = CreateBuffer<T>(ctx, count, BufferKind::State);
    if (!temp.Valid() || !temp.Write(ctx, data, count)) {
        temp.Release(ctx);
        return Job{};
    }
    const Job job = ForEach(ctx, kernel, execution, params, temp, extra...);
    if (job.Ok()) {
        temp.Read(ctx, data, count);
    }
    temp.Release(ctx);
    return job;
}

template <typename T, typename P, typename... Bufs>
Job ForEach(const MyeUpdateContext& ctx, std::vector<T>& data, const char* kernel, Execution execution, const P& params,
            const Bufs&... extra)
{
    return ForEach(ctx, data.data(), static_cast<uint32_t>(data.size()), kernel, execution, params, extra...);
}

template <typename T, size_t N, typename P, typename... Bufs>
Job ForEach(const MyeUpdateContext& ctx, T (&data)[N], const char* kernel, Execution execution, const P& params,
            const Bufs&... extra)
{
    return ForEach(ctx, data, static_cast<uint32_t>(N), kernel, execution, params, extra...);
}

} // namespace Compute
