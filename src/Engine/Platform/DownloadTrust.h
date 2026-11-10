//====================================================================================
//                          DownloadTrust.h
//  MyEngine/ 秋田蓮音                                                      11/10/2026
//                                          ダウンロードした物に「インターネット由来」の印を付けてスキャンを通す
//====================================================================================
#pragma once
#include <string>

namespace mye {

struct DownloadTrustResult
{
    bool ok = false;
    bool blocked = false; // ウイルス対策・ポリシーが拒否した (ファイルは消されていることがある)
    std::string error;
};

// ブラウザと同じ手順 (Windows の Attachment Manager = IAttachmentExecute::Save) でダウンロード物を扱う:
// Mark-of-the-Web (Zone.Identifier) を付け、ウイルス対策のスキャンを呼ぶ (M98、ADR-037 D9)。
// 印を付けずに落とした exe を書いて起動すると、振る舞い検知に「防御の回避」と見なされる
// (v0.86 の Launcher で Behavior:Win32/DefenseEvasion.A!ml として実際に止められた)。
// 呼ぶスレッドで COM を初期化して閉じる (ワーカースレッドから呼んでよい)
DownloadTrustResult MarkAndScanDownload(const std::wstring& path, const std::string& sourceUrl);

// path の Zone.Identifier (代替データストリーム) の中身。無ければ false
bool ReadZoneIdentifier(const std::wstring& path, std::string& content);
// path に Zone.Identifier を書く (zip を展開したファイルへ印を引き継ぐ。エクスプローラーの展開と同じ)
bool WriteZoneIdentifier(const std::wstring& path, const std::string& content);

} // namespace mye
