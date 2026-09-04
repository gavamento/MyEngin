# sub-11: M66k: review-1 の実害 — 保存失敗で commit しない / 偽 dirty でゲートが閉じない / 本文を失わない / 固まった git の逃げ道

- 依存: sub-01〜sub-10 (全部 OK・コミット済み)
- 状態: OK (round 1、コミット待ち)
- 往復: 1

## やること

出所は `review-1.md` の指摘 1・2・4・8。planner が先に仕様を直したので、**着手前に spec の
§4.1「commit 周り」/ §4.1「未保存ガードの 4 窓」/ §4.4 の「タイムアウト」/ 受け入れ条件 18〜20 /
§7 の末尾 2 項 / §8 の 2026-09-04 の行を読み直すこと**。

1. **[指摘 1] 「保存してコミット」は保存に失敗したら stage も commit もしない。**
   `SourceControlWindow.cpp:1006-1011` が `scm.Commit()` を `if (!saved.empty())` の**外**で呼んでいる。
   契約は `EditorApp.cpp:808-814` (`scmHost.saveDocument`) が「空を返す = 呼び出し側は stage も commit もしない」
   と書いており、保存が失敗する経路は実在する (`EditorApp.cpp:1333` の競合ガード / `:1345` の
   `SceneSerializer::SaveToFile` が false)。今は「既に stage 済みの別ファイルだけが commit される」ので、
   押した人の意図と違う中身が共有履歴に残る。
   → 保存が失敗したら stage も commit もせず、**コミット本文も消さない**。
   保存失敗そのもののトーストは `SaveCurrentScene` / 競合ガードが既に出しているので、ここで二重に出さない
   (競合ガードの `Scm_SaveBlockedConflict` と保存失敗の 2 種類が既にある)。

2. **[指摘 4] コミット本文は成功応答を受けてから消す。**
   `SourceControlWindow.cpp:996-997` と `:1010-1011` が `Commit()` を投げた**直後**に
   `commitMessage_[0] = '\0'` している。`commit` は `nothing_to_commit` / `identity_missing` /
   hook 失敗で普通に失敗する (`tools/collab/src/ops.rs` の commit 経路) ので、書いた本文が復元できない。
   → 成功の観測点は `SourceControlSession::Commit` のコールバック内、`ApplyWriteResult(msg)` が true の枝
   (`SourceControlState.cpp:820-831`)。窓へ返す口は Take 系 1 個で足りる
   (`TakePushed()` / `TakeCreatedBranch()` が同型の前例)。失敗時は本文を残したままエラーを見せる。
   ★round 1 の結果: **Take ではなく既存の `WriteDoneFn` コールバックを採用** (planner が承認)。
     Take だと「どの本文に対する成功か」が渡らず、応答待ちの間に書き換えられた本文まで消える。
     Revert / CreateBranch / Push と同じ形なので前例にも合う。以降はこちらが正。
   ★ commit ボタンは `WriteInFlight()` 中は無効なので、飛んでいる間に本文が書き換わる競合は無い
     (それでも「投げた本文」を控えて、控えた文字列と現在の入力欄が一致するときだけ消す形にすると安全)。

3. **[指摘 2] `assets\input\actions.json` を持たないプロジェクトで書き込み系ゲートが恒久的に閉じる。**
   `ProjectSettingsWindow.cpp:32-34` が窓を開いた最初のフレームで `inputActions_` / `assetsRoot_` を控え、
   閉じても戻さない → 以後ずっと `同:427-437` の `TextDiffersFromDisk(assetsRoot + L"\\input\\actions.json",
   inputActions_->ToJsonText())` が評価される。`DiskCompare.cpp:34-37` は不在時「メモリ側が非空なら未保存」で、
   `InputActions::ToJsonText()` は定義 0 件でも `{"actions": [], "axes": []}` を返す (`InputActions.cpp:325`) =
   必ず非空。`actions.json` を書くのは `ProjectSettingsWindow.cpp:420` の Save だけで
   `ProjectTemplates::CreateProject` は作らない → **新規作成したプロジェクトは全部この状態**で、
   `GateBlocker::ProjectSettingsDirty` が立ちっぱなし = revert / checkout / pull / abort / continue が全部塞がる。
   → spec §4.1 の訂正どおり「ディスクを読み直した既定状態と比べる」方式にする
   (`PhysicsLayerNames::DiffersFromDisk` / `PhysicsLayerNames.h:20-25` が同じ罠を避けている前例)。
   planner の見立て (鵜呑みにしない): `TextDiffersFromDisk` に「ファイルが無いときの相手」を渡せる
   3 引数版を足し、`ProjectSettingsWindow` が `InputActions{}.ToJsonText()` を渡すのが最小。
   `InputActions` 側に判定を持たせても良いが、**Editor 層で閉じること** (規則 12)。
   ★ 他の 3 窓 (Animation / Animator / Mixer) は「アセットが実在する = ファイルも実在する」ので
     この不在ケースには当たらない。同じ 3 引数版を通すかどうかは coder の判断でよいが、
     **既存の挙動 (ファイルがある場合の比較) を変えないこと**。

