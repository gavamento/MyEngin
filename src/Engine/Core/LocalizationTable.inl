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

// ---- メニューバー (M47b) ----
// 訳語は Unity 日本語版に合わせる。固有名詞・略語 (Forward / Deferred / SSAO / GPU /
// SVGF / BVH / GI / spp …) は英語のまま置く
MYE_STR(Menu_File,            "File",              "ファイル")
MYE_STR(Menu_NewScene,        "New Scene",         "新規シーン")
MYE_STR(Menu_OpenScene,       "Open Scene...",     "シーンを開く...")
MYE_STR(Menu_SaveScene,       "Save Scene",        "シーンを保存")
MYE_STR(Menu_SaveSceneAs,     "Save Scene As...",  "名前を付けてシーンを保存...")
MYE_STR(Menu_BuildSettings,   "Build Settings...", "ビルド設定...")
MYE_STR(Menu_ProjectSettings, "Project Settings...", "プロジェクト設定...")
MYE_STR(Menu_Exit,            "Exit",              "終了")
MYE_STR(Menu_View,            "View",              "表示")
MYE_STR(Menu_Edit,            "Edit",              "編集")
MYE_STR(Menu_Undo,            "Undo",              "元に戻す")
MYE_STR(Menu_Redo,            "Redo",              "やり直す")
MYE_STR(Menu_GameObject,      "GameObject",        "ゲームオブジェクト")
MYE_STR(Menu_Window,          "Window",            "ウィンドウ")
MYE_STR(Menu_ResetLayout,     "Reset Layout",      "レイアウトをリセット")

MYE_STR(Menu_RenderPath,      "Render Path",       "レンダーパス")
MYE_STR(Menu_Forward,         "Forward",           "Forward")
MYE_STR(Menu_Deferred,        "Deferred",          "Deferred")
MYE_STR(Menu_Rendering,       "Rendering",         "レンダリング")
MYE_STR(Menu_Shadows,         "Shadows",           "影")
MYE_STR(Menu_Ssao,            "SSAO (Deferred)",   "SSAO (Deferred)")
MYE_STR(Menu_GpuInstancing,   "GPU Instancing",    "GPU インスタンシング")
MYE_STR(Menu_PostFx,          "Post FX",           "ポストエフェクト")
MYE_STR(Menu_RtGi,            "RT GI (Deferred)",  "RT GI (Deferred)")
MYE_STR(Menu_RtShadow,        "RT Shadow (Deferred)",     "RT 影 (Deferred)")
MYE_STR(Menu_RtReflection,    "RT Reflection (Deferred)", "RT 反射 (Deferred)")

MYE_STR(Menu_RtDebug,         "RT Debug (Deferred)",   "RT デバッグ (Deferred)")
MYE_STR(Menu_RtDbgOff,        "Off",                   "オフ")
MYE_STR(Menu_RtDbgBvhHeat,    "BVH Heatmap",           "BVH ヒートマップ")
MYE_STR(Menu_RtDbgNormals,    "Hit Normals",           "ヒット法線")
MYE_STR(Menu_RtDbgInstanceId, "Instance ID",           "インスタンス ID")
MYE_STR(Menu_RtDbgRawGi,      "Raw GI (1spp)",         "生 GI (1spp)")
MYE_STR(Menu_RtDbgAccumGi,    "Accumulated GI",        "蓄積 GI")
MYE_STR(Menu_RtDbgHistory,    "History Length",        "履歴長")
MYE_STR(Menu_RtDbgSvgfGi,     "Denoised GI (SVGF)",    "デノイズ後 GI (SVGF)")
MYE_STR(Menu_RtDbgVariance,   "Variance",              "分散")
MYE_STR(Menu_RtDbgShadowVis,  "RT Shadow Visibility",  "RT 影の可視率")
MYE_STR(Menu_RtDbgRawRefl,    "Raw Reflection (1spp)", "生の反射 (1spp)")
MYE_STR(Menu_RtDbgSvgfRefl,   "Denoised Reflection",   "デノイズ後の反射")
MYE_STR(Menu_RtScale100,      "Scale 100%",            "解像度 100%")
MYE_STR(Menu_RtScale50,       "Scale 50%",             "解像度 50%")
MYE_STR(Menu_RtScale25,       "Scale 25%",             "解像度 25%")
MYE_STR(Menu_RtBounce1,       "1 bounce",              "1 バウンス")
MYE_STR(Menu_RtBounce2,       "2 bounces",             "2 バウンス")
MYE_STR(Menu_RtTemporal,      "Temporal Accumulation", "テンポラル蓄積")
MYE_STR(Menu_RtSvgf,          "SVGF Spatial Filter",   "SVGF 空間フィルタ")
MYE_STR(Menu_RtFreezeSeed,    "Freeze Seed",           "シード固定")

