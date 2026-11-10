//====================================================================================
//                          ZipExtract.cpp
//  MyEngine/ 秋田蓮音                                                      11/10/2026
//                                          zip の安全な展開の実装 (miniz)
//====================================================================================
#include "Engine/Platform/ZipExtract.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <vector>

#include "Engine/Platform/PathUtil.h"

// zlib 互換の短い名前 (crc32 / inflate など) を公開させない。miniz.c の項目定義 (Engine.vcxproj) と揃える
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz/miniz.h"

namespace mye {

namespace {

bool IsReservedDeviceName(std::string_view part)
{
    // 拡張子を外した部分で判定する ("nul.txt" も NUL デバイスを開く)
    const size_t dot = part.find('.');
    std::string base(part.substr(0, dot));
    for (char& c : base) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL") {
        return true;
    }
    if (base.size() == 4 && (base.compare(0, 3, "COM") == 0 || base.compare(0, 3, "LPT") == 0)
        && base[3] >= '1' && base[3] <= '9') {
        return true;
    }
    return false;
}

struct Entry
{
    mz_uint index = 0;
    std::string relPath; // 1 段外した後
    bool isDir = false;
    uint64_t size = 0;
};

struct WriteContext
{
    FILE* file = nullptr;
    const std::atomic<bool>* cancel = nullptr;
    const ZipProgressFn* progress = nullptr;
    uint64_t* done = nullptr;
    uint64_t total = 0;
    bool writeFailed = false;
};

size_t WriteCallback(void* opaque, mz_uint64 /*offset*/, const void* data, size_t size)
{
    auto* ctx = static_cast<WriteContext*>(opaque);
    if (ctx->cancel && ctx->cancel->load(std::memory_order_relaxed)) {
        return 0; // 0 を返すと miniz が展開を打ち切る
    }
    if (fwrite(data, 1, size, ctx->file) != size) {
        ctx->writeFailed = true;
        return 0;
    }
    *ctx->done += size;
    if (*ctx->progress) {
        (*ctx->progress)(*ctx->done, ctx->total);
    }
    return size;
}

} // namespace

bool SanitizeZipEntryPath(std::string_view name, std::string& out)
{
    out.clear();
    if (name.empty()) {
        return false;
    }
    std::string s(name);
    for (char& c : s) {
        if (c == '\\') {
            c = '/';
        }
    }
    if (s[0] == '/') {
        return false;
    }
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t slash = s.find('/', pos);
        if (slash == std::string::npos) {
            slash = s.size();
        }
        const std::string_view part(s.data() + pos, slash - pos);
        pos = slash + 1;
        if (part.empty() || part == ".") {
            continue; // "a//b" や "./a" は詰める。末尾の '/' (フォルダ項目) もここで落ちる
        }
        if (part == "..") {
            return false;
        }
        for (const char c : part) {
            if (static_cast<unsigned char>(c) < 0x20 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<'
                || c == '>' || c == '|') {
                return false;
            }
        }
        if (part.back() == '.' || part.back() == ' ') {
            return false; // Windows が黙って末尾を削り、別の名前と同じファイルを指してしまう
        }
        if (IsReservedDeviceName(part)) {
            return false;
        }
        if (!out.empty()) {
            out += '/';
        }
        out.append(part);
    }
    return !out.empty();
}