4. **[指摘 8] 返らない git の逃げ道 (spec §4.4 に追記済み)。**
   `GitTransaction.cpp:842-843` の `Phase::Running` はボタンが 1 つも無いモーダルなので、
   hook 等で git が返らないとエディタ全体が固まる。キャンセルは v1 で「無期限」と決めた線を動かさない。
   → `Phase::Running` に入った時刻を控え、**経過が 15 s (`kStuckHintSec`) を超えたときだけ**
   「戻らない場合はエディタを終了してください。ゲートが全文書の保存を保証しているので、
   失うのは undo 履歴だけです」を出す。常時は出さない (200 ms で終わる commit で毎回出ると警告として読まれない)。
   しきい値の判定は純関数 (例: `bool ShouldShowStuckHint(double elapsedSec)`) に切り出してセルフテストで固定する。
   文言は `LocalizationTable.inl` に en/ja 両方、`###` の右辺は不要 (`TextWrapped` で出すだけ)。

## やらないこと (このサブでは)

- 指摘 3 (ヒント文の折り返し) / 6 (腐ったコメントと未使用アクセサ) / 7 (EndBatch のコメント) /
  9 (`MYE_COLLAB_PROBE` の記載) — **sub-12**。
- 指摘 5 (競合中の保存ガードを 4 窓へ) — planner が却下 (spec §7 に根拠)。**足さない**。
- 書き込み系のキャンセル / 打ち切り (v1 の決定を動かさない)。
- Rust 側 (`tools\collab`) の変更。proto 版も動かさない (フィールドも op も増えない)。
- `spec.md` の本文 (planner の領分)。仕様に穴が残っていたら「不安・質問」で返す。

## 触る場所 (planner の見立て。coder は確認すること)

- `src\Editor\Windows\SourceControlWindow.cpp` — `DrawCommitBox` (996-1013 付近)。指摘 1・4。
- `src\Editor\SourceControl\SourceControlState.{h,cpp}` — `Commit()` の成功を窓へ返す Take 系 1 個。指摘 4。
- `src\Editor\DiskCompare.{h,cpp}` + `src\Editor\Windows\ProjectSettingsWindow.{h,cpp}` — 指摘 2。
- `src\Editor\SourceControl\GitTransaction.{h,cpp}` — `Phase::Running` の経過時間と案内。指摘 8。
- `src\Engine\Core\LocalizationTable.inl` — 新しい文言 (en/ja)。
- `src\Editor\SourceControl\SourceControlSelfTest.cpp` — 受け入れ条件 18/19/20 の固定。
- ファイルは増減しない見込み (増減したら `pwsh -File tools\gen_project_files.ps1`)。

## 受け入れ条件 (このサブ)

1. (spec 18) 「保存してコミット」で保存が失敗する状況を作ると、`stage` も `commit` も飛ばず、
   コミット本文が入力欄に残る。commit 自体が失敗した場合も本文が残る。
   検証: セルフテスト (UI を描かずに通せる形へ切り出して、保存フックが空を返す経路と
   応答が `ok:false` の経路の 2 本) + 実機で 1 回 (競合ガードで保存が止まる状態が作りやすい)。
2. (spec 19) `assets\input\actions.json` を持たない fixture で Project Settings 窓を開いて閉じても
   `GateBlocker::ProjectSettingsDirty` が立たない。アクションを 1 本足すと立ち、Save すると消える。
   検証: セルフテスト 3 ケース (不在 + 既定 = false / 不在 + 編集後 = true / 実在 + 一致 = false) +
   `MYE_COLLAB_PROBE` か実機で「窓を開いた後も Discard が押せる」。
