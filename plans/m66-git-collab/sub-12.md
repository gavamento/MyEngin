# sub-12: M66l: review-1 の衛生 — 折り返しとコメントの実態合わせ、§14 に既知の制約と MYE_COLLAB_PROBE

- 依存: sub-11 (§14.6 に書く既知の制約の 1 つが sub-11 の実装を指すため)
- 状態: 未着手
- 往復: 0

## やること

出所は `review-1.md` の指摘 3・6・7・9 と、planner が裁定した 5・8 の文書化 (spec §7 の末尾 2 項)。
**挙動を変えるのは指摘 3 の 1 行だけ** (表示の折り返し)。他はコメント・未使用コードの削除・文書。

1. **[指摘 3] stage のヒント文が既定ドック幅で切れる。**
   `SourceControlWindow.cpp:567-572` が `Stage` / `Unstage` ボタンの後ろに `SameLine()` +
   `TextDisabled("%s", Tr(StrId::Scm_SelectToStage))` を置いており、spec §4.3 が確定した既定ドック幅
   (左列 ≒ 287 px) では "Select a file to stage it. The" で切れる (根拠 `cache\rev_ui_05_win.png`)。
   すぐ下 576-581 行のコメントが「既定のドック幅 285px では 4 個目のボタンのラベルが切れた」と
   同じ問題を扱っているので、幅の想定は共有されている。
   → 折り返す (`TextWrapped`) か、ボタン行の**次の行**へ出す。`Scm_Busy` / `Scm_SelectedCount` の
   同じ行に出る 2 つも同じ幅にさらされるので、3 つまとめて同じ扱いにすること。
   文言は `LocalizationTable.inl:1046` (en/ja とも)。**文字列は短くしない** (説明を削るのは直しではない)。

2. **[指摘 6] M66e で塞いだ穴を「まだ空いている」と書いたコメント + 未使用アクセサ。**
   `BuildSettingsWindow.h:32-38` が「Asset Browser の [Rebuild Scripts] は `AssetOps::RebuildGameLogic` =
   ShellExecuteW の fire-and-forget でハンドルを持たないため観測できない」と書いているが、
   `RebuildGameLogic` は M66e で削除済み (`AssetOps.cpp:1428-1433` に削除跡) で、
   `EditorApp.cpp:1555` が `scriptBuildProc_` を OR して観測できる。
   → コメントを実態へ (「呼び出し元はこの窓の Stage::Scripts と `EditorApp::PollScriptBuild` の 2 経路、
   後者は Asset Browser の [Rebuild Scripts] を M66e で一本化したもの」)。
   `同:39` の `bool IsRunning()` は全ソースで参照 0 件 → **削除する** (`IsPipelineRunning` /
   `IsScriptBuildRunning` が実際に使われている口)。

3. **[指摘 7] `EndBatch` の「溜まりを捨てる」コメントが実態と食い違う。**
   `ReloadHub.cpp:166-172` は「先に捨ててから適用するので 2 度読まない」と言い切っているが、
   `FileWatcher::DrainChanges` は**デバウンス (`kDebounceMs = 150`) を過ぎたものしか返さない**
   (`FileWatcher.cpp:13, 118-131`)。git が書いてから `EndBatch` までは数十 ms なので、
   直前に書かれたパスは `pending_` に残り、EndBatch の直後に通常経路でもう一度 `HandleChange` される。
   → **コメントを実態に合わせる (挙動は変えない)**。理由も書くこと:
   実害は「同じファイルをもう一度読み直す」だけ (シーンは同内容の `ApplyDiff`、prefab は同 base で no-op) で、
   逆に「EndBatch で適用したパスを 1 デバウンス分だけ無視する」を足すと、**EndBatch 直後に人が入れた
   本物の外部編集まで飲み込む** — 取りこぼしの方が高くつく。
   ★ここは Engine 層なので、コメント以外を触ったら `replay_verify.bat` を必ず回すこと。

