#pragma once

namespace mye {

// GPU メモリ統計 (M102) のヘッドレス回帰テスト: テクスチャのバイト数の見積もり
//   - ミップ無し / 全段ミップ / 配列・キューブ / 3D / ブロック圧縮 (4x4 への切り上げ)
// デバイスを作らない。リソースへの登録 (Track) は実機の画面で確かめる
bool RunGpuMemorySelfTest();

} // namespace mye
