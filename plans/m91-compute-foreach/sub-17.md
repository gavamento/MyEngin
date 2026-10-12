# sub-17: net の selftest V1 (LoadPersist / LoadGame) が不安定に落ちる原因を切り分けて直す

- 依存: sub-16 (913067d)
- 状態: 未着手
- 往復: 0
- 新設: 2026-10-12 (U20 のユーザー回答「M91 の中で今直す」。planner 案「M91 の後に別件」の逆)

## これまでの観測 (spec R11)
| いつ | 症状 | 実行 |
|---|---|---|
| sub-10 / sub-11 | `[replay] cannot open ...\cache\server_net_selftest\save_boundary_late.rep` / `save_boundary_full.rep`、`[hashdump] cannot open ...save_boundary_server.dump` | HEAD (M91e〜) で 8 回中 2 回。基点 42c6b89 では 5 回中 0 回 |
| sub-11 (ce452a4) | selftest の側に `WaitUntilReadable` (最大 5 回、合計 750 ms) を足した。製品コードは変えていない | — |
| sub-16 | 「V1 ...: the offline replay of the server .rep matches every tick (verified 0 of 270, reason '...')」が 1 回 FAIL。ファイルは開けているのに、検証できた tick が 0 件 | 最初の Debug の selftest で 1 回。再実行の 4 回は PASS。ビルドと並行していた可能性がある |

- 試験は `src/Engine/Engine/Net/ServerNetSelfTest.cpp` の V1 (1764 行付近の SaveLoadProbe の試験。1797 行付近の `d.name = "V1 LoadPersist / LoadGame in a session"`)。
- `.rep` は `WriteRepFromLog` が書き、`VerifyRepOffline` が別の HeadlessSim で再生する。置き場は `bin\x64\<cfg>\cache\server_net_selftest\`。

## やること
1. **1 回で回せる手段を作る**: V1 (と、同じファイルを使う net の試験) だけを回せるようにする。全体の selftest は 1 回 5〜7 分かかり、統計が取れないため。
   - 例: 環境変数 `MYE_SELFTEST_ONLY=servernet`、または `--selftest` に試験名で絞る引数
   - 製品の挙動は変えない。selftest の入口だけに置く
   - 使い方を実装メモに書く
2. **再現率を測る**: 次の条件ごとに **30 回ずつ** 回し、FAIL の回数と、FAIL の行 (reason の文字列を全文) を表にする。
   - (a) HEAD を単独で
   - (b) HEAD を負荷のもとで: Debug と Release を同時に回す、ビルドと並行する、など。sub-16 の観測が負荷のもとだったため
   - 条件ごとに、3 回 FAIL したらそこで打ち切ってよい
   - 30 回で 1 度も出ない条件は「その条件では再現しない」と書く
3. **基点と比べる**: (a) と (b) のうち FAIL が出た条件を、M91 の前の基点 **7d28c31** (M91 が始まる前) と、**42c6b89** (M91e の前) で、同じ回数だけ回す。worktree で行い、終わったら片付ける。基点には 1 の手段が無いので、全体の selftest を回すか、1 の変更を一時的に当てる。
4. **原因を特定する**: reason の文字列、.rep のサイズと tick 数、書き込みと読み込みの時刻、ファイルのハンドルの寿命 (書き手が閉じる前に読み手が開いていないか)、置き場の共有 (同じパスを別の試験や別のプロセスが使っていないか)、ウイルス対策などの外部要因を、簡単なものから順に確かめる。原因を断定した根拠を実装メモに書く。
5. **直す** (ユーザーの回答により、原因が M91 の変更でなくても、この M91 の中で直す):
   - 原因が製品コード (Replay の書き込み・閉じ方、HeadlessSim など) にあれば、製品コードを直す。決定論とファイル形式は変えない
   - 原因が試験の側 (置き場の共有、後片付け、試験の順序) にあれば、試験を直す
   - 外部要因 (他のプロセスがファイルを掴む) で、コードでは防げないと断定した場合は、その根拠を示し、試験の側でどう守るかを提案して止まる (planner の判定を待つ)
6. **sub-11 のリトライ (`WaitUntilReadable`) の扱いを決める**:
   - 原因の修正で「cannot open」と「verified 0」の両方が説明でき、直ったなら、リトライは**取り除く** (原因を隠す緩和策を残さない)
   - 説明できない症状が残るなら、リトライは残し、そのコメントに「何を防いでいて、何は防げないか」を書く
   - どちらにしたかを実装メモに書く

## 受け入れ条件 (このサブ)
1. 2 と 3 の表がある: 条件 × (HEAD / 7d28c31 / 42c6b89) × (回数・FAIL 数・reason)
2. 原因が根拠つきで断定されている (4)
3. **直ったことの基準**: 修正前に FAIL が出た条件 (再現率がいちばん高い条件) で、修正後に **連続 30 回 FAIL 0 件**
   - 修正前の再現率が 1/30 以下で、30 回では差が示せない場合は、修正前の再現率 p から「直っていないのに 0 件になる確率が 5% 未満」になる回数 (M ≥ ln(0.05) / ln(1 − p)) まで増やす
   - 回数と計算を実装メモに書く
4. 6 のリトライの扱いが決まっていて、理由が書かれている
5. 全体の確認がすべて PASS する: `bin\x64\Debug\Editor.exe --selftest`、`bin\x64\Release\Editor.exe --selftest`、`bin\x64\Debug\Server.exe --selftest`、`tools\replay_verify.bat`、`tools\check_rules.ps1`
6. 原因が M91 の変更だった場合は、どのコミットかを示す。M91 の外だった場合はそう書く (それでも直す)

## 検証コマンド
- 1 で作った絞り込みでの繰り返し実行、`git worktree add/remove`、上の 5 の一式

## コミット
- 件名候補: `M91l: net の selftest (V1) が不安定に落ちる原因を直す`
- 原因の名前が分かったら、件名に入れる (例: `... (.rep を閉じる前に読んでいた)`)

## 実装メモ (coder が追記)

## フィードバック履歴
