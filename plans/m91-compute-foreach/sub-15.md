# sub-15: 規則 14-c の文言 (U17) と、三校 verify.bat の FAIL の切り分け

- 依存: sub-14
- 状態: OK
- 往復: 0
- 新設: 2026-10-12 (sub-14 round 1 の判定から)

## やること
### A. 規則 14-c のエラーの文言 (U17 = エラーのまま。司会が台帳に記録済み)
1. `tools\check_rules.ps1` の 14-c のエラーの文言に、ファイルごとの直し方を入れる。
   - 例: `git rm --cached -q <file>` → `git checkout -- <file>`。ただし、このコマンドはそのファイルの未コミットの変更を消すので、先に add かコミットするよう注意を 1 行添える
   - あわせて、VS が新しいファイルを CRLF で作ることへの対策 (保存のときに LF にする VS の設定、または `.editorconfig` の `end_of_line = lf`) を `docs/line-endings.md` に 1 段落書く。`.editorconfig` を足すかどうかは coder が判断する。足す場合は、VS がそれを守ることを 1 ファイルで確かめて、結果を実装メモに書く (確かめられなければ足さない)
2. 14-c が今までどおり検出し、文言が出ることを確かめる (一時的に CRLF にしたファイルで確かめ、戻す)。

### B. 三校 `tools\verify.bat` の FAIL 2 件の切り分け (コードは変えない)
sub-14 で、shot の golden の不一致 (diffPixels=28402、maxDiff=122) と、replay の「the agent never left patrol」が出た。改行が原因ではないことは対照実験で示されている。ここでは **M91 (エンジンの 7d28c31 より後の変更) が原因かどうか** だけを切り分ける。M91 は ScriptHost の登録、TickRunner、スナップショットを変えたので、replay の FAIL は M91 の退行の可能性を否定できない。
1. エンジンの 7d28c31 (M91 の前) の worktree を作り、Release をビルドする。`MYE_ENGINE` をその worktree に向けて、三校の `tools\verify.bat` を回す。
   - 三校の作業ツリーは clean のまま (f918c7b のまま) にする
   - 三校の `cache\GameLogic.dll` は、エンジンの ABI に合わせて自動で再ビルドされる。終わったら HEAD のエンジンで再ビルドされた状態に戻す (もう一度 HEAD で起動すればよい)
2. HEAD のエンジンでもう一度回す (sub-14 の数値と同じことを確かめる)。
3. 判定:
   - 7d28c31 でも同じ FAIL → M91 とは無関係 (既存の陳腐化か三校側の問題)。記録だけにする
   - 7d28c31 では PASS で HEAD で FAIL → M91 の退行。どの M91 のコミットからかを、M91a〜 の各コミットで 2 分探索して特定し、SELF_EVAL で報告する。直すのは planner の判定の後
   - 片方だけ (shot だけ、または replay だけ) が変わる場合も、項目ごとに同じ判定をする
4. worktree は片付ける。

## やらないこと (このサブでは)
- 三校の golden の撮り直し、三校のゲームのコードの変更 (U18)
- 三校の push

## 受け入れ条件 (このサブ)
1. A: 14-c の文言にファイルごとの直し方が出ること。check_rules が PASS すること
2. B: 7d28c31 と HEAD の両方で verify.bat を回した結果の表 (項目ごとの PASS / FAIL と数値) と、判定が実装メモにあること。三校とエンジンの作業ツリーは、最後に clean になっていること

## 検証コマンド
- `tools\check_rules.ps1`、`git worktree add/remove`、Release のビルド、`C:\HAL\三校\tools\verify.bat`

## コミット
- A の変更だけをコミットする。件名候補: `M91 追補: check_rules 規則 14-c のエラーに直し方を出す`
- B はコードを変えない (退行が見つかって直す場合は、planner の判定の後で別にコミットする)

## 実装メモ (coder が追記)
### round 1
- A: check_rules 14-c のエラーに `git rm --cached -q -- "<file>" ; git checkout -- "<file>"` と、未コミットの変更が消えるので先に add/コミットする旨の WARNING を追加。docs/line-endings.md に VS の CRLF 対策の節を追加。`.editorconfig` は VS の挙動を実機で確かめられないため足していない。ADR-001 を一時的に CRLF にして 14-c の検出と文言を確認し、戻した (check_rules は 0 error)。
- B: 結果 (Release、三校 f918c7b、どちらも warp):

| エンジン | shot | replay |
|---|---|---|
| HEAD (05243cf) | FAIL diffPixels=28402 maxDiff=122 | FAIL "agent never left patrol" (VERIFY PASS は出る) |
| 7d28c31 (dll を ABI 28 で再ビルド後) | FAIL diffPixels=28402 maxDiff=122 | 同じ FAIL |

- 判定: M91 とは無関係 (両項目とも基点でも同一の数値・同一の失敗)。退行なし。なお最初の基点実行は cache\GameLogic.dll が ABI 29 のままで "API version mismatch (dll=29, engine=28)" となり別の失敗 (diffPixels=34541) になった。基点エンジンの Editor を一度 --project で起動して dll を再ビルドさせてから測り直した。
- 片付け: worktree 削除済み、cache\GameLogic.dll は HEAD の Editor 起動で HEAD 向けに再ビルド済み。三校は clean (f918c7b)。


## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-12)。
  - A: 14-c の文言を確かめた。
  - B: 7d28c31 と HEAD で数値が同じだったので、M91 の退行ではない。三校側の別件として記録する (U18)。
  - .editorconfig は足していない (確かめられなかったため。仕様の許す分岐)。
  - nit: docs に書いた VS のメニュー名は未確認。sub-09 で test_checklists に手動の項目を足すときに確かめる。