// ---- ツールバー (M47b) ----
MYE_STR(Tool_TipMove,         "Move (W)",                     "移動 (W)")
MYE_STR(Tool_TipRotate,       "Rotate (E)",                   "回転 (E)")
MYE_STR(Tool_TipScale,        "Scale (R)",                    "拡縮 (R)")
MYE_STR(Tool_TipGizmoSpace,   "Toggle gizmo space",           "ギズモの座標系を切替")
MYE_STR(Tool_TipPlay,         "Play",                         "再生")
MYE_STR(Tool_TipStop,         "Stop (changes are discarded)", "停止 (変更は破棄されます)")
MYE_STR(Tool_TipPause,        "Pause / Resume",               "一時停止 / 再開")
MYE_STR(Tool_TipStep,         "Step one tick",                "1 tick 進める")
MYE_STR(Tool_SpaceLocal,      "Local",                        "ローカル")
MYE_STR(Tool_SpaceWorld,      "World",                        "ワールド")
// ImGui::Combo の "\0" 区切りアイテム列。末尾の "\0" は必須
MYE_STR(Tool_RenderPathItems, "Forward\0Deferred\0",          "Forward\0Deferred\0")
MYE_STR(Tool_TipLayouts,      "Layouts",                      "レイアウト")

// ---- ステータスバー (M47b) ----
MYE_STR(Status_Playing,       "PLAYING",  "再生中")
MYE_STR(Status_Paused,        "PAUSED",   "一時停止")
MYE_STR(Status_Editing,       "EDITING",  "編集中")
// snprintf の書式。変換指定子の並びは規則 10 が en/ja 一致を機械検査する
MYE_STR(Status_Info,          "%s%s%s | %u entities | %.0f FPS | ",
                              "%s%s%s | %u エンティティ | %.0f FPS | ")
MYE_STR(Status_TipConsole,    "Click to open the Console", "クリックで Console を開く")

// ---- レイアウト (M47b) ----
MYE_STR(Layout_Tip,           "Layouts (save / switch)",       "レイアウト (保存/切替)")
MYE_STR(Layout_Save,          "Save Layout...",                "レイアウトを保存...")
MYE_STR(Layout_Delete,        "Delete",                        "削除")
MYE_STR(Layout_ResetDefault,  "Reset (default)",               "リセット (既定)")
MYE_STR(Layout_OverwriteNote, "An existing layout with the same name is overwritten",
                              "同名レイアウトは上書きされます")
MYE_STR(Layout_NameField,     "Name",                          "名前")

// ---- 汎用ボタン (M47b) ----
MYE_STR(Common_Save,          "Save",    "保存")
MYE_STR(Common_Cancel,        "Cancel",  "キャンセル")
MYE_STR(Common_Ok,            "OK",      "OK")
MYE_STR(Common_Delete,        "Delete",  "削除")
MYE_STR(Common_Rename,        "Rename",  "名前を変更")
MYE_STR(Common_Create,        "Create",  "作成")
MYE_STR(Common_Apply,         "Apply",   "適用")
MYE_STR(Common_Close,         "Close",   "閉じる")
MYE_STR(Common_None,          "  (none)", "  (なし)")

// ---- ヒエラルキー (M47b) ----
MYE_STR(Hier_Create,          "Create",         "作成")
MYE_STR(Hier_CreatePrefab,    "Create Prefab",  "プレハブを作成")
MYE_STR(Hier_Delete,          "Delete",         "削除")

// ---- コンソール (M47b) ----
MYE_STR(Console_Clear,        "Clear",       "クリア")
MYE_STR(Console_Trace,        "Trace",       "Trace")
MYE_STR(Console_Info,         "Info",        "Info")
MYE_STR(Console_Warn,         "Warn",        "Warn")
MYE_STR(Console_Error,        "Error",       "Error")
MYE_STR(Console_Collapse,     "Collapse",    "集約")
MYE_STR(Console_AutoScroll,   "Auto-scroll", "自動スクロール")

// ---- ゲームビュー (M47b) ----
MYE_STR(GameView_Stats,       "Stats", "統計")

// ---- 検索 (M47b) ----
MYE_STR(Search_Entities,      "Entities", "エンティティ")
MYE_STR(Search_Assets,        "Assets",   "アセット")
MYE_STR(Search_RefsToSel,     "References to selection", "選択への参照")
MYE_STR(Search_SelectHint,    "  (select an entity to find who references it)",
                              "  (エンティティを選ぶと、それを参照している側を探します)")
MYE_STR(Search_WhoRefs,       "who references '%s':", "'%s' を参照しているもの:")
MYE_STR(Search_NoRefs,        "  (no references)", "  (参照なし)")

