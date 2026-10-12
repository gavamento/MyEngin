# sub-07: カーネルのアセット扱い・スクリプトのインスペクターの Compute 欄・project_settings

- 依存: sub-06
- 状態: 未着手
- 往復: 0
- 更新: 2026-10-12 (U10 による表示の状態)
- 再編: 2026-10-12 (旧 sub-06 を U6「スクリプトのインスペクター」で組み直した。アセットのインスペクターは作らない)

## やること
spec 4.1.7 / 4.2 (project_settings) / 4.3、D19 / D27、受け入れ 11 / 16。

1. `.kernel.hlsl` のアセット扱い: `.meta` (sub-01 / sub-03 で手で作った .meta の線種 (LF) と guid (SelfTestUnused は手で付けた値) を、既存の生成規則に揃える)、Project ブラウザでの表示、FileWatcher → GPU のホットリロード → ハッシュと検査の評価し直し → C# の再生成要求 (sub-06)。
1b. (sub-10 からの申し送り) プロジェクトとエンジンの両方の `kernels` に同じ名前のカーネルがあるときに、ShaderManager のプロジェクト索引と ComputeService のソース表がどちらを採るかを確かめる。シェーダルートの優先順位 (プロジェクトが先) と揃っていなければ揃える。揃えられない場合は失敗にしてログに出す。結果を実装メモに書く。
1c. (sub-06 から移管) FileWatcher が `.kernel.hlsl` の変更を拾ったら、`ComputeService::UpdateKernelSource` を呼ぶ。C# が読み込まれていれば、`ManagedHost::CompileScripts` を呼ぶか要求を立てる。Kernels.gen.cs の再生成は CompileScripts の中で行われる。
   - 確認: カーネルを編集すると C# の State がいったん不一致で失敗し、自動で再コンパイルされて一致に戻ること。selftest か手順で確かめる。
   - C# の状態の表示には `IsDeclared(CallerKind::CSharp, ...)` と `FindKernel(...)->csharp` を使う。C# の宣言表のスクリプト型名は `Type.FullName`。