4. **[指摘 9] `MYE_COLLAB_PROBE` がどの文書にも無い。**
   `SourceControlSelfTest.cpp:1218-1325` は env が立っているときだけ実 DLL 経由で status / hint /
   バッジ / revert / branches / checkout / ServiceDied を実走する恒久フックで、reviewer が実際に走らせて
   全 10 件 PASS している。隣の `MYE_COLLAB_REQUIRED` は `engine_spec.md:2254` に書かれているのに、
   こちらは無い。
   → `engine_spec.md` §14.5 の検証表 (または直後の段落) に 1 行:
   `MYE_COLLAB_PROBE=<repo> Editor.exe --selftest` で「窓のボタン → Session → DLL」の配線を
   UI 抜きで実走できること。**`CLAUDE.md` は触らない** (検証表は bat のみ / 別件の書き換えを混ぜない)。

5. **[指摘 5 の裁定を文書化] `engine_spec.md` §14.6 の既知の制約に追記。**
   競合中の保存ガードはシーン / アクター編集だけであること。理由と回避策を 1〜2 行で:
   競合中は変更一覧が競合モードに切り替わって stage ボタンが無く、`resolve` は index から
   working tree を上書きするので、Animation / Animator / Mixer / Project Settings の Save で
   競合マーカーを潰しても共有履歴には入らない。ただし working tree のマーカーは消えるので、
   その後は ours / theirs で解決してから続けること。
   (根拠と却下理由の全文は spec §7。engine_spec には結論と回避策だけを英語で。)

6. **[指摘 8 の裁定を文書化] `engine_spec.md` §14.6 に 1 行。**
   書き込み系はキャンセルできない。hook 等で git が返らないときはエディタが応答しないので、
   15 s 後に出る案内のとおりエディタを終了して起動し直す (ゲートが全文書の保存を保証しているので
   失うのは undo 履歴だけ)。sub-11 が実装した案内と**同じことを言う** (食い違わせない)。

## やらないこと (このサブでは)

- 指摘 1・2・4・8 の実装 (sub-11)。
- `ReloadHub` / `FileWatcher` の**挙動**の変更 (コメントだけ)。
- `CLAUDE.md` / `README.md` / `ci.yml` の書き換え (M66 の外に出した別件が混ざる)。
- 競合中の保存ガードを 4 窓へ広げること (planner が却下、spec §7)。
- `LocalizationTable.inl` の既存文言の短縮。

## 触る場所 (planner の見立て。coder は確認すること)

- `src\Editor\Windows\SourceControlWindow.cpp` (567-572 付近)、必要なら `LocalizationTable.inl` の
  該当行 (文言は変えない見込み)。
- `src\Editor\Windows\BuildSettingsWindow.h` (32-39)。`IsRunning()` の削除で他が壊れないことを grep で確認。
- `src\Engine\Engine\HotReload\ReloadHub.cpp` (163-177) — コメントのみ。
- `engine_spec.md` §14.5 / §14.6。

## 受け入れ条件 (このサブ)

1. 既定ドック幅 (左列 287 px) で stage のヒント文が読み切れる。
   検証: 実機で 1 枚撮る (`cache\rev_ui_05_win.png` と同じ 287x814 の条件で比較できる形)。
2. `BuildSettingsWindow.h` に `RebuildGameLogic` への言及が残っていない (grep 0 件)。`IsRunning()` が消えて
   Debug / Release ともビルドが通る。
3. `ReloadHub.cpp` のコメントがデバウンスの取りこぼしを説明しており、**コードの差分が無い**
   (`git diff` でコメント行だけ)。
4. `engine_spec.md` §14.5 に `MYE_COLLAB_PROBE`、§14.6 に指摘 5・8 の制約が入っている。
   §14.6 の記述と sub-11 の案内文が食い違わない。
5. `--selftest` (両構成) / `check_rules.ps1` / `replay_verify.bat` が緑。

## 検証コマンド

```
MSBuild MyEngine.sln /p:Configuration=Debug /p:Platform=x64   (Release も)
cmd /c bin\x64\Debug\Editor.exe --selftest
cmd /c bin\x64\Release\Editor.exe --selftest
pwsh -File tools\check_rules.ps1
tools\replay_verify.bat
pwsh -File tools\collab_fixture.ps1 cache\m66l_fx   (受け入れ条件 1 のスクショ)
```

## 実装メモ (coder が追記)

## フィードバック履歴

- (未着手)
