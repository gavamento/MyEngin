// LocalizationTable.inl — UI 文字列の単一情報源。1 行 = 1 文字列。
//
//   MYE_STR(<StrId>, <English>, <日本語>)
//
// Localization.h が enum を、Localization.cpp が 2 本の配列をここから生成する。
// ja を書き忘れるとマクロの引数不足でコンパイルエラーになる = 網羅性が保証される。
//
// 規約 (tools\check_rules.ps1 の規則 10 が機械検査する):
//   1. en / ja とも空文字列にしない
//   2. "###" を含む行は "###" 以降 (= ImGui の ID) が en と ja で完全一致すること。
//      ImHashStr は "###" でハッシュをシードに戻すので、表示名だけが変わり ID は不変になる。
//      ウィンドウ名の ID は既存の英語名と 1 バイトも変えてはならない — imgui.ini /
//      DockBuilderDockWindow / layouts の panels.json がこの文字列で参照している
//   3. 変換指定子 (%s / %d …) の並びが en と ja で一致すること。MSVC の printf は
//      POSIX の位置指定 ("%1$s") に非対応なので、言語で語順を変えることはできない
//
// Tr() の戻り値を printf 系 (ImGui::Text / TextDisabled / SetTooltip …) の
// **書式引数** に渡してはいけない。訳文中の % がそのまま変換指定子として解釈される。
// TextUnformatted(Tr(X)) か Text("%s", Tr(X)) を使うこと。

// ---- ウィンドウ名 (M47a) ----
// "###" の右辺は M47 以前の英語ウィンドウ名そのまま。変更禁止。
MYE_STR(Win_Hierarchy,        "Hierarchy###Hierarchy",                 "ヒエラルキー###Hierarchy")
MYE_STR(Win_Inspector,        "Inspector###Inspector",                 "インスペクター###Inspector")
MYE_STR(Win_Console,          "Console###Console",                     "コンソール###Console")
MYE_STR(Win_Scene,            "Scene###Scene",                         "シーン###Scene")
MYE_STR(Win_Game,             "Game###Game",                           "ゲーム###Game")
MYE_STR(Win_Assets,           "Assets###Assets",                       "アセット###Assets")
MYE_STR(Win_Animation,        "Animation###Animation",                 "アニメーション###Animation")
MYE_STR(Win_Animator,         "Animator###Animator",                   "アニメーター###Animator")
MYE_STR(Win_Search,           "Search###Search",                       "検索###Search")
MYE_STR(Win_Profiler,         "Profiler###Profiler",                   "プロファイラー###Profiler")
MYE_STR(Win_ParticleSettings, "Particle Settings###Particle Settings", "パーティクル設定###Particle Settings")
MYE_STR(Win_SoundGenerator,   "Sound Generator###Sound Generator",     "サウンドジェネレーター###Sound Generator")
MYE_STR(Win_AudioMixer,       "Audio Mixer###Audio Mixer",             "オーディオミキサー###Audio Mixer")
MYE_STR(Win_ProjectSettings,  "Project Settings###Project Settings",   "プロジェクト設定###Project Settings")
MYE_STR(Win_BuildSettings,    "Build Settings###Build Settings",       "ビルド設定###Build Settings")
MYE_STR(Win_Stats,            "Stats###Stats",                         "統計###Stats")

// ---- タイトルを表示するモーダル (M47a) ----
// ID は OpenPopup / BeginPopupModal の両方が Tr() を通すので "###" 右辺は自由に決めてよい。
// "###" を付けるのは、開いている最中に言語を切り替えてもモーダルが閉じないようにするため。
MYE_STR(Popup_UnsavedChanges, "Unsaved Changes###UnsavedChanges", "未保存の変更###UnsavedChanges")
MYE_STR(Popup_SaveLayout,     "Save Layout###SaveLayout",         "レイアウトを保存###SaveLayout")
MYE_STR(Popup_DeleteProject,  "Delete Project###DeleteProject",   "プロジェクトを削除###DeleteProject")
MYE_STR(Popup_RenameProject,  "Rename Project###RenameProject",   "プロジェクト名を変更###RenameProject")
MYE_STR(Popup_Error,          "Error###Error",                    "エラー###Error")
MYE_STR(Popup_RenameAsset,    "Rename Asset###RenameAsset",       "アセット名を変更###RenameAsset")
MYE_STR(Popup_ImportSettings, "Import Settings###ImportSettings", "インポート設定###ImportSettings")
MYE_STR(Popup_CreateAsset,    "Create Asset###CreateAsset",       "アセットを作成###CreateAsset")

// ---- 言語切替 (M47a) ----
// 言語名はどちらのモードでも自国語で出すのが慣例 (Unity / VS Code 等と同じ)
MYE_STR(Menu_Language,        "Language", "言語")
MYE_STR(Menu_LangJapanese,    "日本語",   "日本語")
MYE_STR(Menu_LangEnglish,     "English",  "English")
