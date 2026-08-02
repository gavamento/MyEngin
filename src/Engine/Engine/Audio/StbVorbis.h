#pragma once
// stb_vorbis の設定マクロと宣言の**単一情報源**。
// 実装 TU (StbVorbisImpl.cpp) も利用側 (AudioClip.cpp / 将来のストリーマ) も必ずこれを通すこと。
// マクロが TU 間でズレると宣言と定義が食い違って ODR 違反になる。
//
// stb_vorbis.c の構造 (v1.22):
//   L74-414   宣言部  … STB_VORBIS_INCLUDE_STB_VORBIS_H でガード
//   L420-5541 実装部  … #ifndef STB_VORBIS_HEADER_ONLY でガード
// 実装部にだけ int16/uint32 等の**無プレフィックス typedef** があるので、HEADER_ONLY で
// 取り込む限りエンジン側の名前とは衝突しない。

#define STB_VORBIS_NO_STDIO        // ファイル I/O はエンジン側で行う (fopen 依存を切る)
#define STB_VORBIS_NO_PUSHDATA_API // pulldata (open_memory + seek) しか使わない
#define STB_VORBIS_MAX_CHANNELS 2  // BGM/SE とも最大ステレオ。テーブルを縮める
// NO_PULLDATA_API / NO_INTEGER_CONVERSION は **定義しない**:
//   前者を消すと stb_vorbis_seek (ループ点に必須) が、後者を消すと
//   stb_vorbis_get_samples_short_interleaved (16bit PCM 直取り) が消える。

#define STB_VORBIS_HEADER_ONLY
#pragma warning(push, 0)
#include "stb/stb_vorbis.c"
#pragma warning(pop)
#undef STB_VORBIS_HEADER_ONLY
