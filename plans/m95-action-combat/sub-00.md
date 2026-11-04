# sub-00: selftest のスイート登録表 (tier)・`MYE_SELFTEST_ONLY` の汎用化・所要時間ログ・`--selftest-full` の入口・CI・予約マーカー

- 依存: なし (Wave 0。これが master に入ってから Wave 1 の worktree を切る)
- 状態: OK (commit 8405f20。条件だった Release 検証は 2026-11-04 に司会が master で実施: replay_verify PASS、Release --selftest-full 110 スイート 0 failed)
- 往復: 0

## やること

spec §4.0 の「sub-00」節。`EditorMain.cpp` の selftest 節のハードコード (`MYE_SELFTEST_ONLY` の 4 値 + 全走の `ok &=` 列) を
「スイート名 → 関数 + tier」の登録表に置き換え、カンマ区切りの任意名で絞れるようにする。`--selftest-full` / `MYE_SELFTEST_FULL=1` の入口、
`--selftest-list` (tier 付き)、所要時間ログを足す。CI を `--selftest-full` に書き換える。
あわせて、並列実装用の予約マーカー (コメント行) を spec 付録 A.2 / A.5 / A.6 のファイルに入れる。

**このサブでは全スイートを `Fast` にしておく** (= `--selftest` と `--selftest-full` が同じ集合)。層分けは計測後に sub-00b が行う。

## やらないこと (このサブでは)

- 層分け (tier の書き換え) と縮小 (sub-00b)
- 各スイートの中身・順序・集合の変更 (全走は変更前と同じスイートを同じ順で回す)
- 製品の挙動 (sim / 描画、`--selftest*` 以外の経路) への変更
- M95 の機能 (sub-01 以降)

## 触る場所 (planner の見立て)

- `src\Editor\App\EditorMain.cpp:570-748` selftest 節。`:586-633` の 4 値の分岐と `:634-748` の `ok &=` 列を、登録表の走査に置き換える。一時フォルダの扱い (`:575-584`) と「先行失敗があっても全部回す」(`:571-574`) は維持。`:298` の `--selftest` の隣に `--selftest-full` / `--selftest-list`
- `src\Editor\SelfTest\SelfTestSuites.h / .cpp` (新規): `enum class SelfTestTier { Fast, Full }`、`struct SelfTestSuite { const char* name; bool (*run)(); SelfTestTier tier; const char* note; }`、全走順の表 (全部 `Fast`)、別名表 (`servernet` / `servernet-*` / `computescript` / `twod` / `m94` / `m95`)、`ResolveSelfTestSelection(onlyValue, full, out, unknown)` のような純関数 (テスト可能にする)。`Run*SelfTest` の宣言ヘッダは `EditorMain.cpp` の include 群から写す
- `tools\gen_project_files.ps1` を回して `build\*.vcxproj` に新規ファイルを載せる
- `src\Engine\Engine\Net\ServerNetSelfTest.cpp:784-789` は触らない (`servernet-v1` を環境変数から自分で読む仕組みはそのまま。登録表では `servernet-` 始まりを `servernet` スイートに解決する)
- `src\Engine\Engine\App\EngineCli.cpp:80-87` の表: `--selftest` が載っていないなら新フラグも載せない (`RunEngineCliSelfTest` が表を検査しているので、載せるなら全部)
- `.github\workflows\ci.yml:142-148` の selftest (Debug) / (Release) の 2 ステップを `--selftest-full %MYE_EXTRA_ARGS%` に
- `AGENTS.md:144` の表に `--selftest` (既定 = fast 層) / `--selftest-full` / `MYE_SELFTEST_ONLY=<名,...>` / `--selftest-list` の行、`:149` の提出前を `--selftest-full` に
- 予約マーカー (中身は空のコメント行。spec 付録 A の表のとおり):
  - `src\Engine\Core\Ecs\Components.h`: `AnimatorControllerComponent` (`:827` の `sTypeId` の直前)、`CharacterControllerComponent` (`:882` の直前。sub-02 → sub-05 の順で 2 行)、`RigidbodyComponent` (`:463` の直前)、`SkinnedMeshComponent::PoseLayer` (`:339-350` の末尾)
  - `src\Engine\Core\Ecs\Components.cpp`: 同 3 struct の反射登録の `});` の直前
  - `src\Engine\Engine\Animation\AnimatorControllerSelfTest.cpp`: `RunAnimatorControllerSelfTest` の本体末尾 (最後の `return` の前) に sub-01 / sub-04 / sub-05 / sub-06 の 4 行
  - `src\Engine\Engine\Physics\Rigid\PhysicsSelfTest.cpp`: 同様に sub-02 / sub-05
  - `src\Editor\SelfTest\PartSelfTest.cpp`: sub-01 (ABI) / sub-03
  - `src\Engine\Engine\Animation\SkeletonSelfTest.cpp`: sub-05
  - `src\Engine\Core\Localization\LocalizationTable.inl`: 末尾に `// ---- M95 ----`
  - マーカーの文言は付録 A.2 / A.5 の表と一字一句同じにする (各サブはその文字列を grep して自分の場所を探す)

