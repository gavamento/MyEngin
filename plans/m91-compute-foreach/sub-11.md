# sub-11: Debug selftest の net 試験 (V1 LoadPersist / LoadGame) の不安定さの切り分け

- 依存: なし (sub-10 の後、sub-06 の前に行う)
- 状態: 未着手
- 往復: 0
- 新設: 2026-10-12 (sub-10 round 1 の SELF_EVAL で、Debug の `Editor.exe --selftest` の 1 回目に「V1 LoadPersist / LoadGame in a session」(`src/Engine/Engine/Net/ServerNetSelfTest.cpp:1797`) の 2 件が FAIL し、2 回目は PASS した)

## やること
疑わしいのは M91e (92bde8a) の変更。M91e は ScriptHost::LoadModule の isNew 経路で、プロセス共有の登録表の construct を、最後に読んだ DLL へ差し替える (spec 7.2 R10)。この試験は複数の HeadlessSim (それぞれ GameLogic.dll を持つ) を同時に動かす。なので M91 で持ち込んだ退行かどうかを、先に切り分ける。

1. **再現率を測る**: HEAD で `bin\x64\Debug\Editor.exe --selftest` を N 回 (目安 5 回。時間が許せば 10 回) 回し、FAIL の回数と、どの試験が落ちたかを記録する。net の試験だけを回す手段があればそれを使ってよい。その場合は手段を書く。
2. **基点と比べる**: 同じ回数を M91e より前 (42c6b89) でも回す (worktree か一時的な checkout を使う。作業ツリーの変更を壊さないこと)。
   - 基点でも同じ率で落ちる → 既存の不安定。M91 の範囲外として記録し、コードは変えない
   - HEAD だけで落ちる → M91e の退行。原因を特定して直す
3. **退行だった場合の直し方** (見立て。coder が確かめること): 同じ名前の型の construct などの関数ポインタを、プロセス共有の登録表に置かない。ScriptHost ごと (= 読んだ DLL ごと) の表から引く形にする。あるいは、DLL を解放するときに、その DLL を指す登録だけを、まだ生きている別の ScriptHost の DLL へ戻す。どちらにしたかと理由を実装メモに書く。
4. 落ちたときの観測 (ログの FAIL 行、例外があれば dbghelp で RVA から file:line を出す。memory `crash-triage-recipe.md`) を実装メモに残す。セーブファイルの置き場がほかの試験やプロセスと衝突していないか (ファイルの競合) も、簡単な原因として先に確かめる。

## やらないこと (このサブでは)
- 既存の不安定だった場合の修正 (記録だけ。直すかどうかはユーザーの判断)

## 受け入れ条件 (このサブ)
1. HEAD と基点の再現率の表 (回数・FAIL 数・落ちた試験名)
2. 退行だった場合: 修正後に HEAD で N 回連続 PASS (Debug)、Release の selftest が 1 回 PASS、`bin\x64\Debug\Server.exe --selftest` が PASS、`tools\replay_verify.bat` が PASS
3. 既存の不安定だった場合: コード変更なし。再現率の表と見立てを実装メモに書き、spec 7.2 に載せる案を「仕様との差分」に出す

## 検証コマンド
- `bin\x64\Debug\Editor.exe --selftest` (繰り返し)、基点のビルド、`Server.exe --selftest`、`tools\replay_verify.bat` (修正した場合)

## コミット
- 修正したときだけコミットする。件名候補: `M91 追補: 複数の sim が同時に生きているときのスクリプト登録の dangling を直す` (既存の「M76 追補」と同じ形。後続のサブの文字は変えない)

## 実装メモ (coder が追記)

## フィードバック履歴