// ---- プロファイラ (M47b) ----
MYE_STR(Prof_Frame,           "frame: %6.2f ms (%.0f fps)",   "フレーム: %6.2f ms (%.0f fps)")
MYE_STR(Prof_PhaseHeader,     "phase breakdown (CPU):",       "フェーズ内訳 (CPU):")
MYE_STR(Prof_PhaseHotReload,  "  2 hot reload : %6.3f ms",    "  2 ホットリロード : %6.3f ms")
MYE_STR(Prof_PhaseTicks,      "  3-5,7 ticks  : %6.3f ms (%d tick)",
                              "  3-5,7 tick    : %6.3f ms (%d tick)")
MYE_STR(Prof_PhaseRender,     "  6 render     : %6.3f ms",    "  6 描画          : %6.3f ms")
MYE_STR(Prof_PhaseUi,         "  8 ui/present : %6.3f ms",    "  8 UI/表示       : %6.3f ms")
MYE_STR(Prof_Particles,       "particles:",                   "パーティクル:")
MYE_STR(Prof_ScopesHeader,    "CPU scopes (this frame):",     "CPU スコープ (今フレーム):")
MYE_STR(Prof_Draw,            "render: %d draw calls, %d tris, %d culled",
                              "描画: %d ドローコール / %d 三角形 / %d カリング")
MYE_STR(Prof_Memory,          "memory: %llu live allocs, %.1f MB total (%llu allocs / %llu frees)",
                              "メモリ: %llu 件が生存 / 合計 %.1f MB (確保 %llu / 解放 %llu)")
MYE_STR(Prof_RenderPath,      "render path: %s",              "レンダーパス: %s")
MYE_STR(Prof_Entities,        "entities: %u",                 "エンティティ: %u")

// ---- パーティクル設定 (M47b) ----
MYE_STR(Particle_Backend,     "Backend",                     "バックエンド")
MYE_STR(Particle_Cpu,         "CPU (SIMD)",                  "CPU (SIMD)")
MYE_STR(Particle_Gpu,         "GPU (Compute)",               "GPU (Compute)")
MYE_STR(Particle_Compare,     "Compare mode (side by side)", "比較モード (並べて表示)")
MYE_STR(Particle_Simd,        "CPU SIMD (SSE)",              "CPU SIMD (SSE)")
MYE_STR(Particle_Speedup,     "speedup: x%.1f",              "速度比: x%.1f")
MYE_STR(Particle_GpuOffset,   "GPU cloud is offset +%.1f on X",
                              "GPU 側は X に +%.1f ずらして描画しています")
MYE_STR(Particle_Alive,       "alive: %u",                   "生存数: %u")
MYE_STR(Particle_Update,      "update: %.3f ms",             "更新: %.3f ms")
MYE_STR(Particle_EditHint,    "(emitter properties are edited in the Inspector)",
                              "(エミッタの各種設定は Inspector で編集します)")

// ---- ビルド設定 (M47b) ----
MYE_STR(Build_Desc,           "Package Runtime.exe + GameLogic.dll + C# host + assets\\ into a folder.",
                              "Runtime.exe + GameLogic.dll + C# ホスト + assets\\ を 1 つのフォルダにまとめます。")
MYE_STR(Build_ReleaseNote,    "(Compile the Release config in Visual Studio / MSBuild first.)",
                              "(先に Visual Studio / MSBuild で Release 構成をビルドしてください。)")
MYE_STR(Build_BundleDotnet,   "Bundle .NET runtime (self-contained)",
                              ".NET ランタイムを同梱 (self-contained)")
MYE_STR(Build_BootScene,      "Boot scene",     "起動シーン")
MYE_STR(Build_OutputFolder,   "Output folder",  "出力フォルダ")
MYE_STR(Build_Package,        "Package Build",  "ビルドを作成")
MYE_STR(Build_OpenFolder,     "Open Folder",    "フォルダを開く")

