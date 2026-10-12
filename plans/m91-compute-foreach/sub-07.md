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

## フィードバック履歴
