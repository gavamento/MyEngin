#pragma once

namespace mye {

// テクスチャクック (M24: PNG→BCn/DDS) のヘッドレス回帰テスト。
// GPU 不要 — CPU 圧縮 + DDS 書き出しヘッダ/mip チェーンの整合を検証する。
bool RunTextureCookSelfTest();

} // namespace mye
