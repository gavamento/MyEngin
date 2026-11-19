#pragma once
#include <cstdint>
#include <string>

#include "Editor/Tools/Terminal/TerminalLayout.h"
#include "Engine/Core/Localization/Localization.h"

namespace mye {

// %LOCALAPPDATA%\MyEngine\ — プロジェクトに依存しないマシンローカルの設定置き場。
// LOCALAPPDATA が取れない環境では exe の隣にフォールバックする。
// projects.json (ProjectRegistry) と editor_global.json が同居する
std::wstring MachineLocalDir();

// editor_global.json — **プロジェクトを開く前に必要な**エディタ設定 (M47a)。
//
// エディタは Hub (プロジェクト選択画面) を描いたあと `RelaunchSelfWithProject` で
// --project 付きの新しいプロセスとして起動し直す (EditorMain.cpp)。つまり Hub の時点では
// プロジェクトが未確定なので、UI 言語のように Hub の描画にも要る設定は
// <project>\.mye\editor_settings.json (EditorSettings) では間に合わない。
//
// 追加の作法は EditorSettings と同じ 3 点セット:
//   struct にメンバ + 既定値 / Load() に root.value(...) / Save() に root[...] =
struct EditorGlobalSettings {
    Lang uiLanguage = Lang::Ja;
    // 更新の自動確認 (M98d)。起動時に前回から 24 時間以上たっていれば GitHub Releases を見に行く
    bool autoUpdateCheck = true;
    int64_t lastUpdateCheckUnix = 0; // 最後に確認できた UTC の Unix 秒 (0 = 未確認)
    // ブラウザ窓 (M105b)。インストール済みブラウザはマシンの性質なのでプロジェクトには置かない
    std::string browserId;           // 選んだ候補の id。"custom" = exe 指定。空 = 未選択
    std::string browserCustomExe;    // "custom" のときの exe パス (UTF-8)
    std::string browserStartUrl;     // 開始 URL (UTF-8)。空 = ブラウザの既定
    // ターミナル窓 (M106d)。シェルの場所はマシンの性質なのでプロジェクトには置かない。範囲のクランプは窓の側 (TerminalLayout.h)
    std::string terminalShell;       // 起動するシェル (UTF-8)。空 = 自動検出 (pwsh → powershell → cmd)
    int terminalFontSize = kTerminalDefaultFontSize;       // 文字サイズ (px)
    int terminalScrollback = kTerminalDefaultScrollback;   // スクロールバックの行数。変更は次に起動するシェルから
    bool terminalLoadVsDevEnv = true; // VS 開発者環境を読み込んで起動する (読み込みの実処理は M106e)

    void Load();
    void Save() const;

    static std::wstring Path();
};

} // namespace mye
