# sub-12: M66l: review-1 の衛生 — 折り返しとコメントの実態合わせ、§14 に既知の制約と MYE_COLLAB_PROBE

- 依存: sub-11 (§14.6 に書く既知の制約の 1 つが sub-11 の実装を指すため)
- 状態: OK (round 2、コミット待ち)
- 往復: 2

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

7. **[round 2 で追加・must] `MYE_COLLAB_PROBE` の失敗 commit 検査が空振りしている** (coder の質問 2)。
   `SourceControlSelfTest.cpp` の `anythingStaged` が `e.indexState != ChangeState::None` を見ているが、
   `porcelain.rs:156-160` は未追跡を `index='?'` で返し `StateFromStatusChar('?')` が `Untracked` を返すので、
   **未追跡ファイルが 1 個あるだけで検査ごと飛ぶ**。fixture は必ず未追跡を持つ = sub-11 で恒久化した
   「窓のボタンと同じ経路で commit の失敗を実走する」検査が実質いつも走っていない。
   → 飛ばすのは「index に staged がある / マージ途中」だけにする (`Untracked` を除外する)。
   未追跡だけならコミットは必ず失敗し、リポジトリは 1 バイトも変わらないので、プローブの約束は保たれる。
   ★合わせて `engine_spec.md` §14.5 に書いた "the failing commit is skipped unless the index is clean" を
     実態に合わせる (未追跡は clean 扱いだと分かる言い回しに)。

8. **[round 2 で追加・should] `DrawRemoteBar` の帯も折り返す** (coder の質問 1、spec §4.3「幅の規則」)。
   `SourceControlWindow.cpp:486` 付近の `Scm_NoRemote` / `Scm_NoUpstream` / `Scm_UpToDate` /
   `Scm_AheadBanner` (と behind の `TextUnformatted(label)`) は `TextDisabled` / `TextUnformatted` のままで、
   ja の 287 px で「このリポジトリにはリモートが設定されていません。」が切れる
   (根拠 `cache\m66l_ui_ja_win.png`)。#3 と同じ直し方 (色を借りて `TextWrapped`) で揃える。
   ★**撮った画面に切れて写っているものだけ**を直す。この窓には非折り返しの `TextDisabled` が 31 箇所あり、
     推測で全部を触ると、横スクロールする子窓やツリー行のように折り返しが**間違い**な場所まで巻き込む。

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
6. (round 2 で追加、spec 22) `MYE_COLLAB_PROBE` の「失敗する commit」検査が、**未追跡ファイルを含む
   標準の fixture でそのまま走る**。検証: `cache\m66l_fx` (M と ? を持つ) に対して probe を回し、
   `probe: a commit with an empty index answers with a failure` と
   `probe: a failed commit leaves the message in the box` が **PASS 行として出る** (skip のログが出ない)。
   staged がある状態では従来どおり skip されることも 1 回確認する (`git add` してから回す)。
7. (round 2 で追加、spec 21) 既定ドックの左列 (287 px) の **ja** で、`DrawRemoteBar` の帯が切れない。
   検証: ja の 287 px を 1 枚撮り直し、`cache\m66l_ui_ja_win.png` で切れていた
   「このリポジトリにはリモートが設定されていません。」が読み切れること。

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

### round 1 (SELF_EVAL の写し)