// ---- プロジェクト設定 (M47b) ----
MYE_STR(PrjSet_Rendering,     "Rendering",           "レンダリング")
MYE_STR(PrjSet_ActivePath,    "Active render path: %s", "現在のレンダーパス: %s")
MYE_STR(PrjSet_Editor,        "Editor",              "エディタ")
MYE_STR(PrjSet_ExternalCmd,   "External editor cmd", "外部エディタのコマンド")
MYE_STR(PrjSet_SnapTranslate, "Snap: translate",     "スナップ: 移動")
MYE_STR(PrjSet_SnapRotate,    "Snap: rotate (deg)",  "スナップ: 回転 (度)")
MYE_STR(PrjSet_SnapScale,     "Snap: scale",         "スナップ: 拡縮")
MYE_STR(PrjSet_GridVisible,   "Grid visible",        "グリッドを表示")
MYE_STR(PrjSet_SaveSettings,  "Save Settings",       "設定を保存")
MYE_STR(PrjSet_PhysicsLayers, "Physics Layers",      "物理レイヤー")
MYE_STR(PrjSet_SaveLayers,    "Save Layers",         "レイヤーを保存")
MYE_STR(PrjSet_Shortcuts,     "Shortcuts",           "ショートカット")
MYE_STR(PrjSet_ColAction,     "Action###action",     "操作###action")
MYE_STR(PrjSet_ColKey,        "Key###key",           "キー###key")
MYE_STR(PrjSet_CmdHint,       "  {file} / {line} are substituted when jumping from the Console",
                              "  {file} / {line} が Console のソースジャンプで置換されます")
MYE_STR(PrjSet_LayerHint,     "Display names only (the sim uses layer numbers). Applies to Collider layer/mask",
                              "表示名のみ (sim はレイヤー番号を使う)。Collider の layer/mask に反映")
MYE_STR(Build_TipBundle,      "ON: bundles the .NET 8 runtime under dotnet\\ so the target machine does not need .NET\n"
                              "OFF: the target machine must have the .NET 8 runtime installed",
                              "ON: .NET 8 ランタイムを dotnet\\ に同梱し、配布先に .NET 不要にする\n"
                              "OFF: 配布先に .NET 8 ランタイムのインストールが必要")

// ---- シーンビューのツールバー (M47b) ----
MYE_STR(SceneView_Move,       "Move",       "移動")
MYE_STR(SceneView_Rotate,     "Rotate",     "回転")
MYE_STR(SceneView_Scale,      "Scale",      "拡縮")
MYE_STR(SceneView_Ortho,      "Ortho",      "平行投影")
MYE_STR(SceneView_Grid,       "Grid",       "グリッド")
MYE_STR(SceneView_Gizmos,     "Gizmos",     "ギズモ")
MYE_STR(SceneView_Lit,        "Lit",        "陰影あり")
MYE_STR(SceneView_Unlit,      "Unlit",      "陰影なし")
MYE_STR(SceneView_Wire,       "Wire",       "ワイヤー")
MYE_STR(SceneView_CamSpeed,   "cam %.1f",   "カメラ %.1f")

// ---- インスペクター (M47b) ----
// フィールド名 (position / roughness など) はリフレクション由来なので M47c で扱う。
// ここはインスペクターが自前で書いているラベルだけ
MYE_STR(Insp_NoSelection,     "(no selection)",             "(選択なし)")
MYE_STR(Insp_AddComponent,    "Add Component",              "コンポーネントを追加")
MYE_STR(Insp_CopyComponent,   "Copy Component",             "コンポーネントをコピー")
MYE_STR(Insp_PasteValues,     "Paste Component Values",     "コンポーネントの値を貼り付け")
MYE_STR(Insp_ResetComponent,  "Reset Component",            "コンポーネントをリセット")
MYE_STR(Insp_RemoveComponent, "Remove Component",           "コンポーネントを削除")
MYE_STR(Insp_ApplyToPrefab,   "Apply to Prefab",            "プレハブに適用")
MYE_STR(Insp_RevertToPrefab,  "Revert to Prefab",           "プレハブに戻す")
MYE_STR(Insp_ApplyAll,        "Apply All",                  "すべて適用")
MYE_STR(Insp_RevertAll,       "Revert All",                 "すべて戻す")
// Collider の衝突マスク。元から "##mask" で ID を分けていたので "###" 付きに揃える
MYE_STR(Insp_Everything,      "Everything###mask_all",      "すべて###mask_all")
MYE_STR(Insp_Nothing,         "Nothing###mask_none",        "なし###mask_none")
MYE_STR(Insp_MaskMixed,       "Mixed (0x%08X)###mask_mixed", "一部 (0x%08X)###mask_mixed")
// マテリアルインスペクタのスライダ (リフレクション由来ではなく手書きラベル)
MYE_STR(Mat_Emissive,         "emissive",                   "自己発光")
MYE_STR(Mat_Metallic,         "metallic",                   "メタリック")
MYE_STR(Mat_Roughness,        "roughness",                  "ラフネス")
MYE_STR(Insp_NoPublicFields,  "(no public fields)",         "(public なフィールドがありません)")
MYE_STR(Insp_NoManagedInst,   "(no managed instance — click 'Compile C# Scripts')",
                              "(マネージドインスタンスがありません — 「C# スクリプトをコンパイル」を押してください)")
