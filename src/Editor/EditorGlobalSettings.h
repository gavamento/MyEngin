#pragma once
#include <string>

#include "Engine/Core/Localization.h"

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

    void Load();
    void Save() const;

    static std::wstring Path();
};

} // namespace mye