```
SELF_EVAL: sub-12 (round 1)
実装:
  - #3 src\Editor\Windows\SourceControlWindow.cpp:DrawChanges — Stage / Unstage の後ろの
    SameLine を外し、3 つのヒント (Scm_Busy / Scm_SelectToStage / Scm_SelectedCount) を
    **次の行に折り返して**出す。TextDisabled は折り返さないので
    PushStyleColor(ImGuiCol_Text, ImGuiCol_TextDisabled の色) + TextWrapped + PopStyleColor。
    文言は 1 文字も変えていない。3 つとも同じ扱いにしたのは、短い 2 つだけ横に残すと
    状態で行の高さが変わって下の破棄ボタンが動くから (理由をコメントに記載)
  - #6 src\Editor\Windows\BuildSettingsWindow.h — 未使用の `IsRunning()` を削除 (参照 0 件を
    全ソース grep で確認)。コメントを実態へ: 呼び出し元は「この窓の Stage::Scripts」と
    「Asset Browser の [Rebuild Scripts] を受ける EditorApp::PollScriptBuild」の 2 経路で、
    後者は M66e で一本化した = ゲートから観測できない経路はもう無い。受け入れ条件 2 に
    合わせて `RebuildGameLogic` の名前は書かない (grep 0 件)
  - #6 追補 src\Editor\AssetOps.cpp:PrepareProjectScriptsBat の関数コメント — 「起動は
    呼び出し側 (RebuildGameLogic = fire-and-forget / StartGameLogicBuild = ...) の責務」も
    削除済み関数を生きているかのように書いていたので実態へ (削除跡コメントは残す)
  - #7 src\Engine\Engine\HotReload\ReloadHub.cpp:EndBatch — コメントのみ。
    DrainChanges が kDebounceMs(150 ms) 経過分しか返さないこと / 直前に書かれたパスは
    EndBatch の直後にもう一度 HandleChange されること / 実害は二度読みだけであること /
    「1 デバウンス分無視する」を足すと EndBatch 直後の本物の外部編集を飲み込むので
    採らないこと、を記載。**コード差分ゼロ** (git diff がコメント行だけ)
  - #9 engine_spec.md §14.5 — 検証表に `MYE_COLLAB_PROBE=<repo> Editor.exe --selftest` の行。
    件数は書かず内容で書いた (M66k が 2 件足しており、未追跡ファイルがあると失敗 commit の
    2 件は skip されるので「N 件」は腐る)
  - #5 の裁定 engine_spec.md §14.6 — 競合中の保存ガードがシーン / アクター編集だけである
    こと、却下の根拠 (競合モードには stage ボタンが無い / resolve が index から working tree を
    上書きする / 競合シーンだけは空で開いて空で保存される)、残存リスク 2 つ (外部ターミナルの
    `git add` / 競合文書を開いていた窓が「保存できないのに未保存」でゲートを閉じる) と
    回避策 (先に ours/theirs) を追記
  - #8 の裁定 engine_spec.md §14.6 — 書き込み系は中断できない / hook で返らない git は
    エディタごと固まる / 15 s 後に出る案内のとおり終了して起動し直す / 失うのは undo 履歴だけ /
    常時出さない理由。sub-11 の `Scm_OpStuckHint` と同じことを言う形にした
  - planner の nit src\Editor\SourceControl\SourceControlSelfTest.cpp (j2) — %TEMP%\mye_scm_no_dll を
    `fs::remove_all` で掃除 (mye_scm_actions と同じ扱い)。実行後に両方とも残っていないことを確認
仕様との差分:
  - [追加] AssetOps.cpp:1352 付近の関数コメント (上記 #6 追補)。sub-12 の受け入れ条件 2 は
    BuildSettingsWindow.h しか要求していないが、**同じ削除済み関数を「生きている起動口」として
    書いた腐りコメント**が同じ M66e の後始末として残っていたため、コメント 3 行だけ直した。
    挙動・シンボルは無変更
  - [追加] セルフテスト (j2) の %TEMP% 掃除 1 行 (FIX_REQUEST が「拾ってよい」とした planner の nit)
検証:
  - MSBuild Debug / Release x64 → 両方 exit 0、警告なし (BuildSettingsWindow.h 変更後に再ビルド済み)
  - cmd /c bin\x64\Debug\Editor.exe --selftest → exit 0、"FAIL" 0 件 (大文字一致)、
    Source control self test PASSED
  - cmd /c bin\x64\Release\Editor.exe --selftest → exit 0、"FAIL" 0 件
  - MYE_COLLAB_PROBE=cache\m66l_fx bin\x64\Debug\Editor.exe --selftest → exit 0、probe 12 件 PASS
    (失敗 commit の 2 件は「index が空でない」で skip。理由は下の申し送り)
  - pwsh -File tools\check_rules.ps1 → 0 error / 0 warning
  - tools\replay_verify.bat → 並列 10 ジョブ 75.1 s、[PASS] 7 シーン + snapshot + timetravel + 規則、exit 0
    (最終差分でもう一度回して緑を確認)
  - 実機 (受け入れ条件 1): pwsh -File tools\collab_fixture.ps1 cache\m66l_fx →
    M と ? を 1 件ずつ作り、`Editor.exe --project cache\m66l_fx --warp --screenshot` で 3 枚:
      cache\m66l_ui_b_win.png  = review-1 と同条件 (窓を 287x814 で浮かせた) の en →
        「Select a file to stage it. The .meta and .terrain.edit sidecars follow it automatically.」が
        2 行で全部読める (修正前 cache\rev_ui_05_win.png は "The" で切れていた)
      cache\m66l_ui_dock_win.png = **既定ドックのまま** (左列 287 px、Source Control タブ選択) の en →
        同じく 2 行で読み切れる
      cache\m66l_ui_ja_win.png = 同条件の ja (--lang ja) → 3 行で読み切れる
  - 未実行: cargo test / collab_verify.bat (Rust と C ABI・プロトコルは無変更)、shot_verify.bat
    (golden は Runtime.exe が撮る = エディタ UI は 1 枚も入らない。描画コードは無変更)
自己採点 (1-5):
  仕様適合: 5 — 指摘 3・6・7・9 と 5・8 の文書化をすべて実施。受け入れ条件 1〜5 を実測で満たした
    (2 は grep 0 件、3 は git diff がコメント行のみ)。やらないこと (挙動変更・CLAUDE.md・文言短縮・
    4 窓へのガード拡張) は 1 つも踏んでいない
  正しさ: 4 — 表示の直しは実画面 3 枚で確認、コメント・文書は一次情報 (FileWatcher.cpp の
    kDebounceMs / DrainChanges、ops.rs の resolve、EditorApp の 2 経路、GitTransaction の
    kStuckHintSec) と突き合わせた。5 にしないのは、ReloadHub の「二度読みは無害」の記述が
    コード読解による推論で、EndBatch 直後の再 HandleChange を実機で観測してはいないため
  コード品質: 4 — 実質差分は 1 か所 (SameLine → 折り返し) で、残りはコメントと文書。
    なぜそうしたかを全部コメントに残した。ImGui の色は既存の TextDisabled と同じ意味色を
    借りるだけで themeColor の新しい割り当ては増やしていない
  テスト: 3 — 挙動を足していないので新規テストは無し (折り返しは純関数化できず、golden にも
    エディタ UI は入らない = 機械で固定する手段が無く、画像 3 枚が根拠)。既存の
    --selftest / check_rules / replay_verify は全部緑で、%TEMP% 掃除の 1 行は実行後の
    Test-Path で確認した
不安・質問:
  1. **同じ「287 px で切れる」が Scm_NoRemote にも出る** (ja のみ実測)。DrawRemoteBar の
     `SourceControlWindow.cpp:486` は TextDisabled のままで、ja の
     「このリポジトリにはリモートが設定されていません。」が
     "…設定されて" で切れる (根拠 cache\m66l_ui_ja_win.png、en は収まる)。同じ行に出る
     Scm_UpToDate / Scm_AheadBanner も長い upstream 名で同型になりうる。sub-12 の範囲は
     「stage の行の 3 つ」と明記されているので**直していない**。直すなら同じ 3 行の置換で済む
     (別サブか、この round に追加指示をもらえれば入れる)
  2. probe の失敗 commit チェックが**未追跡ファイルがあるだけで skip される** (M66k)。
     `anythingStaged` の判定が `e.indexState != ChangeState::None` で、porcelain の `?` は
     indexState にも Untracked が入るため。fixture は必ず未追跡を持つので、実質いつも skip。
     直すなら `?` を除外する 1 行だが、sub-11 の成果物なので触っていない
触ったファイル:
  - src\Editor\Windows\SourceControlWindow.cpp
  - src\Editor\Windows\BuildSettingsWindow.h
  - src\Editor\AssetOps.cpp
  - src\Engine\Engine\HotReload\ReloadHub.cpp
  - src\Editor\SourceControl\SourceControlSelfTest.cpp
  - engine_spec.md
  - plans\m66-git-collab\sub-12.md (この実装メモ)
申し送り:
  - 実機スクショの撮り方 (reviewer の rev_ui_05_win.png と同条件を再現する手順):
    fixture を作って 1 回起動すると `.mye\imgui.ini` に
    `[Window][Source Control] Pos=0,60 Size=287,814 DockId=0x00000001,1` が残る。
    **DockId 行を消すと同じ寸法で浮く** (reviewer の 1 枚はこれ)。既定ドックのまま
    Source Control タブを選ばせたいときは、その窓の DockId の順序番号を 0 にして
    `[Docking][Data]` のノード 1 から `Selected=...` を消す (先頭タブが選ばれる)。
    2 回目の起動で効く。撮影は `--warp --no-audio --screenshot ... --shot-frame 60 --frames 70`、
    `--font-embedded` は付けない (reviewer と同じフォントにするため)
  - 生成物 (gitignore 済み。ステージ対象外): cache\m66l_fx (+ .gitconfig)、
    cache\m66l_ui_*.png / .log、cache\m66l_ui_*_win.png、cache\m66l_selftest_*.log、
    cache\m66l_replay*.log、cache\m66l_probe.log
  - 上の「不安・質問」2 件はどちらも sub-12 の外 (別サブか v1.5 の判断待ち)
```