MYE_STR(Insp_AssetGone,       "(asset no longer exists)",   "(アセットが見つかりません)")
MYE_STR(Insp_MaterialFailed,  "(material parse failed)",    "(マテリアルの読み込みに失敗)")
MYE_STR(Insp_SoundFailed,     "(sound parse failed)",       "(サウンドの読み込みに失敗)")
MYE_STR(Insp_ImportSettings,  "Import Settings",            "インポート設定")
MYE_STR(Insp_Material,        "Material",                   "マテリアル")
MYE_STR(Insp_Variations,      "Variations",                 "バリエーション")
MYE_STR(Insp_Playback,        "Playback",                   "再生")
MYE_STR(Insp_3D,              "3D",                         "3D")
MYE_STR(Insp_LoopPoints,      "Loop points (frames)",       "ループ位置 (フレーム)")
MYE_STR(Insp_AddVariation,    "+ Add Variation",            "+ バリエーションを追加")
MYE_STR(Insp_Preview,         "Preview",                    "プレビュー")
MYE_STR(Insp_StopAll,         "Stop All",                   "すべて停止")
MYE_STR(Insp_PreviewNote,     "(preview uses the saved-in-editor values, not the file on disk)",
                              "(プレビューはエディタ上の値を使います。ディスク上のファイルではありません)")
MYE_STR(Insp_TipEmissive,     "Emissive strength. With RT GI on, the surface itself becomes an indirect light source",
                              "自己発光の強さ。RT GI が有効なら発光面がそのまま間接光の光源になる")
MYE_STR(Insp_GenerateMips,    "Generate Mips",              "ミップマップを生成")
MYE_STR(Insp_CookCompress,    "Cook Compress",              "圧縮 (cook)")
MYE_STR(Insp_Srgb,            "sRGB",                       "sRGB")
MYE_STR(Insp_Transparent,     "transparent",                "半透明")
MYE_STR(Insp_PriorityNote,    "0 = unlimited. Higher priority wins when voices are stolen.",
                              "0 = 無制限。ボイスの奪い合いでは priority が高い方が勝ちます。")
MYE_STR(Insp_AttenNote,       "AudioSource can override these (overrideAttenuation).",
                              "AudioSource 側で上書きできます (overrideAttenuation)。")
MYE_STR(Insp_SpatialNote,     "spatial blend 0 = 2D. Sound is silent beyond max distance.",
                              "spatial blend 0 = 2D。max distance を超えると無音になります。")

// ---- アセットブラウザ (M47b) ----
MYE_STR(Asset_Create,         "Create",              "作成")
MYE_STR(Asset_Folder,         "Folder",              "フォルダ")
MYE_STR(Asset_Scene,          "Scene",               "シーン")
MYE_STR(Asset_MaterialItem,   "Material",            "マテリアル")
MYE_STR(Asset_AnimationClip,  "Animation Clip",      "アニメーションクリップ")
MYE_STR(Asset_Sound,          "Sound",               "サウンド")
MYE_STR(Asset_Mixer,          "Mixer",               "ミキサー")
MYE_STR(Asset_CppScript,      "C++ Script",          "C++ スクリプト")
MYE_STR(Asset_CsScript,       "C# Script",           "C# スクリプト")
MYE_STR(Asset_ShowInExplorer, "Show in Explorer",    "エクスプローラーで表示")
MYE_STR(Asset_RenameItem,     "Rename",              "名前を変更")
MYE_STR(Asset_ImportSettings, "Import Settings...",  "インポート設定...")
MYE_STR(Asset_CompressDds,    "Compress to DDS",     "DDS に圧縮")
MYE_STR(Asset_RebuildScripts, "Rebuild Scripts",     "スクリプトを再ビルド")
MYE_STR(Asset_CompileCs,      "Compile C# Scripts",  "C# をコンパイル")
MYE_STR(Asset_TipRebuild,     "Regenerate + build the C++ scripts (GameLogic.dll) and hot-reload",
                              "C++ スクリプト (GameLogic.dll) を再生成 + ビルドしてホットリロード")
MYE_STR(Asset_TipCompileCs,   "Compile the C# scripts (assets\\scripts\\*.cs) in-engine (Roslyn)",
                              "C# スクリプト (assets\\scripts\\*.cs) をエンジン内でコンパイル (Roslyn)")
MYE_STR(Asset_NameField,      "Name",                "名前")

// ---- プロジェクトマネージャ / Hub (M47b) ----
MYE_STR(Hub_Title,            "MyEngine Hub",              "MyEngine Hub")
MYE_STR(Hub_NewProject,       "New project",               "新規プロジェクト")
MYE_STR(Hub_OpenExisting,     "Open existing",             "既存を開く")
MYE_STR(Hub_Empty,            "(no projects — create one on the right)",
                              "(プロジェクトがありません — 右側から作成してください)")