## 受け入れ条件 (このサブ)

spec §5 の 33, 34, 35, 36, 37。
- 33: 変更前 (`git show HEAD:src/Editor/App/EditorMain.cpp`) の `ok &= mye::Run*SelfTest();` の関数名列と、登録表の関数名列を抽出して diff が空。コマンドと出力を SELF_EVAL に貼る
- 34: 各 `MYE_SELFTEST_ONLY` 値のログ (スイート行) を貼る。`m94` の集合は変更前の `:606-633` と同じ順
- 35: `--selftest-list` の出力 (tier 付き) を貼る
- 36: `git diff --stat` が selftest 節 / 新規 2 ファイル / vcxproj / `ci.yml` / `AGENTS.md` / マーカー行だけ。RV PASS
- 37: `--selftest-full` / `MYE_SELFTEST_FULL=1` のログ (全スイート) と `MYE_SELFTEST_ONLY` の優先、`ci.yml` の diff を貼る

## 検証コマンド

- ビルド Debug / Release
- `$env:MYE_SELFTEST_ONLY='physics'; bin\x64\Debug\Editor.exe --selftest` ほか受け入れ 34 の各値 (PowerShell ツールから。終わったら `Remove-Item Env:MYE_SELFTEST_ONLY`)
- `bin\x64\Debug\Editor.exe --selftest-list`
- SELF_EVAL を返す前: `bin\x64\Debug\Editor.exe --selftest` (既定層 = この時点では全部)
- VERDICT OK の直前 (最後の round): `bin\x64\Debug\Editor.exe --selftest-full` と `bin\x64\Release\Editor.exe --selftest-full`、`tools\check_rules.ps1`、`tools\replay_verify.bat`
- 全スイートの所要時間が出るので、Debug / Release 各 1 回の ms 表を実装メモに残す (sub-00b の計測の種になる)

## 作業場所の注意

- このサブは Wave 0 なので master のチェックアウト (`C:\HAL\MyEngin`) で作業してよい (司会の指示に従う)。worktree で作業する場合は worktree の絶対パスだけを使い、`C:\HAL\MyEngin` へ書かない。終了時に両方で `git status` を確認する

## 実装メモ (coder が追記)

SELF_EVAL: sub-00 (round 1) の要旨 (全文は司会が受けた SELF_EVAL)。

- 新規 `src\Editor\SelfTest\SelfTestSuites.h / .cpp`: 登録表 110 本 (全部 Fast)・別名 `twod` / `m94` / `m95`・`ResolveSelfTestSelection` (純関数)・`PrintSelfTestList`・`RunSelfTestSelection` (所要時間ログ)。`servernet-*` は `servernet` スイートへ解決。
- `EditorMain.cpp`: selftest 節を表の走査へ置換。`--selftest-full` / `--selftest-list` / `MYE_SELFTEST_FULL=1` / 環境変数の可変長読み。
- 別名 1 個だけの指定は別名の並びで回す (`m94` を従来の並びに保つため)。複数指定は表の順。
- 予約マーカー: Components.h / .cpp、4 つの selftest、LocalizationTable.inl。`ci.yml` と `AGENTS.md` を更新。
- Debug 全走 (`--selftest` 566.9 s / `--selftest-full` 583.0 s / `MYE_SELFTEST_FULL=1` 594.1 s、各 110 本 0 failed)。上位: fracture 210 s、servernet 106 s、physics 64 s、navagent 45 s、fractureeditor 28 s、surfacedeferred 18 s、twoddemo 17 s。
- Release のリンクはユーザーの Editor.exe (PID 39252、Release) が掴んでいて未実施。replay_verify も未実施。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-04)。受け入れ 33〜37 を Debug の実測ログと diff で確認 (登録表 110 本の名前列 diff 空、ONLY の各値・exit 2・--selftest-list・--selftest-full / MYE_SELFTEST_FULL / ONLY 優先、ci.yml / AGENTS.md の diff、製品コード差分なし、マーカー 11 箇所の配置)。「別名 1 個だけは別名の並び」規則は仕様へ採用 (spec §4.0 / §8)。Release の --selftest-full と replay_verify は別プロセス (PID 39252) が bin\x64\Release\Editor.exe を掴んでいる環境要因で未実施 → 条件付き OK: 司会がユーザーに exe の解放を頼んでから Release ビルド → --selftest-full (Release) → tools\replay_verify.bat を実施し、結果を台帳に記す。FAIL なら sub-00 を差し戻す。nit: ResolveSelfTestSelection の専用テスト無し (申し送り、sub-00b で足してよい)
