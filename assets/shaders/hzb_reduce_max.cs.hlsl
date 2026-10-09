// GPU オクルージョン用の max-Z 縮小。hzb_reduce.cs.hlsl を max 演算で使うだけ
// (分割規則とスレッドグループ辺長の正本はそちら)。
#define MYE_HZB_MAX 1
#include "hzb_reduce.cs.hlsl"
