#pragma once
#include <cstdint>

// GPU メモリの統計 (M102)。D3D の型を含まない読み出し側の窓口 (エディタはこのヘッダだけを使う)。
// 登録側は GpuMemoryTracker.h (Renderer 層の内部)。
//
// 数えているのは「追跡口を通して作ったリソース」だけ: RenderTexture / gpubuf のバッファ /
// TextureLibrary のテクスチャ / MeshLibrary の頂点・インデックス / VolumeTexture。
// 各パスが直接 CreateBuffer / CreateTexture2D した分は入らない。QueryVideoMemory のプロセス全体の
// 使用量との差が、その未追跡分 (とドライバの内部確保) に当たる。
// バイト数は desc から計算した見積もり (ドライバのパディングやアライメントは含まない)
namespace mye {

class GraphicsDevice;

namespace gpumem {

enum class Kind : uint8_t { Texture, RenderTarget, Buffer, Mesh, Volume, Count };
inline constexpr int kKindCount = static_cast<int>(Kind::Count);
inline constexpr int kNameLength = 96;

struct Totals {
    uint64_t bytes[kKindCount] = {};
    uint32_t count[kKindCount] = {};
};
Totals GetTotals();

struct Entry {
    Kind kind = Kind::Texture;
    uint64_t bytes = 0;
    char name[kNameLength] = {};
};
// 生存中のリソースを大きい順に最大 maxEntries 件 out へ写す。戻り値 = 書いた件数
int CopyLargest(Entry* out, int maxEntries);

// テクスチャ 1 枚のバイト数。mipLevels 段ぶんを足す (各段は幅・高さ・奥行きを半分にしていく)。
// blockCompressed のときは 4x4 ブロック単位で数え、bitsPerPixel はブロックあたりのビット数 / 16。
// arraySize はテクスチャ配列・キューブ (6) の枚数
uint64_t TextureBytes(uint32_t width, uint32_t height, uint32_t depth, uint32_t arraySize, uint32_t mipLevels,
                      uint32_t bitsPerPixel, bool blockCompressed);

// プロセス全体のビデオメモリ (DXGI 1.4 の QueryVideoMemoryInfo、ローカルセグメント)。
// 取れない環境 (古い OS / アダプタ) では valid = false
struct VideoMemory {
    bool valid = false;
    uint64_t usageBytes = 0;
    uint64_t budgetBytes = 0;
};
VideoMemory QueryVideoMemory(const GraphicsDevice& device);

} // namespace gpumem
} // namespace mye