### round 2 (SELF_EVAL の写し)

```
SELF_EVAL: sub-12 (round 2)
実装:
  - #1 (must) src\Editor\SourceControl\SourceControlSelfTest.cpp の probe — `anythingStaged` の判定から
    `Untracked` を除外した。純ラムダ `stagedForCommit(s) = (s != None && s != Untracked)` を置き、
    `stagedForCommit(e.indexState) || e.conflict || (!e.primaryListed && stagedForCommit(e.state))` で
    判定。**未追跡を staged と誤認していた**理由 (porcelain v2 が未追跡を index 列も '?' で返す) を
    コメントに残した。合わせて `engine_spec.md` §14.5 の "the failing commit is skipped unless the
    index is clean" を「staged が無く マージ途中でもないときだけ走る / **未追跡は staged に数えない**
    (数えると fixture 相手では必ず自分で自分を飛ばす)」に書き換え
  - #2 (should) src\Editor\Windows\SourceControlWindow.cpp:DrawRemoteBar — 帯の 5 か所を
    spec §4.3 の幅の規則に合わせた: `Scm_NoRemote` / `Scm_NoUpstream` / behind の `label` /
    `Scm_AheadBanner` / `Scm_UpToDate` を `PushStyleColor(TextDisabled の色) + TextWrapped` へ。
    behind だけは既存の `themeColor::Accent` の push 内なので色は足していない (`TextUnformatted` →
    `TextWrapped("%s", label)`)。**早期 return の 2 経路は return 前に PopStyleColor** している
    (実機で両方通して ImGui のスタック警告が出ないことを確認済み)。
    子窓 `###ScmRemoteCommits` の中 (author / subject) と `Scm_Loading` は**触っていない** —
    前者は横スクロールしない枠付き子窓の中、後者は §4.3 が名指しした「短い状態語」
