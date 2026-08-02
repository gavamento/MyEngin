// stb_vorbis 実装 TU (このファイル以外で「実装部の」stb_vorbis.c を include しないこと)。
// ufbx / cgltf / stb_image と同じ「external にソース同梱 + src の薄い impl TU」方式。
//
// ★StbImpl.cpp に混ぜてはいけない: stb_vorbis.c の実装部はファイルスコープで
//   int8/uint8/int16/uint16/int32/uint32/uint という**無プレフィックスの typedef** を撒く。
//   同一 TU に他のコードを同居させると容易に衝突するので専用 TU に隔離する。
//
// ★alloca について: stb_vorbis はフレーム毎の一時領域を既定で alloca で取る
//   (stb_vorbis.c:930 の temp_alloc)。これを無効化する **コンパイル時マクロは存在しない** —
//   stb_vorbis_open_memory に非 NULL の stb_vorbis_alloc (アリーナ) を渡した時だけ
//   setup_temp_malloc 経路に切り替わる。ストリーミングをワーカースレッド (既定 1MB スタック)
//   で回すときは必ずアリーナを渡すこと。

#include "Engine/Engine/Audio/StbVorbis.h" // 設定マクロ + 宣言 (HEADER_ONLY)

// サードパーティ警告は /W4 で大量に出るため push(0) で抑止する (0 警告方針の維持)。
#pragma warning(push, 0)
// push(0) が黙らせるのはパーサ由来の警告だけ。C4701/C4702 系は**コード生成器**が出すので
// 明示 disable が要る (実測: Debug で stb_vorbis.c:4758 の C4701 が push(0) を貫通した)。
#pragma warning(disable : 4701) // 初期化されていない可能性のあるローカル変数
#pragma warning(disable : 4703) // 初期化されていない可能性のあるローカル ポインタ変数
#pragma warning(disable : 4702) // 到達できないコード (最適化ビルドで出る)
#include "stb/stb_vorbis.c"     // HEADER_ONLY が外れているので実装部が展開される
#pragma warning(pop)