MYE_STR(Hub_TipOpen,          "Double-click to open the project (restarts the editor)",
                              "ダブルクリックでプロジェクトを開く (エディタを再起動します)")
MYE_STR(Hub_RemoveFromList,   "Remove from list",          "リストから外す")
MYE_STR(Hub_Remove,           "Remove",                    "外す")
MYE_STR(Hub_Open,             "Open",                      "開く")
MYE_STR(Hub_Template,         "Template",                  "テンプレート")
MYE_STR(Hub_Location,         "Location",                  "場所")
MYE_STR(Hub_PathField,        "Path",                      "パス")
MYE_STR(Hub_NameField,        "Name",                      "名前")
MYE_STR(Hub_Change,           "Change",                    "変更")
MYE_STR(Hub_TrashNote,        "The folder is moved to the Recycle Bin (recoverable).",
                              "フォルダごと ごみ箱へ移動します (元に戻せます)。")
MYE_STR(Hub_DeleteMsg,        "Delete the project \"%s\"", "プロジェクト \"%s\" を削除します")
MYE_STR(Hub_TipEngineVer,     "Created with engine %s (current: %s)",
                              "エンジン %s で作成 (現在: %s)")

// ---- オーディオミキサー (M47b) ----
MYE_STR(Mixer_Asset,          "mixer asset",   "ミキサーアセット")
MYE_STR(Mixer_AddBus,         "+ Add Bus",     "+ バスを追加")
MYE_STR(Mixer_Remove,         "Remove",        "削除")
MYE_STR(Mixer_ReloadDefault,  "Reload Default", "既定を読み直す")
MYE_STR(Mixer_Reverb,         "Reverb",        "リバーブ")
MYE_STR(Mixer_Preset,         "preset",        "プリセット")
MYE_STR(Mixer_WetDry,         "wet/dry",       "wet/dry")
MYE_STR(Mixer_TipRename,      "click to rename", "クリックで名前を変更")
MYE_STR(Mixer_NoReverb,       "(Default = no reverb)", "(Default = リバーブ無し)")
MYE_STR(Mixer_CreateFromAb,   "(create one from the Asset Browser)",
                              "(アセットブラウザから作成してください)")
MYE_STR(Mixer_NoDevice,       "audio device is not available (--no-audio) — faders are inert",
                              "オーディオデバイスが使えません (--no-audio) — フェーダーは効きません")
MYE_STR(Mixer_NoSystem,       "audio system is not available", "オーディオシステムが使えません")
MYE_STR(Mixer_NoReverbBus,    "reverb bus is unavailable on this device",
                              "この環境ではリバーブバスが使えません")

// ---- アニメーター (M47b) ----
MYE_STR(Anim_SelectEntity,    "Select an entity that has an AnimatorController component.",
                              "AnimatorController を持つエンティティを選択してください。")
MYE_STR(Anim_UnknownCtrl,     "AnimatorController references an unknown .controller.json.",
                              "AnimatorController が未知の .controller.json を参照しています。")
MYE_STR(Anim_NoCtrlLibrary,   "(no controller library)", "(コントローラライブラリがありません)")
MYE_STR(Anim_ClickNode,       "(click a node to edit)",  "(ノードをクリックすると編集できます)")
MYE_STR(Anim_Controller,      "Controller: %s",          "コントローラ: %s")
MYE_STR(Anim_Current,         "Current: %s",             "現在: %s")
MYE_STR(Anim_State,           "State: %s",               "ステート: %s")
MYE_STR(Anim_Transitions,     "Transitions",             "遷移")
MYE_STR(Anim_Conditions,      "Conditions:",             "条件:")
MYE_STR(Anim_ParamsLive,      "Parameters (live)",       "パラメータ (実行中)")
MYE_STR(Anim_AddState,        "Add State",               "ステートを追加")
MYE_STR(Anim_AddTransition,   "Add Transition",          "遷移を追加")
MYE_STR(Anim_AddCondition,    "+ condition",             "+ 条件")
MYE_STR(Anim_AddParam,        "+ param",                 "+ パラメータ")

// ---- アニメーション (M47b) ----
MYE_STR(Clip_SelectEntity,    "(select an entity)",       "(エンティティを選択してください)")
MYE_STR(Clip_NoAnimator,      "'%s' has no Animator.",    "'%s' に Animator がありません。")
MYE_STR(Clip_CreateClip,      "Create Clip + Animator",   "クリップ + Animator を作成")
MYE_STR(Clip_Name,            "Clip: %s",                 "クリップ: %s")
MYE_STR(Clip_Length,          "Length (ticks)",           "長さ (tick)")
MYE_STR(Clip_Play,            "Play",                     "再生")
MYE_STR(Clip_Preview,         "Preview",                  "プレビュー")
MYE_STR(Clip_Record,          "Record",                   "記録")
MYE_STR(Clip_KeyAtTick,       "Key at current tick:",     "現在の tick のキー:")
MYE_STR(Clip_Position,        "Position",                 "位置")
MYE_STR(Clip_Rotation,        "Rotation",                 "回転")
MYE_STR(Clip_Scale,           "Scale",                    "拡縮")
MYE_STR(Clip_DeleteKeys,      "Delete keys @tick",        "この tick のキーを削除")

