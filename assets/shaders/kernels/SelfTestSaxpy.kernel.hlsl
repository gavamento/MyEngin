// 自己テスト用カーネル: y[i] = a * x[i] + y[i]。M91a の GPU/CPU 差の計測に使う最小のカーネル。
#include "kernel_common.hlsli"

KERNEL_PARAMS(SelfTestSaxpyParams)
{
    float a;
};

KERNEL(SelfTestSaxpy, SelfTestSaxpyParams, RW(float, y) RO(float, x))
{
    y[id] = params.a * x[id] + y[id];
}
