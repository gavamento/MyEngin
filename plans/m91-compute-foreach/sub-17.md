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
SELF_EVAL: sub-17 (round 1) -- 原因は特定できなかった (再現せず)。診断と絞り込みの手段を足し、リトライは残した。

### 絞り込みの手段 (1)
- `MYE_SELFTEST_ONLY=servernet` : `Editor.exe --selftest` が net の試験 (RunServerNetSelfTest) だけを回す (約 85 秒、Debug)。
- `MYE_SELFTEST_ONLY=servernet-v1` : さらに V1 (セーブ境界) だけ (約 35 秒、Debug)。
- 使い方 (PowerShell): `$env:MYE_SELFTEST_ONLY='servernet-v1'; bin\x64\Debug\Editor.exe --selftest`。リポジトリ直下が CWD。環境変数が無ければ従来どおり全体。
- 入口は EditorMain.cpp (selftest 節の先頭) と ServerNetSelfTest.cpp (`OnlyV1`)。製品の挙動は変えない。

### 再現率の表 (2、3)
HEAD = fb66b97 + この変更。FAIL 数は「net の試験の FAIL 行 + 終了コード != 0」。「opened after / cannot open」の WARN・ERROR も 0 件。

| 条件 | 構成 | 回数 | FAIL | 備考 |
|---|---|---|---|---|
| (a) 単独、V1 だけ | Debug | 30 | 0 | |
| (b) 負荷 (Debug と Release を同時 + 論理コア 12 本の CPU 負荷)、V1 だけ | Debug | 30 | 0 | |
| 同上 | Release | 30 | 0 | |
| 新しい exe を初回起動 (Editor.exe を別名で複製して実行)、V1 だけ | Debug | 15 | 0 | 中断 (20 回の予定を 15 回で記録) |
| ビルド直後 (EditorMain.cpp を touch して再リンク → すぐ実行)、V1 だけ | Release | 20 | 0 | |
| Release の再ビルドを 25 回繰り返す負荷のもとで全体の selftest | Debug | 4 | 0 | 8 回の予定を 4 回で中止 (最終コードのビルドのため) |
| 全体の selftest (Debug と Release を同時) | Release | 12 | 0 | |
| 同上、Debug | Debug | 4 + 最初の 1 | 0 | |
| 最終コードで全体の selftest (Debug と Release を同時) | Debug / Release | 1 / 1 | 0 / 0 | 下の確認 |

- 条件 (a) (b) のどれでも FAIL が出なかったので、基点 (7d28c31 / 42c6b89) との比較は行っていない (比べる対象の FAIL が無い。worktree は作っていない)。
- 修正前の再現率が 0 件のため、受け入れ条件 3 (修正前に出た条件で連続 30 回 0 件) は適用できない。過去の観測 (sub-10 / sub-11 / sub-16 の Debug 全体 約 13 回中 3 回) が正しければ、全体の selftest を 8 回以上回して 0 件になる確率は約 12% (0.77^8)。V1 だけの 90 回 + 全体 17 回の 0 件は、「V1 を単独で回す限り再現しない」ことを示すが、「直った」ことは示さない。

