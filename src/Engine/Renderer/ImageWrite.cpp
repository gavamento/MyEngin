#include "Engine/Renderer/ImageWrite.h"

#include <cstdio>

#include "stb/stb_image_write.h"

namespace mye {

namespace {
void FileWriteFunc(void* context, void* data, int size)
{
    fwrite(data, 1, static_cast<size_t>(size), static_cast<FILE*>(context));
}
} // namespace

bool WritePngRGBA(const std::wstring& path, const uint8_t* pixels, int width, int height,
                  int rowPitch)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) {
        return false;
    }
    const int ok = stbi_write_png_to_func(&FileWriteFunc, f, width, height, 4, pixels, rowPitch);
    fclose(f);
    return ok != 0;
}

} // namespace mye