仕様との差分:
  - [追加] #1 の判定に `(!e.primaryListed && stagedForCommit(e.state))` を足した。planner の指示は
    「`Untracked` を除外」だが、`PairedEntry::indexState` は**本体の行にしか入らない**
    (`SourceControlState.cpp:236-240`)。`x.png.meta` **だけ**を stage した状態では本体の
    indexState が None のままなので、指示どおりだとプローブが**成功する commit** を打って
    リポジトリを変えてしまう (= spec 受け入れ条件 22 が飛ばす理由に挙げている状況そのもの)。
    サイドカーだけの行は合成状態で保守的に見る 1 節を足して塞いだ。過剰に skip する側なので
    受け入れ条件 6 の「未追跡だけなら走る」は壊れない (実測で確認)
  - [追加なし] #2 は 5 か所とも planner が名指しした帯の中。31 か所ある他の非折り返しには触っていない
検証:
  - MSBuild Debug / Release x64 → 両方 exit 0、警告なし
  - **受け入れ条件 6 の両方向**を実測:
      (a) 未追跡入り fixture (`cache\m66l_fx` = M 1 件 + ? 2 件) で probe →
          `probe: a commit with an empty index answers with a failure` と
          `probe: a failed commit leaves the message in the box` が **PASS 行で出る**
          (skip のログ無し。probe の PASS は 12 → **14** に増えた。`cache\m66l_probe_r2a.log`)。
          実行後に `git log` 1 件・`git status` 変化なし = リポジトリは変わっていない
      (b) `git add` で 1 件 staged にしてから probe → `[probe] skipped the failing-commit check
          (the index is not empty)` が出て 2 件は走らない (`cache\m66l_probe_r2b.log`)。確認後 `git reset` で戻した
  - **受け入れ条件 7 / spec 21**: 帯の 4 状態を ja の既定ドック (左列 287 px) で 1 枚ずつ撮った。
      `cache\m66l_r2_noremote_win.png` = リモート無し → 「このリポジトリにはリモートが設定されて
        いません。」が **2 行で全部読める** (修正前 `cache\m66l_ui_ja_win.png` は "…設定されて" で切れていた)
      `cache\m66l_r2_noupstream_win.png` = リモート有り・追跡なし → 「追跡ブランチなし」1 行 (元から切れない)
      `cache\m66l_r2_uptodate_win.png`   = 「origin/main と同じ状態です。」1 行 (折り返しても見た目は不変)
      `cache\m66l_r2_ahead_win.png`      = 「送信していないコミットが 1 件」1 行
      `cache\m66l_r2_behind_win.png`     = 「新しいコミットが 1 件 (origin/main)」1 行 (右端ぎりぎり。
        これより長い remote 名なら今回の変更で折り返す)
      en は `cache\m66l_r2_en_win.png` (帯・stage ヒントとも切れなし)。fixture は
      `cache\m66l_fx2` + bare `cache\m66l_origin.git` + `cache\m66l_peer` で 4 状態を作った
  - cmd /c bin\x64\Debug\Editor.exe --selftest → exit 0 / "FAIL" 0 件
  - cmd /c bin\x64\Release\Editor.exe --selftest → exit 0 / "FAIL" 0 件
  - pwsh -File tools\check_rules.ps1 → 0 error / 0 warning
  - tools\replay_verify.bat → 並列 10 ジョブ 75.6 s、[PASS]、exit 0
  - 未実行 (round 1 と同じ理由): cargo test / collab_verify.bat (Rust・C ABI 無変更)、
    shot_verify.bat (golden は Runtime.exe が撮る = エディタ UI は入らない)