ZipExtractResult ExtractZip(const std::wstring& zipPath, const std::wstring& destDir, bool stripSingleTopDir,
                            const ZipProgressFn& progress, const std::atomic<bool>* cancel)
{
    ZipExtractResult result;
    mz_zip_archive zip = {};
    if (!mz_zip_reader_init_file(&zip, WideToUtf8(zipPath).c_str(), 0)) {
        result.error = std::string("cannot open zip: ") + mz_zip_get_error_string(mz_zip_get_last_error(&zip));
        return result;
    }

    // 1 周目: 全エントリの名前を検査する (書き始めてから不正な名前に気付くと半端な展開が残る)
    std::vector<Entry> entries;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    entries.reserve(count);
    uint64_t total = 0;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st = {};
        if (!mz_zip_reader_file_stat(&zip, i, &st)) {
            result.error = "cannot read zip entry #" + std::to_string(i);
            mz_zip_reader_end(&zip);
            return result;
        }
        if (!st.m_is_supported || st.m_is_encrypted) {
            result.error = std::string("unsupported zip entry: ") + st.m_filename;
            mz_zip_reader_end(&zip);
            return result;
        }
        Entry e;
        e.index = i;
        e.isDir = st.m_is_directory != 0;
        e.size = st.m_uncomp_size;
        if (!SanitizeZipEntryPath(st.m_filename, e.relPath)) {
            result.error = std::string("unsafe path in zip: ") + st.m_filename;
            mz_zip_reader_end(&zip);
            return result;
        }
        total += e.isDir ? 0 : e.size;
        entries.push_back(std::move(e));
    }

    if (stripSingleTopDir && !entries.empty()) {
        // 全エントリが "<top>/..." か、フォルダ項目 "<top>" そのものなら外す
        const std::string& first = entries.front().relPath;
        const std::string top = first.substr(0, first.find('/'));
        bool allUnderTop = true;
        bool anyFileUnderTop = false;
        for (const Entry& e : entries) {
            if (e.relPath == top && e.isDir) {
                continue;
            }
            if (e.relPath.size() <= top.size() || e.relPath.compare(0, top.size(), top) != 0
                || e.relPath[top.size()] != '/') {
                allUnderTop = false;
                break;
            }
            anyFileUnderTop = true;
        }
        if (allUnderTop && anyFileUnderTop) {
            for (Entry& e : entries) {
                e.relPath = e.relPath == top ? std::string() : e.relPath.substr(top.size() + 1);
            }
        }
    }

    // 2 周目: 書き出す
    const std::filesystem::path root(destDir);
    uint64_t done = 0;
    for (const Entry& e : entries) {
        if (e.relPath.empty()) {
            continue; // 外した最上位フォルダ自身
        }
        const std::filesystem::path target = root / std::filesystem::path(Utf8ToWide(e.relPath));
        std::error_code ec;
        if (e.isDir) {
            std::filesystem::create_directories(target, ec);
            if (ec) {
                result.error = "cannot create folder: " + e.relPath;
                mz_zip_reader_end(&zip);
                return result;
            }
            continue;
        }
        std::filesystem::create_directories(target.parent_path(), ec);
        FILE* f = nullptr;
        if (_wfopen_s(&f, target.c_str(), L"wb") != 0 || !f) {
            result.error = "cannot write: " + e.relPath;
            mz_zip_reader_end(&zip);
            return result;
        }
        WriteContext ctx;
        ctx.file = f;
        ctx.cancel = cancel;
        ctx.progress = &progress;
        ctx.done = &done;
        ctx.total = total;
        // CRC32 は miniz が展開しながら照合する (壊れた zip はここで false になる)
        const bool extracted = mz_zip_reader_extract_to_callback(&zip, e.index, WriteCallback, &ctx, 0) != 0;
        const bool closed = fclose(f) == 0;
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            result.cancelled = true;
            result.error = "cancelled";
            mz_zip_reader_end(&zip);
            return result;
        }
        if (!extracted || !closed || ctx.writeFailed) {
            result.error = ctx.writeFailed || !closed
                ? "write failed: " + e.relPath
                : "corrupt zip entry: " + e.relPath + " ("
                    + mz_zip_get_error_string(mz_zip_get_last_error(&zip)) + ")";
            mz_zip_reader_end(&zip);
            return result;
        }
        ++result.files;
    }
    mz_zip_reader_end(&zip);
    result.bytes = done;
    result.ok = true;
    return result;
}

} // namespace mye
