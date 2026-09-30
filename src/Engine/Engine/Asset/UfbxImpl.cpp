// ufbx 実装 TU (このファイル以外で ufbx.c を include しないこと)。
// ufbx は単一ファイルの FBX ローダ (cgltf の FBX 版)。cgltf/stb と同じ「external に
// ソース同梱 + src の薄い impl TU でコンパイル」方式で組み込む。
// サードパーティ警告は /W4 で大量に出るため push(0) で抑止する (0 警告方針の維持)。
#pragma warning(push, 0)
#include "ufbx/ufbx.c"
#pragma warning(pop)