3. (spec 20) 実行中モーダルは 15 s を超えたときだけ回復案内を出す。しきい値の純関数をセルフテストで固定。
4. 既存の受け入れ条件 4 (selftest 全節) / 5 (replay 無変更) が緑のまま。Editor 層だけを触るので
   `replay_verify.bat` は無変更緑であること (sim に触っていない証明)。
5. `check_rules.ps1` 緑 (規則 10 = ローカライズ表の対称性、規則 12 = Editor 層封じ込め)。

## 検証コマンド

```
MSBuild MyEngine.sln /p:Configuration=Debug /p:Platform=x64   (Release も)
cmd /c bin\x64\Debug\Editor.exe --selftest
cmd /c bin\x64\Release\Editor.exe --selftest
pwsh -File tools\check_rules.ps1
tools\replay_verify.bat
pwsh -File tools\collab_fixture.ps1 cache\m66k_fx   (実機。fixture は actions.json を持たない = 指摘 2 の再現条件)
```

`cargo test` / `collab_verify.bat` は Rust を触らないなら不要 (触ったら回す)。
`shot_verify.bat` は描画に触らないので不要 (acoustic 2 枚のフレークを踏むだけ損)。

## 実装メモ (coder が追記)

### round 1 (SELF_EVAL の写し)

```
SELF_EVAL: sub-11 (round 1)
実装:
  - #1 src/Editor/Windows/SourceControlWindow.{h,cpp}:SaveThenCommit — 「保存 -> stage -> commit」の
    3 手を自由関数に切り出し、1 手目が空パスを返したら stage も commit もせず false を返す。
    DrawCommitBox の「保存してコミット」はこれを呼ぶだけになった (保存失敗のトーストは
    SaveCurrentScene / 競合ガード側が出すので二重に出さない。本文も消えない)
  - #4 src/Editor/Windows/SourceControlWindow.{h,cpp}:SubmitCommit / ShouldClearCommitMessage —
    「コミット」「保存してコミット」の両方が SubmitCommit を通る。投げた本文を控え、
    **応答が ok かつ入力欄がその本文のまま**のときだけ空にする
  - #4 src/Editor/SourceControl/SourceControlState.{h,cpp}:Commit — `WriteDoneFn done = {}` を追加
    (Revert / CreateBranch / Push と同じ型。宣言を M66c 節の手前へ移動)。応答の ok/失敗を必ず
    1 回返す。送れなかった 2 経路 (本文が空 / サービス停止) も done(false, ...) で返す
  - #2 src/Editor/DiskCompare.{h,cpp}:TextDiffersFromDisk — 「ファイルが無いときの比較相手」を
    渡す 3 引数版を追加。2 引数版は 3 引数版に `""` を渡すだけ = 既存 3 窓の挙動は不変
  - #2 src/Editor/Windows/ProjectSettingsWindow.{h,cpp}:InputActionsDifferFromDisk — 自由関数に
    切り出し、不在時は `InputActions{}.ToJsonText()` (= ディスクを読み直した既定状態) と比べる。
    HasUnsavedChanges はこれを呼ぶだけ
  - #8 src/Editor/SourceControl/GitTransaction.{h,cpp}:kStuckHintSec / ShouldShowStuckHint /
    runningSince_ — Phase::Running に入った時刻 (ImGui::GetTime = 実時間) を控え、15 s を
    **超えた**ときだけ回復案内を Warning 色 + 折り返しで出す
  - src/Engine/Core/LocalizationTable.inl:Scm_OpStuckHint — en/ja。書式指定子なし、`###` なし
  - src/Editor/SourceControl/SourceControlSelfTest.cpp — (j) 節 15 checks (18/19/20 の固定) +
    MYE_COLLAB_PROBE に 2 checks (実 DLL の commit 失敗 -> 本文を残す)
仕様との差分:
  - [逸脱] #4 の窓への口は「Take 系 1 個」ではなく **既存の WriteDoneFn コールバック**にした。
    理由: Take だと「どの本文に対する成功か」が渡らず、応答待ちの間に書き換えられた本文を
    消してしまう。Push / CreateBranch が同じ形なので前例にも合う (sub-11「やること」2 の
    ★ で planner が求めた「控えた文字列と一致するときだけ消す」も同時に満たす)
  - [追加] Commit の**送れなかった 2 経路**でも done(false, code, detail) を呼ぶようにした。
    元は黙って return しており、窓が「投げた本文」を抱えたまま応答を待つ形になる
  - [追加] MYE_COLLAB_PROBE に「index が空なら commit は必ず失敗する」を使った実 DLL の
    失敗経路検査を 1 本足した (staged が 1 件でもある / マージ途中なら実行しない =
    プローブがリポジトリを変えない約束は維持)
