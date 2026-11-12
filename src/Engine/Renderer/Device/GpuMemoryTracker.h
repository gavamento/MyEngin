#pragma once
#include <d3d11.h>

#include "Engine/Renderer/Device/GpuMemoryStats.h"

// GPU メモリの登録側 (Renderer 層の内部。D3D の型を受ける)。読み出しは GpuMemoryStats.h。
//
// 寿命の追跡はリソース自身に任せる: 小さな COM オブジェクトを SetPrivateDataInterface で
// リソースへぶら下げ、D3D がリソースを壊すときにそれを Release する → そこで合計から引く。
// 生成側は「作った直後に Track を 1 回呼ぶ」だけでよく、解放の経路 (ComPtr の Reset、
// デバイス消失からの作り直し、遅延破棄) を 1 つずつ追わなくて済む
namespace mye::gpumem {

// サイズは desc から計算する。name は一覧に出す名前 (写しを持つ)。null のリソースは無視する
void TrackTexture2D(ID3D11Texture2D* texture, Kind kind, const char* name);
void TrackTexture3D(ID3D11Texture3D* texture, Kind kind, const char* name);
void TrackBuffer(ID3D11Buffer* buffer, Kind kind, const char* name);

// Track 済みのリソースの名前を付け直す (作った時点では名前が分からない経路用)。未登録なら何もしない
void Rename(ID3D11DeviceChild* resource, const char* name);

} // namespace mye::gpumem