自己採点 (1-5):
  仕様適合: 5 — must #1 / should #2 とも実施し、spec §4.3 の幅の規則 (文は折り返す / 語は折り返さない /
    子窓とツリー行は触らない / 基準は ja) に沿っている。受け入れ条件 6 は**両方向**、7 は 4 状態 + en を実測
  正しさ: 5 — #1 は「空振りしていた検査が実際に走る」ことを PASS 行の増加 (12 → 14) で観測し、
    リポジトリが変わらないことを git log / status で確認。skip 側も再現。#2 は 5 状態を実画面で確認し、
    早期 return を含む 2 経路の style スタックも実走で通した
  コード品質: 4 — 折り返しは同じ 2 行パターンの繰り返しで、ヘルパは作っていない (窓の他の 31 箇所へ
    無自覚に広がる口を作らないため)。判定ラムダにはなぜ Untracked を除くかを実装の一次情報つきで書いた
  テスト: 3 — #1 は既存の恒久フックが**やっと走るようになった**分だけ被覆が増えたが、
    「未追跡があっても走る」こと自体を機械で固定するテストは無い (probe は env 依存で CI では走らない)。
    #2 の折り返しは画像でしか確認できない。round 1 と同じ理由で 3
不安・質問: なし (round 1 の 2 件はどちらもこの round で解消した)
触ったファイル: (round 1 + round 2 の合算)
  - src\Editor\Windows\SourceControlWindow.cpp
  - src\Editor\Windows\BuildSettingsWindow.h
  - src\Editor\AssetOps.cpp
  - src\Engine\Engine\HotReload\ReloadHub.cpp
  - src\Editor\SourceControl\SourceControlSelfTest.cpp
  - engine_spec.md
  - plans\m66-git-collab\sub-12.md (実装メモのみ)
申し送り:
  - **probe の PASS 行数は 14 が正**になった (未追跡だけの fixture のとき)。staged があると 12 に減る。
    reviewer が数える場合はこの 2 通りがあることに注意
  - `PairedEntry::indexState` は**本体の行にしか入らない** (サイドカーだけが status に載った行は
    `primaryListed=false` で index / worktree の内訳を持たない)。「index が空か」を entries から
    判定するコードを他所で書くときは同じ穴を踏む
  - 帯の 4 状態を作る fixture 手順: `collab_fixture.ps1` → `git init --bare -b main` した origin を
    `git remote add` (= 追跡なし) → `push -u` (= 同期) → ローカルに 1 コミット (= 先行) →
    clone した peer から push して**起動時 fetch を待つ** (behind は fetch 後にしか出ないので
    `--shot-frame 150` まで伸ばす)
  - 生成物 (gitignore 済み): cache\m66l_fx / m66l_fx2 (+ .gitconfig)、cache\m66l_origin.git、
    cache\m66l_peer、cache\m66l_*.png / *_win.png、cache\m66l_*.log