検証:
  - MSBuild Debug / Release -> 両方成功、警告なし
  - cmd /c bin\x64\Debug\Editor.exe --selftest -> exit 0 (44 スイート緑。Source control 節に
    新規 15 件が PASS)
  - cmd /c bin\x64\Release\Editor.exe --selftest -> exit 0
  - MYE_COLLAB_PROBE=cache\m66k_fx Editor.exe --selftest -> exit 0、probe 12 件 PASS
    (新規 2 件込み。fixture の git log は 1 コミットのまま = 変えていない)
  - pwsh -File tools\check_rules.ps1 -> 0 error / 0 warning
  - tools\replay_verify.bat -> exit 0、10 ジョブ全 PASS (76.3s)。**無変更緑**
  - 実機 #2 (故障点での検証): fixture cache\m66k_fx は actions.json を持たない。
    一時プローブで Project Settings 窓を 60 フレーム目に開き 180 で閉じ、ゲートを毎 30 フレーム
    記録 -> **修正後は全フレーム blockers=0**。3 引数版を 2 引数版へ戻して再ビルドすると
    frame 90 以降ずっと gateProjectSettingsDirty=1 / blockers=1 (窓を閉じても解けない)
    = review-1 #2 の再現。ログ cache\m66k_ps.log / cache\m66k_ps_old.log
  - 実機 #1: 競合 fixture cache\m66k_cfx (main.scene.json を両側で変えて merge) で
    「保存してコミット」の 1 手目を一時プローブから実行 ->
    `save blocked: the open document is conflicting` + `conflicted=1 dirtyAfterSave=1
    wentToCommit=0` (STAGED も COMMITTED も出ない)。競合していない fixture では
    `STAGED <path>` + `COMMITTED` + `wentToCommit=1`。ログ cache\m66k_sc_conf.log /
    cache\m66k_sc_ok.log
  - 実機 #8: post-checkout hook で 25 s 返らない checkout を作り、実行中モーダルを連番撮影。
    frame 2400 (Running から約 10 s) は「git を実行しています...」だけ、frame 2880 (約 16 s) で
    回復案内が Warning 色 2 行で出る。cache\m66k_stuck_before15s.png / _after15s.png
  - 実行しなかったもの: cargo test / collab_verify.bat (Rust 無変更)、shot_verify.bat
    (描画無変更。sub-11 の検証コマンド表どおり)