// ---- サウンドジェネレータ (M47b) ----
MYE_STR(SndGen_Presets,       "Presets",           "プリセット")
MYE_STR(SndGen_Waveform,      "Waveform",          "波形")
MYE_STR(SndGen_Duty,          "Duty",              "デューティ比")
MYE_STR(SndGen_FreqStart,     "Freq start (Hz)",   "開始周波数 (Hz)")
MYE_STR(SndGen_FreqEnd,       "Freq end (Hz)",     "終了周波数 (Hz)")
MYE_STR(SndGen_Duration,      "Duration (s)",      "長さ (秒)")
MYE_STR(SndGen_Amplitude,     "Amplitude",         "振幅")
MYE_STR(SndGen_Envelope,      "Envelope (ADSR)",   "エンベロープ (ADSR)")
MYE_STR(SndGen_Attack,        "Attack (s)",        "アタック (秒)")
MYE_STR(SndGen_Decay,         "Decay (s)",         "ディケイ (秒)")
MYE_STR(SndGen_Sustain,       "Sustain",           "サステイン")
MYE_STR(SndGen_Release,       "Release (s)",       "リリース (秒)")
MYE_STR(SndGen_AdsrNote,      "(A+D+R is scaled down proportionally if it exceeds the duration)",
                              "(A+D+R が全長を超えると比例縮小されます)")
MYE_STR(SndGen_Format,        "Format",            "フォーマット")
MYE_STR(SndGen_SampleRate,    "Sample rate",       "サンプルレート")
MYE_STR(SndGen_Mono,          "Mono",              "モノラル")
MYE_STR(SndGen_Stereo,        "Stereo",            "ステレオ")
MYE_STR(SndGen_PreviewSec,    "Preview",           "プレビュー")
MYE_STR(SndGen_SaveSec,       "Save",              "保存")
MYE_STR(SndGen_AudioDisabled, "(audio disabled)",  "(オーディオ無効)")
MYE_STR(SndGen_NoiseSeed,     "Noise seed: %llu",  "ノイズシード: %llu")

// ---- GameObject 生成メニュー (M47b) ----
// **メニューの表示名だけ**を訳す。生成されるオブジェクト名 ("Cube" 等) は英語のまま —
// NameComponent はシーン JSON に載り WorldHasher の入力にもなる「データ」なので触らない
MYE_STR(Create_Empty,         "Create Empty",      "空のオブジェクト")
MYE_STR(Create_3DObject,      "3D Object",         "3D オブジェクト")
MYE_STR(Create_Cube,          "Cube",              "キューブ")
MYE_STR(Create_Sphere,        "Sphere",            "スフィア")
MYE_STR(Create_Plane,         "Plane",             "平面")
MYE_STR(Create_Quad,          "Quad",              "クアッド")
MYE_STR(Create_Cylinder,      "Cylinder",          "シリンダー")
MYE_STR(Create_Capsule,       "Capsule",           "カプセル")
MYE_STR(Create_Light,         "Light",             "ライト")
MYE_STR(Create_DirLight,      "Directional Light", "平行光")
MYE_STR(Create_PointLight,    "Point Light",       "ポイントライト")
MYE_STR(Create_SpotLight,     "Spot Light",        "スポットライト")
MYE_STR(Create_Audio,         "Audio",             "オーディオ")
MYE_STR(Create_AudioSource,   "Audio Source",      "オーディオソース")
MYE_STR(Create_AudioListener, "Audio Listener",    "オーディオリスナー")
MYE_STR(Create_Camera,        "Camera",            "カメラ")

// ---- 統計ウィンドウ / 未保存確認 (M47b) ----
MYE_STR(Stats_Fps,            "FPS: %.1f (%.3f ms)",        "FPS: %.1f (%.3f ms)")
MYE_STR(Stats_Frame,          "Frame: %llu / Tick: %llu",   "フレーム: %llu / Tick: %llu")
MYE_STR(Stats_Entities,       "Entities: %u",               "エンティティ: %u")
MYE_STR(Stats_PlayState,      "Play state: %s",             "再生状態: %s")
MYE_STR(Stats_Editing,        "Editing",                    "編集中")
MYE_STR(Stats_Playing,        "Playing",                    "再生中")
MYE_STR(Stats_Paused,         "Paused",                     "一時停止")
MYE_STR(Stats_GameLogic,      "GameLogic: %s (v%u, %u scripts)",
                              "GameLogic: %s (v%u, スクリプト %u 個)")