2. スクリプトのインスペクターの Compute 欄 (C++ の FIELDS スクリプトと C# スクリプトの両方): 行 = 宣言 ∪ 観測 (名前順)。列は spec 4.1.7 のとおり。
   上書き (Auto / CPU 固定) はカーネル単位で、「このカーネルを使うすべてのスクリプトに効く」と表示する (U8 の裁定)。State を GPU へ向ける UI は作らない。
   状態には「宣言なしで失敗」(U10) と「宣言あり・カーネル無し」(D28-f) を含める。検査で違反しているカーネルは、規則と行を表示する。文字列は `Tr()` で日英両方。`###` の後の識別子と書式指定子の順を揃える。
3. `project_settings.json` の `"compute"` 節: 読込 (不正な値は既定値 + WARN)。上書きを書き戻す箇所は 1 つにする (`ProjectSettingsWindow.cpp:66` の流儀)。Runtime / Server も読み、contentHash の対象に入っていることを確かめる。

## やらないこと (このサブでは)
- `.kernel.hlsl` アセットのインスペクター (D19)

## 触る場所 (planner の見立て)
- エンティティのインスペクターのスクリプト欄 (C++ スクリプト / C# スクリプト)、`src/Editor/Windows/Project/ProjectSettingsWindow.cpp`
- 設定の読込 (`particleBackend` を読んでいる箇所と同じ所)、`LocalizationTable.inl`、FileWatcher / ReloadHub

## 受け入れ条件 (このサブ)
1. spec 5-11: 編集 → 不一致の表示 → C++ の State が失敗 → 再ビルドで一致に戻る。C# は再生成で一致に戻る。手動で確認する手順を実装メモに書く (文書への反映は sub-09)
2. spec 5-16: 対応表から作る表示の行 (宣言のみ・宣言なしで失敗・両方・宣言あり・カーネル無し・検査違反) を selftest で確かめる。C++ と C# のスクリプトのインスペクターのスクリーンショットを、一時プローブと `--screenshot` で撮る (パスを実装メモに書く)
3. 設定の読込 (既定値・不正な値・上書き) を selftest で確かめる

## 検証コマンド
- Release ビルド、`Editor.exe --selftest`、`tools\check_rules.ps1`

## 実装メモ (coder が追記)

SELF_EVAL: sub-07 (round 1) の要旨 (正本は司会への返信)。

- .meta: SelfTestAllFuncs / SelfTestUnused の guid を、AssetDatabase の生成規則 (HashStr(NormalizePathKey(絶対パス))。common.hlsli.meta で一致を確認) の値に直した。LF・末尾改行なしは既存と同じ (sub-12 で済み)。
- 1b (同名カーネルの優先順): ShaderManager の索引は assetsRoot 配下だけを走査し、解決は「索引 → dirs_ (プロジェクト assets\shaders → エンジン assets\shaders) の直下の Name.kernel.hlsl」。ComputeService のソース表は assetsRoot\shaders\kernels だけ。
  外部プロジェクトからエンジンの kernels は GPU・CPU のどちらからも見えず、プロジェクトとエンジンの同名は起きない。起こりうるのはプロジェクト内の同名 (索引が両方を無効にする) と kernels\ 以外に置いた場合で、
  ComputeService::VerifyKernelSourceAgreement が CPU と GPU の読むファイルを照合し、食い違うカーネルを検査違反扱い + ERROR 1 回にする (Bind・DLL 読込・ホットリロードの後に実行)。
- 1c: ReloadHub::SetKernelHooks。.kernel.hlsl の変更で ComputeService::ReloadKernelSource (ディスクの綴りで名前を取る・変更検出・削除) → 照合 → 変更ありなら ManagedHost::CompileScripts。
  手動確認: Editor で Play 中に assets\shaders\kernels\X.kernel.hlsl を保存する。コンソールに "[compute] kernel source changed - recompiling C# scripts"、C++ の行は「古い — GameLogic を再ビルド」、C# の行は再コンパイル後に「一致」。Rebuild Scripts で C++ も一致に戻る。
- Compute 欄: カーネルごとの縦並び (狭いインスペクターで表の列が切れたため)。上書きの Auto / CPU 固定はセッション上書き、保存は Project Settings の Compute 欄だけ (SaveComputeKernelOverrides)。
- project_settings.json "compute": ComputeSettings.{h,cpp}。Editor / Runtime = EngineLoop、Server = HeadlessSim が Bind の直後に適用。assets 配下なので contentHash に入る (selftest で確認)。
- 追加の修正: EngineLoop が managedHost.CompileScripts の前に managedHost.SetComputeService を呼んでいなかったため、Editor / Runtime の起動時コンパイルの C# 登録がエンジンに拒否されていた (HeadlessSim は順序が違い見えなかった)。呼びを前に出した。
- round 2: Debug ビルド + Debug --selftest PASS、tools\replay_verify.bat PASS (19 ジョブ。Debug/Release 13 シーン + jobs A/B + タイムトラベル + 規則)。Debug で初めて ComputeSettingsSelfTest の dangling (一時 vector への FindRow) が表面化したので直した。Project Settings 窓の Compute 欄のスクリーンショット = scratchpad の project_settings.png (一時プローブはコミット対象に残していない)。
- スクリーンショット (一時プローブ、リポジトリ外): C++ = scratchpad の inspector_cpp.png、C# = inspector_cs.png (C:\Users\akita\AppData\Local\Temp\claude\C--HAL-MyEngin\1045aa2f-0617-404e-a3b6-257b3a941d8f\scratchpad)。

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-10-12)。
  - must: Debug のビルドと Debug の `Editor.exe --selftest`、`tools\replay_verify.bat` が未実施。EngineLoop / HeadlessSim / ScriptHost / ReloadHub を変えた広範なエンジン変更なので、AGENTS.md 7 章に従って必要。
  - should: Project Settings 窓の Compute 欄のスクリーンショットを撮る。
  - 採用した逸脱・追加: 縦並びの表示、セッション上書きと保存口が 1 つ、設定の許容範囲、1b の解釈、EngineLoop の SetComputeService の順序の修正 (sub-06 の取りこぼしを sub-07 で直したものとして扱う)。
- round 2: VERDICT OK (planner、2026-10-12)。
  - must #1 は解消した。Debug のビルドと selftest、replay_verify の 19 ジョブが PASS した。Debug でしか出ないテスト側の寿命切れのバグを直した。
  - should #2 も解消した。Project Settings 窓のスクリーンショットを撮った。
