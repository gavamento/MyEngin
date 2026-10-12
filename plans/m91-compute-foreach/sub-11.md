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

## 追加 (U13、ユーザー回答 2026-10-12「いま足す」)
net の試験の `cannot open` (spec R11) に対して、**selftest の側だけ**にリトライを足す。
- 置き場所: `src/Engine/Engine/Net/ServerNetSelfTest.cpp`。`.rep` を `VerifyRepOffline` に渡す前と、hash dump (`*.dump`) を読む前に、小さな補助関数 (例: `WaitUntilReadable(path)`) を呼ぶ。製品コード (`Replay.cpp` の `ReplayPlayer::Load`、hashdump の読込) は変えない (製品の振る舞いと、ほかの呼び出し元に影響させないため)
- 判定: 読み取りで開けたら成功 (`std::ifstream` か `CreateFileW(GENERIC_READ, FILE_SHARE_READ)`)。開けたハンドルはすぐ閉じる
- 回数と待ち方: 最大 5 回まで試す。失敗したら待って次を試す。待ち時間は 50 → 100 → 200 → 400 ms (合計で最大 750 ms)。selftest の中の壁時計の待ちなので、sim の決定論には影響しない
- ログ: 1 回目で開けたときは何も出さない。リトライしたときは、試した回数と経過 ms とパスを `MYE_LOG_WARN` で 1 行出す (例: `[selftest] opened after 3 tries (150 ms): <path>`)。これで不安定さの頻度をあとから追える。5 回とも失敗したときは、そのまま既存の経路へ進め、今までどおり `cannot open` の ERROR と試験の FAIL を出す (失敗を隠さない)
- ファイルが存在しない場合も同じ扱いにする (書き終わる前に読みに行く可能性も、同じ待ちで吸収する)
- 補助関数の確認: 存在しないパスに対して 5 回試して false を返し、合計の待ち時間がおよそ 750 ms で収まることを selftest に 1 項目足す (待ち時間を引数で縮められるようにしてもよい)

## やらないこと (このサブでは)
- 製品コード側のリトライ

## 受け入れ条件 (このサブ)
1. HEAD と基点の再現率の表 (回数・FAIL 数・落ちた試験名)
2. 退行だった場合: 修正後に HEAD で N 回連続 PASS (Debug)、Release の selftest が 1 回 PASS、`bin\x64\Debug\Server.exe --selftest` が PASS、`tools\replay_verify.bat` が PASS
3. (結論済み: 既存の不安定。R11)
4. (U13) 上の「追加」節のリトライが入っている。補助関数の selftest 項目が PASS する。Debug の `Editor.exe --selftest` が PASS する (いまの作業ツリーと、新規 CRLF worktree の両方。round 1 の must 2 と合わせて 1 回で確かめてよい)。`bin\x64\Debug\Server.exe --selftest` が PASS する

## 検証コマンド
- `bin\x64\Debug\Editor.exe --selftest` (繰り返し)、基点のビルド、`Server.exe --selftest`、`tools\replay_verify.bat` (修正した場合)

## コミット
- round 2 で CRLF の修正を行うので、コミットする。件名候補: `M91 追補: CRLF のチェックアウトでカーネルの selftest が落ちるのを直し、net の selftest にファイルを開くリトライを足す` (既存の「M76 追補」と同じ形。後続のサブの文字は変えない)

## 実装メモ (coder が追記)
SELF_EVAL: sub-11 (round 1) -- コード変更なし。記録のみ。

再現率 (Debug の `Editor.exe --selftest` 全体。net だけを回す手段は無かったので全体を毎回回した。1 回約 7 分):

| ビルド | 回数 | net 試験の FAIL | 落ちた試験 |
|---|---|---|---|
| HEAD ff60f77 | 6 (最初の 1 回 + 5 回) | 1 (最初の 1 回) | V1 の 2 件: ".rep whose start tick is outside its record range FAILS" と "client .rep's dump at tick 280 exists and equals..." |
| 基点 42c6b89 (worktree、Debug 全ビルド) | 5 | 0 | なし |

