// 自己テスト用カーネル: params と途中の RO (skipped) を使わない。FXC がリソースを落として詰めても、
// エンジンが名前でバインドできることを確かめる。out[i] = a[i] + c[i]
#include "kernel_common.hlsli"

KERNEL_PARAMS(SelfTestUnusedParams)
{
    float unused;
};

KERNEL(SelfTestUnused, SelfTestUnusedParams, RW(float, outv) RO(float, a) RO(float, skipped) RO(float, c))
{
    outv[id] = a[id] + c[id];
}
