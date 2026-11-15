//====================================================================================
//                          RestoreWriteIo.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          復元がファイルシステムを書き換える唯一の口 (宣言)
//====================================================================================
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace mye {

// 復元 (M104p2) が利用者のファイルとジャーナルを書く・消す・名前を変える操作は、すべてここを通す。
// 読み取りは RestoreFileIo (別の口)。試験は偽の実装で、N 回目の書き込みの失敗や、途中の他者の書き込みを差し込む。
// Copy は大きいファイルのために別のスレッドから呼ばれることがあるので、実装は並行呼び出しに耐えること
class RestoreWriteIo {
public:
    virtual ~RestoreWriteIo() = default;

    // 書き切れたときだけ既存ファイルを置き換える (WriteFileReplacing)。親ディレクトリは先に MakeDirs で作る
    virtual bool Write(const std::wstring& absPath, std::string_view bytes) = 0;
    // offset 0 は作り直し (空にして先頭から)。それ以外は offset の位置へ書く (大きい内容を塊ごとに積むため)
    virtual bool Append(const std::wstring& absPath, uint64_t offset, std::string_view bytes) = 0;
    virtual bool Remove(const std::wstring& absPath) = 0;
    // replace が偽のとき、行き先が既にあれば失敗する (名前を戻す先を上書きしない)
    virtual bool Rename(const std::wstring& from, const std::wstring& to, bool replace) = 0;
    // 内容の複写 (行き先は上書き)。大きいファイルでも呼び出し側のメモリに全部載せない
    virtual bool Copy(const std::wstring& from, const std::wstring& to) = 0;
    virtual bool MakeDirs(const std::wstring& absDir) = 0;
    // 自分のジャーナルの掃除だけに使う (復元の対象のファイルには使わない)
    virtual bool RemoveTree(const std::wstring& absDir) = 0;
};

std::unique_ptr<RestoreWriteIo> MakeSystemRestoreWriteIo();

} // namespace mye