MYE_STR(Confirm_UnsavedBody,  "The scene has unsaved changes. Save them?",
                              "シーンに未保存の変更があります。保存しますか？")
MYE_STR(Confirm_Save,         "Save",       "保存する")
MYE_STR(Confirm_DontSave,     "Don't Save", "保存しない")

// ---- インスペクター: オーディオ/マテリアルの手書きラベル (M47b) ----
MYE_STR(Insp_Bus,             "bus",              "バス")
MYE_STR(Insp_Volume,          "volume",           "音量")
MYE_STR(Insp_VolumeRandom,    "volume random",    "音量のランダム幅")
MYE_STR(Insp_Pitch,           "pitch",            "ピッチ")
MYE_STR(Insp_PitchRandom,     "pitch random",     "ピッチのランダム幅")
MYE_STR(Insp_Loop,            "loop",             "ループ")
MYE_STR(Insp_StreamBgm,       "stream (BGM)",     "ストリーム (BGM)")
MYE_STR(Insp_LoopStart,       "loop start",       "ループ開始")
MYE_STR(Insp_LoopEnd,         "loop end",         "ループ終了")
MYE_STR(Insp_Priority,        "priority",         "優先度")
MYE_STR(Insp_MaxInstances,    "max instances",    "同時再生数の上限")
MYE_STR(Insp_Weight,          "weight",           "重み")
MYE_STR(Insp_SpatialBlend,    "spatial blend",    "spatial blend")
MYE_STR(Insp_MinDistance,     "min distance",     "最小距離")
MYE_STR(Insp_MaxDistance,     "max distance",     "最大距離")
MYE_STR(Insp_Rolloff,         "rolloff",          "減衰カーブ")
MYE_STR(Insp_Doppler,         "doppler",          "ドップラー")
MYE_STR(Insp_ReverbSend,      "reverb send",      "リバーブ送り")
MYE_STR(Insp_Revert,          "Revert",           "戻す")

// ---- アニメーター: ノード/遷移の手書きラベル (M47b) ----
MYE_STR(Anim_Clip,            "clip",         "クリップ")
MYE_STR(Anim_Name,            "name",         "名前")
MYE_STR(Anim_Speed,           "speed",        "速度")
MYE_STR(Anim_Loop,            "loop",         "ループ")
MYE_STR(Anim_From,            "from",         "遷移元")
MYE_STR(Anim_To,              "to",           "遷移先")
MYE_STR(Anim_Duration,        "duration",     "所要 tick")
MYE_STR(Anim_HasExitTime,     "hasExitTime",  "終了待ち")
MYE_STR(Anim_Param,           "param",        "パラメータ")
MYE_STR(Anim_Val,             "val",          "値")
MYE_STR(Anim_TransitionRow,   "-> %s (%d/%d)", "-> %s (%d/%d)")

// ---- アニメーション: 補足 (M47b) ----
MYE_STR(Clip_TipPreview,      "Preview: scrub to see the pose (restored on exit).\n"
                              "Record: edit the transform (gizmo) to auto-key at the current tick.",
                              "プレビュー: スクラブでポーズを確認できます (終了時に元へ戻ります)。\n"
                              "記録: ギズモで動かすと現在の tick に自動でキーが打たれます。")
MYE_STR(Insp_NoneItem,        "(none)", "(なし)")
// 衝突マスクのポップアップ内ボタン (上の mask_all/mask_none とは別のポップアップ)
MYE_STR(Insp_MaskAll,         "Everything", "すべて")
MYE_STR(Insp_MaskNone,        "Nothing",    "なし")
MYE_STR(Insp_StreamNote,      "(stream: plays on the BGM lane — previewing another stream sound crossfades)",
                              "(ストリーム: BGM レーンで再生されます。別のストリーム音を試聴するとクロスフェードします)")
MYE_STR(Insp_LoopEndNote1,    "end <= start means \"to the end\". Applies to stream + loop only:",
                              "end <= start は「最後まで」の意味です。stream + loop のときだけ効きます:")
MYE_STR(Insp_LoopEndNote2,    "the stream seeks back sample-exactly, so the seam is inaudible.",
                              "ストリームはサンプル単位で正確に巻き戻るので、継ぎ目は聞こえません。")

// ---- Hub: 補足 (M47b) ----
MYE_STR(Hub_EngineVer,        "engine %s",   "エンジン %s")