### 原因の切り分け (4): 反証できたもの
- 「.rep が閉じる前に読んでいる」: `ReplayRecorder::Finish` の一括モードは局所の `std::ofstream` を関数の終わりで閉じる。呼び出し元が読むのはその後。反証。
- 「CRT の FILE* の枠を使い切っている」: 全体の selftest の V1 の開始時点で fopen("NUL") を数えると 509 本空き (上限 512)。反証 (一時プローブ、コミットしない)。
- 「同じ置き場を別の試験・別プロセスが使う」: `server_net_selftest` を名指しするコードは ServerNetSelfTest.cpp だけ。Server.exe --selftest は別の試験 (ServerSelfTest)。同じ exe フォルダの Editor.exe を同時に 2 本回さない限り衝突しない (今回の 2 並列は Debug と Release で別フォルダ)。
- 「.rep を削除する製品コードがある」: Engine 内の remove / remove_all は CookedCache / DllReloader / HeadlessSim の shadow コピー / PathUtil / ShaderManager の一時ファイルだけで、`cache\server_net_selftest` には触れない。
- 「FileWatcher がファイルを掴む」: 監視は `assets\` とエンジンのシェーダだけ (ReloadHub)。exe フォルダは見ない。
- 「ウイルス対策が掴む」: Defender は有効 (リアルタイム保護・動作監視とも True)。除外は管理者権限が無く見られない。症状 (「ビルド直後の初回に出る」) とは合うが、ビルド直後・初回起動・負荷の再現を 90 回超やっても出なかったので、**根拠は無い (断定できない)**。

### 観察の訂正 (sub-16 の「verified 0 of 270」)
- 270 = `r.log.size() - clientFrom` (540 - 270)。つまり落ちたのは **V6 の「参加途中のクライアントの .rep」の照合** (`client`)。V1 の `full` (540 本) ではない。台帳・spec・sub-17 の観測表の「verified 0 of 270」はこの理解で読む。
- 「ファイルは開けているのに」は未確認。Check の文面に `ready` / `ran` を出していなかったため、Load 失敗 (cannot open) でも同じ文面になりうる。このために文面へ `ready` と `ran` を足した (下)。

### 変更
- `EditorMain.cpp`: `MYE_SELFTEST_ONLY=servernet*` なら net の試験だけを回して終わる。
- `ServerNetSelfTest.cpp`:
  - `OnlyV1()` と、`servernet-v1` のとき V1 以外の scenario を飛ばす。
  - `WaitUntilReadable`: 失敗した回の `file_size` のエラー値と `errno` を覚え、リトライで開けた場合の WARN に足す。5 回とも失敗した場合も WARN を 1 行出す。`file_size` の戻り値を捨てていた C4834 を `(void)` で消した。
  - V1 の `full` / `client` の Check の文面に `ready` / `ran` を足した (Load の失敗と、Load 後の照合の失敗を区別できるように)。

### リトライ (6) の扱い
- **残す**。原因を特定できておらず、「cannot open」と「verified 0」を説明できる修正が無いので、取り除く条件 (原因の修正で両症状が説明できる) を満たさない。
- コメントに「防げるのは、あとで開ける種類だけ。開けた後に中身が違う場合は防げない」と書いた。次に起きたとき、WARN の `file_size error` / `errno`、FAIL の `ready` / `ran` / `reason` で原因を絞れる。

### 確認 (5)
- `bin\x64\Debug\Editor.exe --selftest` → exit 0。`bin\x64\Release\Editor.exe --selftest` → exit 0 (同時実行)。
- `bin\x64\Debug\Server.exe --selftest` → exit 0。
- `tools\replay_verify.bat` → exit 0 (21 ジョブ PASS、236 秒)。
- `tools\check_rules.ps1` → 0 errors (50 warnings、既存の規則 7 の警告)。

### 受け入れ条件との対応
1. 表: 上のとおり (基点の列は、比べる FAIL が無いので無し)。
2. 原因の断定: **できていない**。反証できた仮説と、残っている仮説 (Defender 等のファイルの一時ロック、未確認の非決定性) を上に書いた。
3. 直ったことの基準: **適用できない** (修正前の FAIL が 1 件も再現しなかった)。
4. リトライの扱い: 残す (理由は上)。
6. M91 の変更か: **不明**。V1 の単独実行は再現しないので、M91e の ScriptHost の登録表の差し替えが原因である証拠も、否定できる証拠も得られなかった。基点の 5 回 0 件は有意でない (sub-11)。

### 次の手 (planner の判定を待つ)
- (案 A) 次に出たときの診断だけ待つ (今回入れた WARN / FAIL 文面)。コミットして M91 を閉じ、再発したら別件にする。
- (案 B) 全体の selftest を Debug で 30 回連続 (約 4 時間) 回して再現率を測る。出たらその場で原因を絞る。
- (案 C) 「過去の 3/13 は同じ時間帯に他のエージェントが同じ exe フォルダの Editor.exe や msbuild を動かしていた」ことを疑い、台帳の実行ログから同時実行の有無を確かめる (コードでは検証できない)。
- 触ったファイル: `src/Editor/App/EditorMain.cpp`、`src/Engine/Engine/Net/ServerNetSelfTest.cpp`、`plans/m91-compute-foreach/sub-17.md` (この節のみ)。

### round 2 (FIX_REQUEST: 同じ exe からの同時実行)
**原因を特定した: 同じ exe のフォルダから net の selftest を同時に複数回すと、作業フォルダ `bin\x64\<cfg>\cache\server_net_selftest` の `save_boundary_*.rep` / `.dump` を互いに上書き・削除しあう。** M91 の外の原因 (ServerNetSelfTest.cpp の置き場の作りで、M91e 以前から同じ。基点との比較は不要)。

- 再現 (指摘 1): 同じ `bin\x64\Debug\Editor.exe` から `MYE_SELFTEST_ONLY=servernet` を 4 本同時に起動、10 ラウンド (修正前):

| ラウンド | 1〜10 |
|---|---|
| 失敗したプロセス数 / 4 | 毎回 3 (計 30 / 40)。毎回 1 本だけ PASS |
| 失敗の FAIL 行 | 30 本すべてに `V1 ...: the client .rep's dump at tick 280 exists and equals the server .rep's dump ... (0 lines)` (ERROR `[hashdump] cannot open ...save_boundary_client.dump`) |
| 加えて | 30 本中 12 本で `[replay] cannot open ...save_boundary_full.rep`。リトライ (`WaitUntilReadable`) は 5 回待っても (file_size error 2 = ファイルが無い) 開けなかった |

  「verified 0 of 270」は今回の 10 ラウンドでは直接は出なかった (同じ文面は 0 件)。ただし同じ仮説で説明できる (他のプロセスが同じ名前の .rep / .dump を作り直す・消す途中で読む)。直接の観測ではない。
- 修正 (指摘 2、再現した場合):
  - `CrashRoot()` を `...\cache\server_net_selftest\p<pid>` にした (`_getpid()`)。.rep / .dump / クライアントのクラッシュリング (`rc.crashRoot`) はすべてここ。
  - 終了時に自分のフォルダを `remove_all` で消す (`sims.reset()` の後)。
  - 開始時に `SweepStaleWorkDirs()`: `server_net_selftest\p*` のうち更新が 1 時間より古いものを消す (落ちた実行の残り)。1 時間以内のものは同時に走っている実行かもしれないので残す。
  - sub-11 のリトライ `WaitUntilReadable` と `TestWaitUntilReadable`、その呼び出し 3 か所、`<thread>` / `<cerrno>` の include を**取り除いた**。2 つの症状が同じ原因で説明できるため。round 1 で足した `ready` / `ran` を Check の文面に出す診断は残した (Load の失敗と照合の失敗を区別できる)。
- 直ったことの基準: 同じ条件 (同じ `bin\x64\Debug\Editor.exe` から `MYE_SELFTEST_ONLY=servernet` を 4 本同時) で **連続 30 ラウンド (120 プロセス) FAIL 0 件**。修正前は 10 ラウンドで 30 / 40。
- 注意 (範囲外、申し送り): `HeadlessSim` のセーブフォルダ `<exe>\save_server` (V1 のスロット 7) と `cache\hot_server\i<n>` も同じ exe では共有される。V1 のセーブファイルが消えても、ネット中は LoadGame が no-op なので結果は変わらず、今回の失敗には関与していない。DLL のコピーの方は DllReloader が同時起動を考慮済み。
- 全体の確認 (最終コード): 下の SELF_EVAL に記載。

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-10-12)。
  - must 1: **同じ exe のフォルダから、`MYE_SELFTEST_ONLY=servernet` を 2〜4 本同時に**回し、FAIL が再現するかを確かめる。
    - 理由: 作業ディレクトリ `bin\x64\<cfg>\cache\server_net_selftest` は exe ごとに 1 つで、同時に走るプロセスが同じ .rep / .dump を上書きしあう。過去の FAIL (sub-10 / 11 / 16) は、ほかの作業 (ビルド、ほかの selftest、worktree の実行など) と重なっていた可能性がある。round 1 の「Debug と Release を同時に」は別のフォルダなので、この仮説を試していない。
    - 回数: 同時 4 本 × 10 ラウンドを目安にする。
  - must 2: 再現した場合の扱い。
    - 作業ディレクトリをプロセスごとに一意にして直す。例: `server_net_selftest\<pid>` を作り、終わったら消す。
    - 直ったことの基準: 同じ条件 (同時 4 本) で連続 30 ラウンド FAIL 0。
    - 2 つの症状 (cannot open と verified 0) が説明できれば、sub-11 のリトライを取り除く。
    - 基点との比較は要らない。M91 の外の原因と分かるため。
    - 件名候補: `M91l: net の selftest の作業フォルダをプロセスごとに分ける (同時実行で .rep を上書きしあっていた)`
  - must 3: 再現しなかった場合の扱い。
    - 案 A (診断を足したうえでコミットし、再発したら直す) とする。
    - それでも、作業ディレクトリをプロセスごとに一意にする変更は入れる。同時実行で壊れうる構造は、それ自体が欠陥で、AGENTS.md 3.4 の「障害の局所化」に当たるため。
    - リトライは残し、そのコメントのとおりとする。
    - 件名候補: `M91l: net の selftest の作業フォルダをプロセスごとに分け、不安定な FAIL の診断を足す`
  - 観測の読み方の訂正 (270 は V6 の参加途中のクライアントの .rep) を受け入れ、spec の R11 を直した。
- round 2: VERDICT OK (planner、2026-10-12)。同時 4 本で再現し (修正前 30/40 が FAIL)、作業フォルダをプロセスごとに分けて直った (30 ラウンドで FAIL 0)。リトライは取り除いた。`save_server` と `hot_server` は U21 として記録した。