- 基点の 5 回は毎回 `CRLF 化してもハッシュは同じ (正規化)` の 1 件だけ FAIL (exit 1)。net とは無関係で、worktree の checkout 形式 (改行) の違いによるものと推測 (未確認)。HEAD の作業ツリーでは出ない。
- sub-10 で観測した分 (2 回中 1 回 FAIL) を足すと HEAD は 8 回中 2 回、基点は 5 回中 0 回。差は統計的には判別できない (Fisher でおよそ p=0.5)。
- FAIL の機序 (HEAD の 1 回目のログ `[ERROR] [replay] cannot open ...\cache\server_net_selftest\save_boundary_late.rep` と `[hashdump] cannot open ...save_boundary_server.dump`): 直前に `ReplayRecorder::Finish` が書き終えて「recorded 270 ticks」まで出た .rep を、`ReplayPlayer::Load` (Replay.cpp:204-207、`file_size` か `ifstream` のどちらか) が開けなかった。さらに同じ試験で、約 2000 行前に正常に読めた save_boundary_full.rep も 2 回目の読みで開けなかった。ハッシュ不一致・クラッシュ・dangling ではなく、ファイルを開く失敗のみ。ScriptHost / construct の差し替えとは経路が違う。
- 簡単な原因の確認: 試験は bin\x64\Debug\cache\server_net_selftest を共有する単一プロセスで、並行の他プロセスは走らせていない (自分で確認)。したがってセーブ置き場の競合ではなく、書いた直後のファイルを Windows 側 (ウイルス対策のスキャンや索引) が一時的に掴んだ、という見立て。実証はしていない (失敗した run が最初の 1 回だけで、再現できなかった)。
- 失敗した HEAD の 1 回目は、直前のビルドの直後 (Editor.exe 初回実行) だった。基点の 1 回目 (ビルド直後) は落ちなかったので、ビルド直後が条件とは言い切れない。

結論: 退行と断定する証拠は無く (機序が M91e と無関係、基点との差は有意でない)、受け入れ条件 3 に従い、コードは変えていない。M91e の退行を完全には否定できないので、ユーザー判断として次を提案する: (a) このまま M91 を進め、再度落ちたら ServerNetSelfTest の `VerifyRepOffline` 前に開き直し (数回リトライ) を足す、(b) 先にリトライを足す。

仕様との差分:
- [追加] spec 7.2 に載せる案: 「Debug selftest の net 試験 (V1) が稀に 'cannot open' で FAIL する。基点 5 回 0 件 / HEAD 8 回 2 件で有意差なし。機序はファイルを開く失敗で ScriptHost と無関係と見る」
- [追加] 基点のビルドに使った worktree (C:\HAL\MyEngin_base) は `git worktree remove` で片付け済み。

触ったファイル: plans/m91-compute-foreach/sub-11.md のみ (ソース変更なし)。

SELF_EVAL: sub-11 (round 2)
- #1: KernelLintSelfTest.cpp の CRLF 化ラムダが、入力の CR を落としてから CR+LF を足す形に直した (CRCRLF にならない)。
- #2: 新規 worktree (HEAD、autocrlf=true、.hlsl / Kernels.gen.* が CRLF であることを `file` で確認、未コミット修正はパッチで反映) の Debug 全ビルド + `Editor.exe --selftest` で、上の 1 件に加えて `Kernels.gen.h/.cpp が PowerShell 版と一致する` の 2 件が FAIL した。原因は StripBanner が CRLF のコミット済み生成物と LF の生成器出力を比べていたこと。StripBanner に CR 除去を足した (.gitattributes は変えない。理由: 比較の側で正規化すれば生成物の checkout 形式に依存せず、既存の規則にも触れない)。Kernel の golden とハッシュの比較 (D17) は正規化済みで落ちなかった。
- #4: ServerNetSelfTest.cpp に `WaitUntilReadable` (最大 5 回、50→100→200→400 ms、リトライ時だけ MYE_LOG_WARN) を足し、VerifyRepOffline の冒頭 (.rep) と ReadHashDump の前 (*.dump 2 本) で呼ぶ。5 回失敗なら従来の経路。試験 `TestWaitUntilReadable` (存在しないパスは false で 700-1500 ms、存在するファイルは 50 ms 未満で true) を追加。製品コードは変更なし。
- 検証: 最終コードで 現在の作業ツリー (LF) と新規 CRLF worktree の両方で Debug の `Editor.exe --selftest` が exit 0、`Server.exe --selftest` も両方 exit 0。worktree は片付け済み。Release selftest / replay_verify は未実行 (退行の修正ではなく selftest だけの変更のため)。
- 仕様との差分: [追加] 指摘 2 の過程で見つけた StripBanner の CR 除去 (selftest のみ)。

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-10-12)。net の不安定さの切り分けの結論 (M91 の退行ではない。R11) は受け入れた。切り分けの途中で見つかった CRLF の問題 (R12) を must として、このサブで直す。
- round 2: VERDICT OK (planner、2026-10-12)。must 2 件と U13 のリトライは解消した。round 1 の実装メモにある「worktree 固有」という見立ては誤りで、round 2 の結論 (CRLF でチェックアウトされたことによる問題、R12) が正しい。should: `TestWaitUntilReadable` の上限 1500 ms は遅い CI で揺れうるので、sub-12 で上限をなくす (下限だけ見る) か、上限を十分に広げる。
