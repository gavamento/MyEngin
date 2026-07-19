#pragma once
#include <cstdint>
#include <string>

namespace mye {

// RGBA8 ピクセル列を PNG 保存 (rowPitch はバイト単位)
bool WritePngRGBA(const std::wstring& path, const uint8_t* pixels, int width, int height,
                  int rowPitch);

} // namespace mye
