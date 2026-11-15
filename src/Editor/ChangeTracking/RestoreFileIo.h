//====================================================================================
//                          RestoreFileIo.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          復元の判定が使うファイルの読み取り口 (読むだけ。宣言)
//====================================================================================
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace mye {

// 復元の判定 (M104p) がファイルシステムを見る唯一の口。**読み取りだけ**で、書く・消す・名前を変える関数は持たない
// (書くのは次のサブ。ここに足さない)。試験は偽の実装で、呼び出し回数と読んだ範囲を数える
constexpr uint32_t kRestoreAttrAbsent = 0xFFFFFFFFu; // GetFileAttributesW の INVALID_FILE_ATTRIBUTES
constexpr uint32_t kRestoreAttrDirectory = 0x10u;    // FILE_ATTRIBUTE_DIRECTORY
constexpr uint32_t kRestoreAttrReparse = 0x400u;     // FILE_ATTRIBUTE_REPARSE_POINT (シンボリックリンクとジャンクション)

class RestoreFileIo {
public:
    enum class ReadStatus : uint8_t { Ok, Absent, Error };

    virtual ~RestoreFileIo() = default;

    // ファイル属性。存在しなければ kRestoreAttrAbsent
    virtual uint32_t Attributes(const std::wstring& absPath) = 0;
    // offset から最大 maxBytes を out へ足す。totalSize はファイルの総バイト数 (読めたときだけ)
    virtual ReadStatus Read(const std::wstring& absPath, uint64_t offset, size_t maxBytes, std::string& out, uint64_t& totalSize) = 0;
};

// 本物の実装 (Win32 の読み取り専用の API だけ)
std::unique_ptr<RestoreFileIo> MakeSystemRestoreFileIo();

} // namespace mye