自己採点 (1-5):
  仕様適合: 5 — spec §4.1 commit 周りの 2 項 / §4.1 未保存ガードの訂正 / §4.4 の 15 s を
    そのまま実装。逸脱 1 件は上に明示 (planner の「Take 系」見立てのみ)
  正しさ: 5 — 3 件とも**故障点で再現してから修正後に消えることを観測**した
    (#2 は旧実装へ戻して赤を見た / #1 は競合と非競合の両方向 / #8 は 15 s 前後の 2 枚)。
    未検証は「ja 文言の実画面」と「commit 失敗時の赤字表示」(どちらも既存経路)
  コード品質: 4 — 純関数 3 本に切り出して UI から時間・保存・判定を外した。日本語コメントで
    「なぜ」を残した。SaveThenCommit を SourceControlWindow.h の自由関数に置いたのは
    やや異例 (窓専用の政策なので別ヘッダを増やすより軽いと判断)
  テスト: 5 — セルフテスト新規 15 件 + プローブ新規 2 件。18/19/20 の 3 条件すべてに
    機械検査がある。#2 は旧実装で赤くなることも確認済み
不安・質問:
  - 仕様の穴ではないが 1 点報告: `Commit` の応答が失敗のとき、窓のエラー表示はヘッダの
    赤字 (ErrorText) 任せで、コミット欄の近くには何も出ない。本文が残るようになった今
    「押したのに何も起きていないように見える」余地がある。UI を足すかは planner 判断
    (足すなら sub-12 の範囲外なので新しいサブが要る)。
  - 一時プローブ (EditorApp / GitTransaction の env 分岐) は**削除済み**。恒久で残したのは
    MYE_COLLAB_PROBE 内の 2 件だけ。
触ったファイル:
  - src/Editor/DiskCompare.h
  - src/Editor/DiskCompare.cpp
  - src/Editor/Windows/ProjectSettingsWindow.h
  - src/Editor/Windows/ProjectSettingsWindow.cpp
  - src/Editor/Windows/SourceControlWindow.h
  - src/Editor/Windows/SourceControlWindow.cpp
  - src/Editor/SourceControl/SourceControlState.h
  - src/Editor/SourceControl/SourceControlState.cpp
  - src/Editor/SourceControl/GitTransaction.h
  - src/Editor/SourceControl/GitTransaction.cpp
  - src/Editor/SourceControl/SourceControlSelfTest.cpp
  - src/Engine/Core/LocalizationTable.inl
  - plans/m66-git-collab/sub-11.md (この実装メモ)
申し送り:
  - sub-12 (§14.6) が指す実装: `GitTransaction.h` の `kStuckHintSec` (= 15.0) と
    `ShouldShowStuckHint`。案内の文言は `StrId::Scm_OpStuckHint`。
  - `TextDiffersFromDisk` の 3 引数版は「直列化器が常に非空を返す文書」用。Animation /
    Animator / Mixer は**アセットが実在する = ファイルも実在する**ので 2 引数版のまま
    (既存挙動を変えないという sub-11 の指示どおり)。新しい窓を足す人はどちらを使うか
    ヘッダのコメントで判断できる。
  - 生成物 (gitignore 済み): cache\m66k_fx / m66k_cfx (競合入り) / m66k_hookfx
    (post-checkout が 25 s 眠る) と各 .gitconfig、cache\m66k_*.log、cache\m66k_*.png。
    **m66k_hookfx は checkout が 25 s 掛かる**ので、再利用するなら hook を消すこと。
  - ImGui::GetTime() は `ImGui_ImplWin32_NewFrame` が実時間で進めるので、`--screenshot` の
    固定 dt モードでも**壁時計**のまま (15 s の意味が変わらない)。連番撮影は
    `--shot-every N --shot-realtime`。
```

## フィードバック履歴

- round 1: SELF_EVAL 提出 (coder)
- round 1: VERDICT **OK** (planner)。must 0 / should 0 / nit 2。
  - 逸脱 1 件 (Take → `WriteDoneFn`) は**仕様が甘かった側**として採用。sub-11 の「やること」2 に追記済み。
  - 追加 2 件 (送れなかった commit も done を返す / プローブの失敗経路) はどちらも採用。
    前者は「呼ばないと窓が投げた本文を抱えたまま帰ってこない」= コールバック契約の穴埋め。
  - planner 独立検証: `cmd /c bin\x64\Debug\Editor.exe --selftest` → exit 0、新規 16 行すべて PASS /
    `pwsh -File tools\check_rules.ps1` → 0 error 0 warning / バイナリの更新時刻がソースより新しいこと
    (12:34 > 12:20) を確認 = 提出された検証結果はこのツリーのもの。
  - `phase_ = Phase::Running` は `BeginOp` の 1 箇所だけで、その直後に `runningSince_` を置いている
    (grep で確認) = 案内が古い時刻で誤発火する経路は無い。`Phase::ConflictScan` は読み取り系 (30 s タイムアウト)
    なので恒久的に固まらない = 案内が要らない、で正しい。
  - nit (sub-12 で気が向いたら): (a) セルフテストが作る `%TEMP%\mye_scm_no_dll` を消していない
    (`mye_scm_actions` は消している)。(b) `SaveThenCommit` / `ShouldClearCommitMessage` を
    `SourceControlWindow.h` の自由関数に置いた件は**このままでよい** (窓専用の政策 + テスト可能性)。
  - 質問への回答: commit 失敗時にコミット欄の近くへエラーを**足さない**。`SourceControlWindow.cpp:353-370` の
    ヘッダ赤字は同じ窓の中で常に見える位置にあり、`nothing_to_commit` も `ErrorText` の表に載っている
    (`:68`) ので「何も出ない」状態にはならない。M66f で「押した直後に見る場所を 2 つに分けない」と
    決めた線 (`:472` のコメント) を、同じミルストーンの中で逆向きに崩さない。
