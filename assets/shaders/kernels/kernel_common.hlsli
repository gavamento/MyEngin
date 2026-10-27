// Compute.ForEach のカーネル (*.kernel.hlsl) の共通ヘッダ。GPU (HLSL) と C++ の CPU 版が同じソースを取り込む。
// 書き方の規則: docs/adr (ADR-030) と plans/m91-compute-foreach/spec.md 4.1.1 / 4.1.2。
//
//   KERNEL_PARAMS(PName) { float dt; };
//   KERNEL(Name, PName, RW(T, a) RO(U, b)) { ... id, params, a[id], b[i] ... }
//
// ★RW / RO の間にコンマを書かない (FXC のマクロは可変長引数を持てないので、宣言を並べて 1 引数に束ねる)。
// ★第 1 引数の RW の要素数が id の範囲になる。
// ★オーバーフローしうる整数の計算は uint で書く (符号付きオーバーフローは C++ で未定義)。float -> int は FloatToInt / FloatToUint。
#ifndef MYE_KERNEL_COMMON_HLSLI
#define MYE_KERNEL_COMMON_HLSLI

#ifdef __cplusplus

// ---- C++ (CPU 版) ----
// Shared/KernelCompat.h を、名前空間 mye::kernel::cpu::<Name> に入る前に include しておくこと
#ifndef MYE_KERNEL_COMPAT_INCLUDED
#error "Shared/KernelCompat.h を、カーネルを取り込む名前空間の外で先に include してください"
#endif

#define KERNEL_PARAMS(N) struct N
// [[maybe_unused]]: params や途中の RO を使わないカーネル (SelfTestUnused がその経路を検証する) で C4100 を
// 出さない。CI は /p:MyeWarnAsError=true で警告をエラーにする
#define RW(T, n) , [[maybe_unused]] ::mye::kernel::RWView<T> n
#define RO(T, n) , [[maybe_unused]] ::mye::kernel::ROView<T> n
// Name_KernelName: 登録用メタ (名前)。Name_Cpu: 要素 1 個ぶんの本体 (実行は Engine/Compute/KernelRunner.h)
#define KERNEL(Name, P, Decls)                                                        \
    inline constexpr const char* Name##_KernelName = #Name;                           \
    inline void Name##_Cpu(::mye::kernel::KernelId id, [[maybe_unused]] const P& params Decls)

#else

// ---- HLSL (GPU 版) ----
typedef uint KernelId;

// 乱数代わりの整数ハッシュ。KernelCompat.h の HashU32 / HashToUnitFloat と同じ式
uint HashU32(uint v)
{
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
float HashToUnitFloat(uint h)
{
    return (float)(h >> 8u) * (1.0f / 16777216.0f);
}

// float -> int / uint。KernelCompat.h の FloatToInt / FloatToUint と同じ式 (NaN は 0、範囲外は飽和)
int FloatToInt(float x)
{
    if (x != x)
    {
        return 0;
    }
    if (x < -2147483648.0f)
    {
        x = -2147483648.0f;
    }
    if (x > 2147483520.0f)
    {
        x = 2147483520.0f;
    }
    return (int)x;
}
uint FloatToUint(float x)
{
    if (!(x > 0.0f))
    {
        return 0u;
    }
    if (x > 4294967040.0f)
    {
        x = 4294967040.0f;
    }
    return (uint)x;
}

#define params (__kernel_params[0])
#define KERNEL_PARAMS(N) struct N
// レジスタは宣言順に自動割り当て (params = t0、RO は t1 から、RW は u0 から)。
// エンジンは名前ではなくリフレクションの BindPoint で束縛する
#define RW(T, n) RWStructuredBuffer<T> n;
#define RO(T, n) StructuredBuffer<T> n;
#define KERNEL(Name, P, Decls)                                                        \
    StructuredBuffer<P> __kernel_params;                                              \
    cbuffer __KernelCount { uint __kernel_count; };                                   \
    Decls                                                                             \
    void Name##_Body(KernelId id);                                                    \
    [numthreads(64, 1, 1)] void CSMain(uint3 tid : SV_DispatchThreadID)               \
    {                                                                                 \
        if (tid.x >= __kernel_count) { return; }                                      \
        Name##_Body(tid.x);                                                           \
    }                                                                                 \
    void Name##_Body(KernelId id)

#endif // __cplusplus

#endif // MYE_KERNEL_COMMON_HLSLI
