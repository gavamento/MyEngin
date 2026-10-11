//====================================================================================
//                          KernelSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          カーネル基盤 (互換層・実行器・ハッシュ・GPU/CPU 差) の回帰テスト
//====================================================================================
#include "Engine/Engine/Compute/KernelSelfTest.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Engine/Compute/KernelHash.h"
#include "Engine/Engine/Compute/KernelRunner.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/Device/GpuBufferUtil.h"
#include "Engine/Renderer/Device/GraphicsDevice.h"
#include "Engine/Renderer/Shader/ShaderManager.h"
#include "Shared/KernelCompat.h"

// カーネルのソースをそのまま C++ として取り込む (正本は assets/shaders/kernels/)。
// 名前空間を mye::kernel の内側に置く理由は Shared/KernelCompat.h の冒頭を参照
namespace mye::kernel::cpu::SelfTestSaxpy {
#include "../../../../assets/shaders/kernels/SelfTestSaxpy.kernel.hlsl"
}
namespace mye::kernel::cpu::SelfTestMix {
#include "../../../../assets/shaders/kernels/SelfTestMix.kernel.hlsl"
}

namespace mye {
namespace {

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace sx = kernel::cpu::SelfTestSaxpy;
namespace mx = kernel::cpu::SelfTestMix;

int g_fails = 0;

void Check(bool cond, const char* msg)
{
    if (cond) {
        MYE_LOG_INFO("  PASS: %s", msg);
    } else {
        MYE_LOG_ERROR("  FAIL: %s", msg);
        ++g_fails;
    }
}

// CPU 版の結果ハッシュの固定値。Debug / Release / jobs の有無 / chunk 数のどれでも同じになる
constexpr uint64_t kSaxpyGolden = 0x1e2749c7308fa99cull;
constexpr uint64_t kMixGolden = 0x938f8aef9ad7c55cull;

// GPU との許容差 (WARP での実測は Saxpy 0 ULP / Mix 6.2e-6。実測の記録: plans/m91-compute-foreach/sub-01.md)
constexpr float kSaxpyMaxUlp = 1.0f;
constexpr float kMixMaxAbs = 1e-4f;

constexpr uint32_t kSaxpyCount = 10007; // 8 の倍数でも 64 の倍数でもない
constexpr uint32_t kMixCount = 4099;
constexpr uint32_t kMixProbeCount = 4000; // kMixCount より短い = 末尾付近の要素が範囲外を読む

// 決定的な入力 (rand を使わない)。[-1, 1)
float InputValue(uint32_t i, uint32_t salt)
{
    return kernel::HashToUnitFloat(kernel::HashU32(i * 2654435761u + salt)) * 2.0f - 1.0f;
}

template <class T> uint64_t HashVector(const std::vector<T>& v)
{
    return HashBytes(v.data(), v.size() * sizeof(T));
}

// ---- 入力の生成 ----

struct SaxpyData {
    std::vector<float> x;
    std::vector<float> y0;
};

SaxpyData MakeSaxpyData()
{
    SaxpyData d;
    d.x.resize(kSaxpyCount);
    d.y0.resize(kSaxpyCount);
    for (uint32_t i = 0; i < kSaxpyCount; ++i) {
        d.x[i] = InputValue(i, 1u) * 100.0f;
        d.y0[i] = InputValue(i, 2u) * 10.0f;
    }
    return d;
}

constexpr sx::SelfTestSaxpyParams kSaxpyParams = { 1.7f };

struct MixData {
    std::vector<mx::SelfTestMixProbe> probes;
};

MixData MakeMixData()
{
    MixData d;
    d.probes.resize(kMixProbeCount);
    for (uint32_t i = 0; i < kMixProbeCount; ++i) {
        d.probes[i].dir = kernel::float3(InputValue(i, 3u), InputValue(i, 4u), InputValue(i, 5u));
        d.probes[i].weight = InputValue(i, 6u);
    }
    return d;
}

mx::SelfTestMixParams MakeMixParams()
{
    mx::SelfTestMixParams p;
    p.time = 2.0f;
    p.axis = kernel::float3(0.3f, -0.5f, 0.8f);
    p.seed = 12345u;
    return p;
}

// ---- CPU 実行 ----

std::vector<float> RunSaxpyCpu(jobs::JobSystem& js, const SaxpyData& d, const kernel::RunOptions& opt,
                               kernel::RunStatus* status = nullptr)
{
    std::vector<float> y = d.y0;
    const kernel::RunStatus st = kernel::RunKernelCpu(
        js, &sx::SelfTestSaxpy_Cpu, kSaxpyParams, opt,
        kernel::RWView<float>(y.data(), kSaxpyCount), kernel::ROView<float>(d.x.data(), kSaxpyCount));
    if (status) {
        *status = st;
    }
    return y;
}

std::vector<mx::SelfTestMixElem> RunMixCpu(jobs::JobSystem& js, const MixData& d,
                                           const kernel::RunOptions& opt)
{
    std::vector<mx::SelfTestMixElem> out(kMixCount);
    const mx::SelfTestMixParams params = MakeMixParams();
    kernel::RunKernelCpu(js, &mx::SelfTestMix_Cpu, params, opt,
                         kernel::RWView<mx::SelfTestMixElem>(out.data(), kMixCount),
                         kernel::ROView<mx::SelfTestMixProbe>(d.probes.data(),
                                                              static_cast<uint32_t>(d.probes.size())));
    return out;
}

// ---- GPU 実行 (WARP) ----

struct GpuBuf {
    const char* name; // HLSL の変数名
    UINT stride;
    UINT count;
    const void* init;
    bool rw;
    ComPtr<ID3D11Buffer> buf;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11ShaderResourceView> srv;
};

// prog の CSMain を dispatchCount 要素ぶん走らせ、RW バッファの中身を out へ書き戻す。
// バインドは名前ではなくリフレクションの BindPoint で行う (レジスタは宣言順の自動割り当て)
bool RunKernelGpu(GraphicsDevice& device, ShaderProgram& prog, std::vector<GpuBuf>& bufs,
                  uint32_t dispatchCount, std::vector<std::vector<uint8_t>>& out)
{
    ID3D11Device* dev = device.Device();
    ID3D11DeviceContext* ctx = device.Context();
    // ★読み取り専用バッファは SRV だけにする。params (4 バイト x 1) を gpubuf::CreateStructured
    //   (SRV|UAV 両方) で作ると WARP が落ちた。SRV のみにしたら解消 (原因は未特定)
    for (GpuBuf& b : bufs) {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = b.stride * b.count;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | (b.rw ? D3D11_BIND_UNORDERED_ACCESS : 0u);
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = b.stride;
        const D3D11_SUBRESOURCE_DATA init = { b.init, 0, 0 };
        if (FAILED(dev->CreateBuffer(&bd, &init, b.buf.GetAddressOf()))) {
            return false;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.NumElements = b.count;
        if (FAILED(dev->CreateShaderResourceView(b.buf.Get(), &srvDesc, b.srv.GetAddressOf()))) {
            return false;
        }
        if (b.rw) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format = DXGI_FORMAT_UNKNOWN;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            uavDesc.Buffer.NumElements = b.count;
            if (FAILED(dev->CreateUnorderedAccessView(b.buf.Get(), &uavDesc, b.uav.GetAddressOf()))) {
                return false;
            }
        }
    }
    ComPtr<ID3D11Buffer> countCb;
    if (!gpubuf::CreateConstant(dev, 16, countCb)) {
        return false;
    }
    const uint32_t countData[4] = { dispatchCount, 0, 0, 0 };
    gpubuf::UploadCB(ctx, countCb.Get(), countData);

    ComPtr<ID3D11ShaderReflection> refl;
    if (FAILED(D3DReflect(prog.csBytecode.data(), prog.csBytecode.size(), IID_ID3D11ShaderReflection,
                          reinterpret_cast<void**>(refl.GetAddressOf())))) {
        return false;
    }
    D3D11_SHADER_DESC sd = {};
    refl->GetDesc(&sd);
    ctx->CSSetShader(prog.cs.Get(), nullptr, 0);
    bool bound = true;
    for (UINT i = 0; i < sd.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC bd = {};
        refl->GetResourceBindingDesc(i, &bd);
        if (bd.Type == D3D_SIT_CBUFFER) {
            ID3D11Buffer* cb = countCb.Get();
            ctx->CSSetConstantBuffers(bd.BindPoint, 1, &cb);
            continue;
        }
        GpuBuf* match = nullptr;
        for (GpuBuf& b : bufs) {
            if (std::strcmp(b.name, bd.Name) == 0) {
                match = &b;
            }
        }
        if (!match) {
            MYE_LOG_ERROR("    unknown resource '%s'", bd.Name);
            bound = false;
            continue;
        }
        // 構造化バッファでは NumSamples が要素の大きさ。C++ の sizeof と食い違えば params / 要素の詰め方がずれている
        if (bd.NumSamples != match->stride) {
            MYE_LOG_ERROR("    stride mismatch '%s': HLSL %u / C++ %u", bd.Name, bd.NumSamples,
                          match->stride);
            bound = false;
        }
        if (bd.Type == D3D_SIT_UAV_RWSTRUCTURED) {
            ID3D11UnorderedAccessView* uav = match->uav.Get();
            ctx->CSSetUnorderedAccessViews(bd.BindPoint, 1, &uav, nullptr);
        } else if (bd.Type == D3D_SIT_STRUCTURED) {
            ID3D11ShaderResourceView* srv = match->srv.Get();
            ctx->CSSetShaderResources(bd.BindPoint, 1, &srv);
        } else {
            bound = false;
        }
    }
    ctx->Dispatch((dispatchCount + 63u) / 64u, 1, 1);

    ID3D11UnorderedAccessView* nullUav[8] = {};
    ID3D11ShaderResourceView* nullSrv[8] = {};
    ctx->CSSetUnorderedAccessViews(0, 8, nullUav, nullptr);
    ctx->CSSetShaderResources(0, 8, nullSrv);

    out.assign(bufs.size(), {});
    for (size_t i = 0; i < bufs.size(); ++i) {
        if (!bufs[i].rw) {
            continue;
        }
        D3D11_BUFFER_DESC desc = {};
        bufs[i].buf->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> staging;
        if (FAILED(dev->CreateBuffer(&desc, nullptr, staging.GetAddressOf()))) {
            return false;
        }
        ctx->CopyResource(staging.Get(), bufs[i].buf.Get());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            return false;
        }
        const auto* p = static_cast<const uint8_t*>(mapped.pData);
        out[i].assign(p, p + static_cast<size_t>(bufs[i].stride) * bufs[i].count);
        ctx->Unmap(staging.Get(), 0);
    }
    return bound;
}

// float の ULP 距離 (符号付きの並びを整数に写して引く)
int64_t UlpDistance(float a, float b)
{
    auto key = [](float f) -> int64_t {
        int32_t i;
        std::memcpy(&i, &f, sizeof(i));
        return i < 0 ? static_cast<int64_t>(INT32_MIN) - i : i;
    };
    const int64_t d = key(a) - key(b);
    return d < 0 ? -d : d;
}

// ---- 各テスト ----

// テンプレート引数を介さないと requires が硬いエラーになる (テンプレート外の式は SFINAE されない)
template <class V, class I> concept CanIndex = requires(V v, I i) { v[i]; };

void TestCompatTypes()
{
    MYE_LOG_INFO("-- 互換型のレイアウト・添字の型 --");
    static_assert(sizeof(kernel::float2) == 8 && sizeof(kernel::float3) == 12
                  && sizeof(kernel::float4) == 16 && sizeof(kernel::uint3) == 12);
    static_assert(sizeof(mx::SelfTestMixElem) == 20 && sizeof(mx::SelfTestMixProbe) == 16
                  && sizeof(mx::SelfTestMixParams) == 20);
    static_assert(std::is_trivially_copyable_v<mx::SelfTestMixElem>);
    // RW は KernelId でしか添字にできない (id + 1 の結果 = uint や int リテラルは不可)
    static_assert(CanIndex<kernel::RWView<float>, kernel::KernelId>);
    static_assert(!CanIndex<kernel::RWView<float>, kernel::uint>);
    static_assert(!CanIndex<kernel::RWView<float>, int>);
    static_assert(CanIndex<kernel::ROView<float>, kernel::uint>);
    Check(true, "static_assert: レイアウトと RW の添字は KernelId 専用");

    // RO の範囲外は 0 初期化の T
    const float data[3] = { 1.0f, 2.0f, 3.0f };
    const kernel::ROView<float> ro(data, 3);
    Check(ro[2] == 3.0f && ro[3] == 0.0f && ro[100000] == 0.0f && ro[static_cast<kernel::uint>(-1)] == 0.0f,
          "RO: 範囲外の読みは 0");
    const mx::SelfTestMixProbe probes[1] = { { kernel::float3(1.0f, 2.0f, 3.0f), 4.0f } };
    const kernel::ROView<mx::SelfTestMixProbe> roStruct(probes, 1);
    const mx::SelfTestMixProbe oob = roStruct[1];
    Check(oob.dir.x == 0.0f && oob.dir.y == 0.0f && oob.dir.z == 0.0f && oob.weight == 0.0f,
          "RO: 範囲外の構造体は全成分 0");
    kernel::uint n = 0;
    kernel::uint stride = 0;
    roStruct.GetDimensions(n, stride);
    Check(n == 1 && stride == sizeof(mx::SelfTestMixProbe), "GetDimensions");
}

void TestHashVectors()
{
    MYE_LOG_INFO("-- ソースハッシュ (BOM / CRLF 正規化) --");
    using kernel::HashKernelSource;
    const std::string lf = "KERNEL(A)\n{\n    x;\n}\n";
    std::string crlf;
    for (char c : lf) {
        if (c == '\n') {
            crlf += '\r';
        }
        crlf += c;
    }
    const std::string bomLf = "\xEF\xBB\xBF" + lf;
    const std::string bomCrlf = "\xEF\xBB\xBF" + crlf;
    const uint64_t h = HashKernelSource(lf);
    Check(h == HashStr(lf), "LF 版 = 素の FNV-1a 64");
    Check(HashKernelSource(crlf) == h, "CRLF 版は LF 版と同値");
    Check(HashKernelSource(bomLf) == h && HashKernelSource(bomCrlf) == h, "BOM 付きも同値");
    Check(HashKernelSource("a\rb") != HashKernelSource("a\nb") && HashKernelSource("a\rb") == HashStr("a\rb"),
          "単独の CR は取り除かない");
    Check(HashKernelSource("a\r\r\nb") == HashStr("a\r\nb"), "CRCRLF は直前の CR だけ残る");
    Check(HashKernelSource("") == kFnvOffset, "空文字列は offset basis");
    // 固定値: PowerShell の codegen / C# 変換器と突き合わせるための参照値
    Check(HashKernelSource("a") == 0xaf63dc4c8601ec8cull, "固定値: \"a\" = af63dc4c8601ec8c");
    Check(HashKernelSource("\xEF\xBB\xBF" "a\r\n") == HashKernelSource("a\n"), "BOM + CRLF の短い入力");

    // 実ファイル: 改行コードと BOM を変えても同じ値
    const std::wstring dir = FindEngineShaderDir();
    std::ifstream in(fs::path(dir) / L"kernels" / L"SelfTestSaxpy.kernel.hlsl", std::ios::binary);
    std::string file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Check(!file.empty(), "SelfTestSaxpy.kernel.hlsl を読めた");
    std::string normalized;
    for (size_t i = 0; i < file.size(); ++i) {
        if (!(file[i] == '\r' && i + 1 < file.size() && file[i + 1] == '\n')) {
            normalized += file[i];
        }
    }
    std::string asCrlf;
    for (char c : normalized) {
        if (c == '\n') {
            asCrlf += '\r';
        }
        asCrlf += c;
    }
    Check(HashKernelSource(file) == HashKernelSource("\xEF\xBB\xBF" + asCrlf)
              && HashKernelSource(file) == HashKernelSource(normalized),
          "実ファイル: LF / CRLF / BOM 付き CRLF で同値");
}

void TestCpuDeterminism(jobs::JobSystem& js)
{
    MYE_LOG_INFO("-- CPU 版の結果ハッシュ (jobs / chunk 数) --");
    const SaxpyData sd = MakeSaxpyData();
    const MixData md = MakeMixData();

    struct Config {
        const char* label;
        bool jobs;
        size_t maxChunks;
        size_t minBlocks;
    };
    const Config configs[] = {
        { "jobs on, 既定", true, 0, 16 },
        { "jobs off", false, 0, 16 },
        { "jobs on, maxChunks=1", true, 1, 16 },
        { "jobs on, maxChunks=3", true, 3, 16 },
        { "jobs on, 1 ブロックずつ", true, 0, 1 },
        { "jobs off, 1 ブロックずつ", false, 0, 1 },
    };
    uint64_t saxpyFirst = 0;
    uint64_t mixFirst = 0;
    bool saxpyAll = true;
    bool mixAll = true;
    bool first = true;
    for (const Config& c : configs) {
        js.SetEnabled(c.jobs);
        kernel::RunOptions opt;
        opt.maxChunks = c.maxChunks;
        opt.minBlocksPerChunk = c.minBlocks;
        const uint64_t hs = HashVector(RunSaxpyCpu(js, sd, opt));
        const uint64_t hm = HashVector(RunMixCpu(js, md, opt));
        MYE_LOG_INFO("    %-26s saxpy=%016llx mix=%016llx", c.label, static_cast<unsigned long long>(hs),
                     static_cast<unsigned long long>(hm));
        if (first) {
            saxpyFirst = hs;
            mixFirst = hm;
            first = false;
        }
        saxpyAll = saxpyAll && hs == saxpyFirst;
        mixAll = mixAll && hm == mixFirst;
    }
    js.SetEnabled(true);
    Check(saxpyAll, "Saxpy: 全構成で同じハッシュ");
    Check(mixAll, "Mix: 全構成で同じハッシュ");
    Check(saxpyFirst == kSaxpyGolden, "Saxpy: 固定値と一致");
    Check(mixFirst == kMixGolden, "Mix: 固定値と一致");

    // 実際に複数 chunk に割れていること (割れていなければ上の一致は何も証明しない)
    if (js.WorkerCount() > 0) {
        const jobs::JobSystem::Stats before = js.GetStats();
        kernel::RunOptions opt;
        opt.minBlocksPerChunk = 1;
        RunSaxpyCpu(js, sd, opt);
        const jobs::JobSystem::Stats after = js.GetStats();
        Check(after.parallelBatches > before.parallelBatches, "ワーカーへ配られた (並列実行が実際に走った)");
    } else {
        MYE_LOG_INFO("  (ワーカー 0: 並列実行の確認は省略)");
    }
}

void TestRunnerGuards(jobs::JobSystem& js)
{
    MYE_LOG_INFO("-- 実行器のガード --");
    const SaxpyData sd = MakeSaxpyData();

    // ジョブの中から呼ぶと拒否される (jobs 有効・無効のどちらでも)
    for (const bool enabled : { true, false }) {
        js.SetEnabled(enabled);
        kernel::RunStatus inner = kernel::RunStatus::Ok;
        std::vector<float> y = sd.y0;
        js.ParallelRanges(64, 1, [&](size_t begin, size_t) {
            if (begin == 0) {
                inner = RunKernelCpu(
                    js, &sx::SelfTestSaxpy_Cpu, kSaxpyParams, kernel::RunOptions{},
                    kernel::RWView<float>(y.data(), kSaxpyCount),
                    kernel::ROView<float>(sd.x.data(), kSaxpyCount));
            }
        });
        Check(inner == kernel::RunStatus::InJob, enabled ? "ジョブの中からは InJob (jobs on)" : "ジョブの中からは InJob (jobs off)");
        Check(y == sd.y0, "拒否されたときバッファは変わらない");
    }
    js.SetEnabled(true);
    Check(!jobs::JobSystem::InJob(), "ジョブの外では InJob が false に戻る");

    kernel::RunStatus st = kernel::RunStatus::BadBuffer;
    RunSaxpyCpu(js, sd, kernel::RunOptions{}, &st);
    Check(st == kernel::RunStatus::Ok, "ジョブの外では Ok");

    // 要素数の上限 (実行はされない)
    float dummy = 0.0f;
    const kernel::RunStatus tooMany = RunKernelCpu(
        js, &sx::SelfTestSaxpy_Cpu, kSaxpyParams, kernel::RunOptions{},
        kernel::RWView<float>(&dummy, static_cast<uint32_t>(kernel::kMaxKernelElements + 1)),
        kernel::ROView<float>(&dummy, 1));
    Check(tooMany == kernel::RunStatus::TooManyElements, "上限超過は TooManyElements");

    // 2 本目の RW が短いと範囲外書き込みになるので拒否する
    struct Local {
        static void TwoRw(kernel::KernelId id, const int&, kernel::RWView<float> a, kernel::RWView<float> b)
        {
            a[id] = 1.0f;
            b[id] = 2.0f;
        }
    };
    std::vector<float> a(32, 0.0f);
    std::vector<float> b(16, 0.0f);
    const int unused = 0;
    const kernel::RunStatus bad = RunKernelCpu(js, &Local::TwoRw, unused, kernel::RunOptions{},
                                               kernel::RWView<float>(a.data(), 32), kernel::RWView<float>(b.data(), 16));
    Check(bad == kernel::RunStatus::BadBuffer && a[0] == 0.0f, "短い RW は BadBuffer で何も書かない");

    // 要素 0 個は何もしない
    const kernel::RunStatus empty = RunKernelCpu(js, &sx::SelfTestSaxpy_Cpu, kSaxpyParams, kernel::RunOptions{},
                                                 kernel::RWView<float>(&dummy, 0), kernel::ROView<float>(&dummy, 0));
    Check(empty == kernel::RunStatus::Ok && dummy == 0.0f, "要素 0 個は Ok で何もしない");
}

void TestRoOutOfRangeInKernel(jobs::JobSystem& js)
{
    MYE_LOG_INFO("-- カーネルの中の RO 範囲外 --");
    // x を y より短くすると、末尾の x[id] は 0 になり y[id] = a * 0 + y[id] = y[id] のまま
    const SaxpyData sd = MakeSaxpyData();
    std::vector<float> y = sd.y0;
    const uint32_t shortCount = kSaxpyCount - 100;
    RunKernelCpu(js, &sx::SelfTestSaxpy_Cpu, kSaxpyParams, kernel::RunOptions{},
                 kernel::RWView<float>(y.data(), kSaxpyCount), kernel::ROView<float>(sd.x.data(), shortCount));
    bool tailUnchanged = true;
    bool headChanged = false;
    for (uint32_t i = 0; i < kSaxpyCount; ++i) {
        if (i >= shortCount) {
            tailUnchanged = tailUnchanged && y[i] == kSaxpyParams.a * 0.0f + sd.y0[i];
        } else {
            headChanged = headChanged || y[i] != sd.y0[i];
        }
    }
    Check(tailUnchanged && headChanged, "短い RO の末尾を読む要素は 0 を読む");
}

void TestGpuVsCpu(jobs::JobSystem& js)
{
    MYE_LOG_INFO("-- GPU (WARP) と CPU の差 --");
    GraphicsDevice device;
    if (!device.Init(true)) {
        Check(false, "WARP device init");
        return;
    }
    const std::wstring shaderDir = FindEngineShaderDir();
    Check(!shaderDir.empty(), "エンジンのシェーダフォルダが見つかる");
    ShaderManager shaders;
    shaders.SetAssetsRoot(fs::path(shaderDir).parent_path().wstring());
    Check(shaders.Init(device, { shaderDir }), "shader manager init");
    shaders.RebuildProjectShaderIndex();
    const std::vector<std::string> names = shaders.ProjectShaderNames(".kernel");
    Check(std::find(names.begin(), names.end(), "SelfTestSaxpy.kernel") != names.end()
              && std::find(names.begin(), names.end(), "SelfTestMix.kernel") != names.end(),
          "*.kernel.hlsl がプロジェクト索引に載る");

    kernel::RunOptions opt;
    js.SetEnabled(true);

    // ---- Saxpy ----
    {
        ShaderProgram* prog = shaders.Get(shaders.LoadCompute("SelfTestSaxpy.kernel"));
        Check(prog && prog->valid, "SelfTestSaxpy.kernel が FXC (cs_5_0) で通る");
        if (prog && prog->valid) {
            const SaxpyData sd = MakeSaxpyData();
            std::vector<GpuBuf> bufs;
            bufs.push_back({ "__kernel_params", sizeof(sx::SelfTestSaxpyParams), 1, &kSaxpyParams, false, {}, {}, {} });
            bufs.push_back({ "y", sizeof(float), kSaxpyCount, sd.y0.data(), true, {}, {}, {} });
            bufs.push_back({ "x", sizeof(float), kSaxpyCount, sd.x.data(), false, {}, {}, {} });
            std::vector<std::vector<uint8_t>> out;
            Check(RunKernelGpu(device, *prog, bufs, kSaxpyCount, out), "Saxpy: GPU で dispatch できた (stride 一致)");
            const std::vector<float> cpu = RunSaxpyCpu(js, sd, opt);
            if (out.size() == 3 && out[1].size() == cpu.size() * sizeof(float)) {
                std::vector<float> gpu(cpu.size());
                std::memcpy(gpu.data(), out[1].data(), out[1].size());
                int64_t maxUlp = 0;
                float maxAbs = 0.0f;
                size_t differ = 0;
                for (size_t i = 0; i < cpu.size(); ++i) {
                    maxUlp = std::max(maxUlp, UlpDistance(cpu[i], gpu[i]));
                    maxAbs = std::max(maxAbs, std::fabs(cpu[i] - gpu[i]));
                    differ += cpu[i] != gpu[i] ? 1 : 0;
                }
                MYE_LOG_INFO("    Saxpy GPU/CPU: 最大 %lld ULP / 最大絶対誤差 %g / 不一致 %zu / %zu 要素",
                             static_cast<long long>(maxUlp), maxAbs, differ, cpu.size());
                Check(static_cast<float>(maxUlp) <= kSaxpyMaxUlp, "Saxpy: GPU と CPU の差が許容差 (ULP) の中");
            } else {
                Check(false, "Saxpy: 読み戻しのサイズ");
            }
        }
    }

    // ---- Mix ----
    {
        ShaderProgram* prog = shaders.Get(shaders.LoadCompute("SelfTestMix.kernel"));
        Check(prog && prog->valid, "SelfTestMix.kernel が FXC (cs_5_0) で通る");
        if (prog && prog->valid) {
            const MixData md = MakeMixData();
            const mx::SelfTestMixParams params = MakeMixParams();
            std::vector<mx::SelfTestMixElem> zeros(kMixCount);
            std::memset(zeros.data(), 0, zeros.size() * sizeof(zeros[0]));
            std::vector<GpuBuf> bufs;
            bufs.push_back({ "__kernel_params", sizeof(params), 1, &params, false, {}, {}, {} });
            bufs.push_back({ "outv", sizeof(mx::SelfTestMixElem), kMixCount, zeros.data(), true, {}, {}, {} });
            bufs.push_back({ "probes", sizeof(mx::SelfTestMixProbe), kMixProbeCount, md.probes.data(), false, {}, {}, {} });
            std::vector<std::vector<uint8_t>> out;
            Check(RunKernelGpu(device, *prog, bufs, kMixCount, out), "Mix: GPU で dispatch できた (stride 一致)");
            const std::vector<mx::SelfTestMixElem> cpu = RunMixCpu(js, md, opt);
            if (out.size() == 3 && out[1].size() == cpu.size() * sizeof(mx::SelfTestMixElem)) {
                std::vector<mx::SelfTestMixElem> gpu(cpu.size());
                std::memcpy(gpu.data(), out[1].data(), out[1].size());
                float maxAbs = 0.0f;
                int64_t maxUlp = 0;
                size_t tagDiff = 0;
                size_t worst = 0;
                for (size_t i = 0; i < cpu.size(); ++i) {
                    const float a[4] = { cpu[i].pos.x, cpu[i].pos.y, cpu[i].pos.z, cpu[i].val };
                    const float b[4] = { gpu[i].pos.x, gpu[i].pos.y, gpu[i].pos.z, gpu[i].val };
                    for (int k = 0; k < 4; ++k) {
                        const float d = std::fabs(a[k] - b[k]);
                        if (!(d <= maxAbs)) {
                            maxAbs = d;
                            worst = i;
                        }
                        maxUlp = std::max(maxUlp, UlpDistance(a[k], b[k]));
                    }
                    tagDiff += cpu[i].tag != gpu[i].tag ? 1 : 0;
                }
                MYE_LOG_INFO("    Mix GPU/CPU: 最大絶対誤差 %g (要素 %zu) / 最大 %lld ULP / tag 不一致 %zu / %zu 要素",
                             maxAbs, worst, static_cast<long long>(maxUlp), tagDiff, cpu.size());
                Check(maxAbs <= kMixMaxAbs, "Mix: GPU と CPU の差が許容差 (絶対誤差) の中");
                Check(tagDiff == 0, "Mix: 整数ハッシュ (HashU32) は GPU と CPU でビット一致");
            } else {
                Check(false, "Mix: 読み戻しのサイズ");
            }
        }
    }
}

} // namespace

bool RunKernelSelfTest()
{
    MYE_LOG_INFO("==== Kernel (Compute.ForEach 基盤) self test ====");
    g_fails = 0;

    jobs::JobSystem js; // プロセス共有のインスタンスは触らない (--no-jobs の設定を壊さない)
    js.Init();

    TestCompatTypes();
    TestHashVectors();
    TestCpuDeterminism(js);
    TestRunnerGuards(js);
    TestRoOutOfRangeInKernel(js);
    TestGpuVsCpu(js);

    js.Shutdown();
    MYE_LOG_INFO("==== Kernel self test: %s ====", g_fails == 0 ? "PASS" : "FAIL");
    return g_fails == 0;
}

} // namespace mye
