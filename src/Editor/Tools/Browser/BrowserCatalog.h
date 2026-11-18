//====================================================================================
//                          BrowserCatalog.h
//  MyEngine/ 秋田蓮音                                                      11/18/2026
//                                          インストール済みブラウザの検出 (候補一覧)
//====================================================================================
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace mye {

// 起動引数の系統。exe のファイル名で決める (ClassifyBrowserFamily)
enum class BrowserFamily { Chromium, Firefox, Unknown };

// StartMenuInternet のサブキー 1 つから読んだ生の値 (Win32 を使わずに候補へ畳めるよう分けてある)
struct BrowserRawEntry {
    std::wstring id;           // サブキー名
    std::wstring displayName;  // サブキーの既定値 (空ならサブキー名を使う)
    std::wstring command;      // shell\open\command の既定値
    std::wstring httpsProgId;  // Capabilities\URLAssociations の "https" 値 (無ければ空)
};

struct BrowserCandidate {
    std::wstring id;
    std::wstring displayName;
    std::wstring exePath;
    BrowserFamily family = BrowserFamily::Unknown;
    bool isDefault = false;
};

// command (引用符あり / なし / 引数付き / %1 付き) から実行ファイルのパスだけを取り出す。取り出せなければ空
std::wstring ExtractBrowserExePath(const std::wstring& command);

// exe のファイル名 (大文字小文字無視) から起動引数の系統を決める
BrowserFamily ClassifyBrowserFamily(const std::wstring& exePath);

// 起動引数 (exe を除く)。Chromium 系 = --new-window [URL] / Firefox 系 = -new-window [URL] / 不明 = URL だけ
std::wstring BuildBrowserLaunchArgs(BrowserFamily family, const std::wstring& url);

// 生データ → 候補一覧の純関数。raw は優先順 (HKCU → HKLM → WOW6432Node) に並べて渡す。
// 重複 (exe の正規化パスが同じ) は先勝ち、存在しない exe と iexplore.exe は除く、並びは表示名 (大文字小文字無視) → exe パス。
// defaultProgId = 既定ブラウザの https の ProgId (空なら既定の印は付けない)
std::vector<BrowserCandidate> BuildBrowserCandidates(const std::vector<BrowserRawEntry>& raw,
                                                     const std::wstring& defaultProgId,
                                                     const std::function<bool(const std::wstring&)>& exeExists);

// レジストリを読んで候補を返す (Win32)。パネルを開いたとき / 再検出のときだけ呼ぶ (毎フレーム読まない)
std::vector<BrowserCandidate> DetectInstalledBrowsers();

// --browser-list: 候補を stdout に出して 0 を返す
int RunBrowserListCli();

} // namespace mye