```

## フィードバック履歴

- round 1: VERDICT REWORK (planner) — must 1 = probe の失敗 commit 検査が未追跡で空振り (+ §14.5 の文言)、
  should 2 = `DrawRemoteBar` の帯も折り返す。仕様変更あり (spec §4.3 の幅の規則 / 受け入れ条件 21・22)。
  round 1 の成果物 (指摘 3・6・7・9 と 5・8 の文書化) は差し戻し対象外で認定済み。
- round 1: VERDICT **REWORK** (planner)。must 1 / should 1 / nit 0。
  round 1 の成果物そのものに不足は無い (指摘 3・6・7・9 と 5・8 の文書化はすべて実施され、
  planner 側でも diff を通読して確認した) が、**coder が挙げた「不安・質問」2 件のうち 1 件が
  『恒久検査が実質いつも走っていない』**というもので、これを残したまま閉じると
  「テストがあるのに走っていない」+ 同じコミットで書いた engine_spec §14.5 の説明が実態と食い違う。
  同じファイル (`SourceControlSelfTest.cpp`) を既に触っているサブなので、round 2 で 1 行足して閉じる。
  - must = 「やること」7 (probe の `anythingStaged` から `Untracked` を除外 + §14.5 の言い回し修正)。
  - should = 「やること」8 (`DrawRemoteBar` の帯の折り返し。撮った画面に切れて写っているものだけ)。
  - 仕様側は planner が先に直した: spec §4.3 に「幅の規則」、§5 に受け入れ条件 21・22、§6 の sub-12 行、§8 に履歴 2 行。
  - planner 独立検証 (round 1 の成果物に対して): `git diff` 全通読 /
    `porcelain.rs:156-160` と `StateFromStatusChar` を突き合わせて質問 2 が事実であることを確認 /
    `DrawRemoteBar` の 4 文字列が非折り返しであることをコードで確認 /
    窓の非折り返し `TextDisabled` が 31 箇所あることを数え、全置換を**要求しない**と決めた。
- round 2: VERDICT **OK** (planner)。must 0 / should 0 / nit 2 (どちらも申し送り止まり、直さない)。
  - 「仕様との差分」1 件目 (`!primaryListed && stagedForCommit(state)` の追加) は**planner の指示が不足していた側**。
    `SourceControlState.cpp:236-240` は `path == primary` のときしか `indexState` / `primaryListed` を入れないので、
    `x.png.meta` だけを stage した状態では本体行が `indexState == None` になり、指示どおりの実装だと
    プローブが**成功する commit** を打ってリポジトリを変える。指示を鵜呑みにせず一次情報で潰したのは正しい。
    spec §5 の条件 22 に「保守的に飛ばす側は許容」を追記済み。
  - planner 独立検証 (coder の報告を再現したもの):
    `MYE_COLLAB_PROBE=cache\m66l_fx` (M 1 + ? 2) で `bin\x64\Debug\Editor.exe --selftest` → exit 0、
    `probe: a commit with an empty index answers with a failure` と
    `probe: a failed commit leaves the message in the box` が **PASS 行で出る**、skip ログ 0 件。
    実行後の fixture は `git log` 1 件・`git status` が実行前と同一 = リポジトリ不変。
    `pwsh -File tools\check_rules.ps1` → 0 error 0 warning。
    `cache\m66l_r2_noremote_win.png` を目視 → 帯が 2 行 (「このリポジトリにはリモートが設定されていません。」)、
    stage のヒントが 3 行で、どちらも最後まで読める。style スタックは早期 return 2 経路とも Pop 対応済み (diff で確認)。
  - nit (直さない。申し送りへ): (a) ja の折り返しが `.meta` の途中で割れる (「…できます。.」/「meta と .terrain.edit」)。
    ImGui の CJK 折り返しは文字境界で切るので避けようがなく、読めているので v1 は許容 (気になるなら文言の
    組み替えは v1.5)。(b) サイドカーだけが worktree 変更されている行 (`x.png.meta` だけ M、本体は無変更) では
    index が空でもプローブが飛ぶ。安全側なので許容するが、reviewer が「なぜか飛んだ」を見たときのために記録。
  - probe の PASS 行数は **未追跡だけの fixture で 14 / staged があると 12** の 2 通りある (reviewer が数えるとき用)。
