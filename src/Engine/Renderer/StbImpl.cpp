// stb 実装 TU (このファイル以外で *_IMPLEMENTATION を定義しないこと)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#include "stb/stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

// BCn ブロック圧縮 (M24: PNG→BC1/BC3 クック用)。
// stb_dxt 実装は /W4 で C4244 (int→uchar) を出すためサードパーティ警告を抑止する
#define STB_DXT_IMPLEMENTATION
#pragma warning(push, 0)
#include "stb/stb_dxt.h"
#pragma warning(pop)
