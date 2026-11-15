//====================================================================================
//                          OwnSaveRecord.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          自分の保存の直後に控えたファイルの印 (宣言)
//====================================================================================
#pragma once
#include <cstdint>
#include <map>
#include <string>

namespace mye {

// ファイルの大きさと最終更新時刻 (file_time_type の tick 数)。読めなければ valid = false
struct FileStamp {
    bool valid = false;
    uint64_t size = 0;
    int64_t writeTime = 0;
    bool operator==(const FileStamp&) const = default;
};

FileStamp StatFileStamp(const std::wstring& absPath);

// Editor が保存した直後のファイルの印を控える。ファイル監視は保存の 150 ms 以上あとに届くので、その間に編集を続けると
// 自分の保存の通知が「未保存のシーンへの外部変更」に見える。保留になった時点のファイルが控えた印と同じなら、
// それは自分の保存の結果で (ライブのシーンは保存の内容 + その後の編集)、読み込まずに保留を捨ててよい。
// 外から書き換えられていれば印が変わる (最終更新時刻は 100 ns 単位) ので、今までどおり確認を出す
class OwnSaveRecord {
public:
    // 保存した直後に呼ぶ (その場でファイルを見る)。読めなければ控えない
    void Note(const std::wstring& absPath);
    // 印を直接渡す (試験用)
    void Note(const std::wstring& absPath, const FileStamp& stamp);
    // pendingPath のいまのファイルが、控えた印と同じか。控えが無い・読めない・違えば false
    bool MatchesNow(const std::wstring& pendingPath) const;
    // 純粋な判定: どちらも読めていて、大きさと更新時刻が同じ
    static bool Matches(const FileStamp& noted, const FileStamp& current);

private:
    // 正規化したパス → 控えた印。保存したファイルごとの最新だけ
    std::map<std::wstring, FileStamp> noted_;
};

} // namespace mye
