#pragma once
#include <cstdint>
#include <string>

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

    void Load();
    void Save() const;

    static std::wstring Path();
};

} // namespace mye
