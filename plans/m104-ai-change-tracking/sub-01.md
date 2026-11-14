# sub-01: 試作 6 件と設計文書への反映 (M104b)

- 依存: なし
- 状態: 未着手
- 往復: 0

## やること

設計が前提にしているが確かめていないことを 6 件、最小の試作で確かめ、結果を設計文書へ反映する。
**このサブのコミットは文書だけ。** 試作コードはリポジトリの外 (`%TEMP%\mye_m104b\`) に作り、コミットしない (spec §2 D2)。

各試作は「実行の前に期待する結果を書く → 実行 → 観測と照合」の順で行い、予測と違った点を必ず記録する (AGENTS.md §6 の 5)。

| # | 確かめること | 最小の試作 | 外れた場合 |
|---|---|---|---|
| 1 | Rust から、現在のユーザーだけに許可する DACL と `PIPE_REJECT_REMOTE_CLIENTS` を付けた名前付きパイプを張り、C++ の overlapped I/O (スレッドなし、`Poll()` 相当) のクライアントと 1 行 1 JSON を往復できる。`GetNamedPipeClientProcessId` で相手の PID が取れる。Win32 API は `extern "system"` の自前宣言 (`C:\HAL\MyEngin\tools\collab\src\git.rs` の前例) と `windows-sys` のどちらが保ちやすいか | Rust の小さい bin (サーバ) と、`cl` で作る単体の C++ exe (クライアント)。1 MiB 近い行と、複数接続も 1 回ずつ流す | 手段を変える。責務の分け方は変えない |
| 2 | `rusqlite` (bundled) が開発機 (rustc 1.98) でビルドでき、WAL で開いて読み書きできる。ビルド時間と `target` の大きさも記録する。**CI は確かめない** (spec §2 D3。「未検証」のまま残し、sub-05 で確かめる) | 小さい bin。edition 2021 で通る版を特定する | ビルド済みの SQLite を置く、または別の保存形式。結論を出さずに「不安・質問」へ |
| 3 | `notify` 8 系 (`default-features = false`) が、(a) 短い間隔の連続書き込み (b) 削除 (c) リネーム (d) 「一時ファイルへ書いて rename」(`C:\HAL\MyEngin\src\Engine\Platform\PathUtil.h` の `WriteFileReplacing` と同じ手順) (e) ディレクトリごとの移動 (f) 大量 (1000 ファイル) の一括変更、をどういうイベント列で返すか | 小さい bin で一時ディレクトリを監視し、イベント列をそのまま記録する。`C:\HAL\MyEngin\tools\collab\src\watch.rs` の使い方を先に読む | arch §4 のデバウンスの幅と、§5 のリネームのまとめ方を直す |
| 4 | C# コンポーネントのフィールドを `FieldFromJson` (`C:\HAL\MyEngin\src\Engine\Core\Util\JsonUtil.h`) で書けるか。書けない場合、managed 側のフィールドを Editor から書く既存の経路があるか (`InspectorWindow.cpp` の `DrawManagedComponentFields` が何を呼んでいるか) | まずコードを読んで結論を出す。読むだけで決まらなければ一時プローブで実走する (プローブは**自分が足した変更だけ**を元へ戻し、コミットしない) | C# コンポーネントは Entity 単位の復元だけにする (restore §4.2) |
| 5 | フックの入力と設定。Claude Code 分は planner が公式文書で確認済み (spec §2 D14 の内容と URL をそのまま文書へ写す)。Codex CLI / Gemini CLI は spec §2 D15 の URL を読めれば結果を書き、読めなければ「未確認」と書く (**確認済みのように書かない**) | 文書の確認だけ。加えて、D14 の形の JSON を標準入力から読む最小の Rust コードで、Edit の `old_string` / `new_string` / `replace_all` を Before に当てて After のハッシュを計算できることを確かめる | 無いクライアントは Claimed 止まり (attr §6.1) |
| 6 | 親と運命を共にしないか (spec §2 D10)。(a) PowerShell から起動した親 → 子 (b) Claude Code が起動した MCP サーバ相当の親 → 子、で親を終了させた後に子が生きているか。子が Job に入っているかを `IsProcessInJob` で見る。`CREATE_BREAKAWAY_FROM_JOB` / `DETACHED_PROCESS` の効き方も見る | 試作 1 のサーバを子として起動する小さい親。(b) が自動で試せなければ、試せた範囲と試せなかった理由を書く | 縮退を arch §3 に書く: 親と一緒に終わったら、次の起動で `service_down` の欠落を記録する |

## やらないこと (このサブでは)

- `tools\track\` の作成、`Cargo.toml` の追加、`check_rules.ps1` / `.gitignore` の変更 (sub-02 以降)。
- リポジトリ内への試作コードの配置。既存のソースの変更 (試作 4 の一時プローブは元へ戻す)。
- CI への push、ワークフローの変更。
- 設計の責務の分け方 (ADR-039 の決定 1〜11) の変更。試作の結果で変えてよいのは実現手段だけ。責務まで変える必要が出たら、文書を直さずに「不安・質問」で planner へ返す。
- `C:\HAL\MyEngin\plans\m104-ai-change-tracking\spec.md` の編集 (planner の持ち場)。後続サブの仕様が変わる結果は SELF_EVAL の「申し送り」に書く。

## 触る場所 (planner の見立て)

- 新規: `C:\HAL\MyEngin\docs\ai-change-tracking\prototype-results.md` — 試作ごとに「期待 / 手順とコマンド / 観測 (出力の引用) / 結論 / 未確認の残り / 設計への反映先」。環境 (OS、rustc、cl の版) を冒頭に書く。開発機の絶対パスは書かない (規則 17 が警告する)。
- `C:\HAL\MyEngin\docs\ai-change-tracking\architecture.md` — §4 末尾と §5 の「未検証」(試作 3)、§3 (試作 6)。
- `C:\HAL\MyEngin\docs\ai-change-tracking\ipc-contract.md` — §1 末尾の「未検証」(試作 1。採った Win32 の呼び方を書く)。
- `C:\HAL\MyEngin\docs\ai-change-tracking\db-schema.md` — §8 末尾の「未検証」(試作 2。CI は未検証のまま残す)。
- `C:\HAL\MyEngin\docs\ai-change-tracking\attribution-rules.md` — §6.1 の表 (試作 5)。終了コード 2 を返さない、`timeout` を明示する、`tool_use_id` で対にする、を追記。
- `C:\HAL\MyEngin\docs\ai-change-tracking\restore-safety.md` — §4.2 末尾の「未検証」(試作 4)。
- `C:\HAL\MyEngin\docs\adr\ADR-039-ai-change-tracking.md` — 決定 2、3 の実現手段が変わったときだけ。状態の行は「設計確定・実装中」にしてよい。
- `C:\HAL\MyEngin\plans\m104-ai-change-tracking.md` — 進捗表の M104b を「試作 6 件」にし状態を更新、「試作で確かめること」に 6 を足して結果の要約、「予約する番号」「申し送り」。

## 受け入れ条件 (このサブ)

1. `prototype-results.md` に 6 件すべての節があり、それぞれに実行したコマンドと観測した出力がある。実行できなかったものは「未実行」と理由が書いてある → 文書の目視。
2. 設計文書 5 本の「未検証」の段落が、結果 (確認済み / 変更した / 未確認のまま) で置き換わっている。残っている「未検証」は、理由と、どのサブで確かめるかが書いてある → `未検証` を grep して 1 件ずつ確かめる。
3. 試作 1〜3 について、sub-02〜05 が使う依存の版と API の呼び方が 1 つに決まっている (「どちらでもよい」で終わらせない) → 文書の目視。
4. 計画の「試作 3 件」が直り、試作 6 が表にある → 差分。
5. `git status` に出るのは上の文書だけ。`tools\track\` も試作コードも無い。既存のソースに差分が無い → `git status --short`、`git diff --stat`。
6. 文書の相対リンクが切れていない → リンク先の存在確認。

## 検証コマンド

- `git status --short` / `git diff --stat ca6a3991`
- `pwsh -NoProfile -File tools\check_rules.ps1` (規則 14 の改行、規則 17 の絶対パスの警告を見る。文書だけの変更なのでエンジンのビルドは不要。AGENTS.md §7)
- 試作そのものの実行コマンド (結果の文書に書いたもの)

## 実装メモ (coder が追記)

SELF_EVAL: sub-01 (round 1)
実装:
  - docs\ai-change-tracking\prototype-results.md (新規) — 試作 6 件の期待 / 手順 / 観測 / 予測と違った点 / 結論 / 未確認の残り。環境 (OS・rustc・cl・依存の版) を冒頭に
  - docs\ai-change-tracking\ipc-contract.md §1 — 「未検証」を確認済み + 決定 (windows-sys 0.61、DACL / pipe の呼び方、ERROR_PIPE_BUSY の再試行) に置き換え
  - docs\ai-change-tracking\db-schema.md §8 — rusqlite 0.42 / sha2 0.11 / rust-version 1.89 を決定。CI は「未検証」のまま理由と sub-05 を書いた
  - docs\ai-change-tracking\architecture.md — §2.2 依存に windows-sys、§3 (試作 6 の結果・縮退・起動の決定)、§4 末尾 (notify の観測とデバウンス等の具体値)、§5 (リネームのまとめ方)、冒頭の用語
  - docs\ai-change-tracking\attribution-rules.md §6.1 — Codex / Gemini は未確認と明記、Claude Code の入力の表、制約 3 点、宣言の適用規則
  - docs\ai-change-tracking\restore-safety.md §4.2 — C# は FieldFromJson で書けない (コードで確定)、別の分岐の決定、未検証は sub-16
  - docs\adr\ADR-039-ai-change-tracking.md — 状態を「設計確定・実装中」、決定 2 / 3 と影響に windows-sys・rust 1.89、末尾に試作の結果への参照
  - plans\m104-ai-change-tracking.md — M104b を「実装済み・判定待ち」/ 試作 6 件、試作表に 6 と結果列、予約する番号 (依存の版・pipe / mutex 名)、申し送り
仕様との差分:
  - [逸脱] 試作 4 は一時プローブで実走していない — sub-01 が「読むだけで決まらなければ実走」としており、読んで決まった (FieldDesc が無い = FieldFromJson は使えない、書く既存経路 2 つを特定)。ただし「1 キー JSON で他のフィールドが変わらない」「Undo で戻る」は読んだ推測のままで、「未検証」に sub-16 を添えて残した
  - [逸脱] 試作 6 (b) は、本物の Claude Code に MCP サーバを起動させて親を終わらせる実験をしていない — 実行すると自分が動いている claude.exe ごと終わるため。代わりに本物の mye_mcp.exe が Job に入っていること (IsProcessInJob) だけ観測した。Job の性質 (kill-on-close か、BREAKAWAY_OK か) は QueryInformationJobObject が ACCESS_DENIED で読めなかった
  - [逸脱] 試作 5 の Codex CLI / Gemini CLI は未確認のまま — coder に外部ページを読む手段が無い (sub-01 が許す扱い)。確認済みのようには書いていない
  - [追加] 試作 3 に補助の bin を 4 本足した (latency / sharecheck / renames / moveout) — 最初の記録だけでは、到着時刻 (集める側の時刻が実際の到着ではなかった)・書き込み中の見分け方・大量リネームの対応・移し出しが分からなかったため。リポジトリには入れていない
  - [追加] 設計文書へ決定として次を書いた (planner の確認が要る): デバウンス 300 ms / 最大 2 秒 + 共有違反で書き込み中を見分ける + 30 秒で打ち切り coalesced (arch §4)、MCP からの起動は CREATE_BREAKAWAY_FROM_JOB を試して ERROR_ACCESS_DENIED なら外す (arch §3)、tools\track に rust-version = "1.89" (db-schema §8)。いずれも「初期値、sub-03 などで調整」と書いた。責務の分け方 (ADR 決定 1〜11) は変えていない
  - [追加] ADR-039 の決定 2 / 3 と「影響」に windows-sys を依存として足した (実現手段の変更に当たるため。sub-01 の「決定 2、3 の実現手段が変わったとき」)
検証:
  - 試作 1: p1_pipe (Rust) + p1_client (cl、/utf-8) 実行 → 100 本 + 1,048,000 バイトの行が 46.9 ms で全一致、maxPoll 0.2 ms、client_pid 一致、\\localhost\pipe\ はフラグありで err=5 / なしで VALID、2 本目のサーバは err=5 (FIRST_PIPE_INSTANCE)、Get-Acl の読み戻しは自分の SID の FA だけ。予測外れ 1 件 (2 本目の接続が ERROR_PIPE_BUSY 231) → クライアントに再試行を入れて通した
  - 試作 2: cargo build / --release / 実行 → bundled ビルド成功 (clean debug 8.8 s、target 97 MiB)、WAL・外部キー・別接続読み・integrity_check ok。予測外れ 1 件 (sha2 0.11 は {:x} 不可)、MSRV 1.89 を発見
  - 試作 3: p3_notify / latency / sharecheck / renames / moveout を実行 → 文書の観測表のとおり。予測外れ 4 件 (開いたままの書き込みは閉じるまで届かない、別ディレクトリ移動は Remove+Create、置き換え rename に Remove が混じる、ディレクトリ単位の操作は中のイベントが来ない)。バッファあふれは再現せず
  - 試作 4: 実走なし。InspectorWindow.cpp / ManagedHost.cpp / ScriptRuntime.cs / SceneSerializer.cpp / JsonUtil.cpp を読んで確定
  - 試作 5: p5_hook + PowerShell 駆動スクリプトを実行 → 当てられる 5 件で declared_after と別実装の After が一致、当てられない 3 件は無変更と一致、壊れた入力 4 種は exit 0 / stdout 空、約 19 ms。After を作ったのは Claude Code ではなく試作スクリプト
  - 試作 6: jt.exe host で 4 種の Job × 4 種のフラグ = 16 通りを実行 → 予測すべて一致。jt jobinfo で本物の mye_mcp.exe が Job 内と観測。selfjob の Job 照会は ACCESS_DENIED
  - git status --short → 変更は docs 6 本 + plans 1 本 + 新規 prototype-results.md のみ (ほかは司会管理の plans\m104-ai-change-tracking\)。git diff --stat -- src tools は空。tools\track も試作コードも無い
  - pwsh -NoProfile -File tools\check_rules.ps1 → 0 error、60 warning (すべて既存。自分が触った文書への規則 14 / 17 の警告なし)
  - 文書の相対リンク → 触った 8 ファイルの ](...) を全部 Test-Path で確認、切れなし
  - grep 未検証 → 設計文書 5 本に残るのは 4 件 (arch §3 の本物のクライアント終了と Editor の Job、arch §4 のバッファあふれ、db-schema §8 の CI、restore-safety §4.2 の C# Field の実走)。すべて理由と確かめるサブを併記。ipc-contract §1 は「未確認」の段落 (別ユーザー・本物のリモート・Editor 実装 = sub-09)
  - selftest / replay_verify は未実行 (src\ を触らないサブなので、司会の指示どおり)。エンジンのビルドも未実行 (文書のみ)
自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 1〜6 を満たした。ただし試作 4 は実走なし、試作 6 (b) と試作 5 の Codex / Gemini は一部未実施で、いずれも理由と残りを書いた
  正しさ: 4 — 試作 1・2・3・5・6(a) は実行して観測した。試作 4 は読んだ推測を含み、試作 5 の After は本物の Claude Code 由来ではなく、試作 6(b) は決着していない
  コード品質: 4 — 文書のみ。試作コードは対象外 (リポジトリ外)。文書は確認済み / 決定 / 未検証を分けて書いた
  テスト: 4 — 全試作に期待を先に書いて照合した。CI での bundled と本物のフック入力は未確認のまま残る
不安・質問:
  - 試作 6 (b): Job の性質が読めず、本物のクライアントでの生存実験もしていない。arch §3 の起動の決定 (BREAKAWAY を試して外す) は合成した Job の実測に基づく。sub-05 / sub-07 で本物を使って確かめる前提でよいか (planner)。
  - rusqlite 0.42 / sha2 0.11 が最低 Rust 1.89 を要求する。collab の「古い stable でも通る側に倒す」方針とは異なる。rust-version = "1.89" を明示する決定で足りるか、古い版 (MSRV の低い rusqlite など) へ固定する方向を取るか (planner / ユーザー)。
  - 設計文書へ書いた具体値 (デバウンス 300 ms / 2 秒、共有違反の再試行 250 ms・30 秒) は初期値。sub-03 の仕様に取り込むかは planner の判断。
  - ClaudeCode の Edit がこの環境で CRLF や引用符をどう扱うかは実測していない (declared_edit_match が CRLF で外れる可能性。外れても Claimed 止まりの安全側)。
触ったファイル:
  - docs\ai-change-tracking\prototype-results.md (新規)
  - docs\ai-change-tracking\architecture.md
  - docs\ai-change-tracking\ipc-contract.md
  - docs\ai-change-tracking\db-schema.md
  - docs\ai-change-tracking\attribution-rules.md
  - docs\ai-change-tracking\restore-safety.md
  - docs\adr\ADR-039-ai-change-tracking.md
  - plans\m104-ai-change-tracking.md
  - plans\m104-ai-change-tracking\sub-01.md (実装メモ節のみ)
申し送り:
  - sub-03 (ファイル監視): arch §4 の決定と prototype-results 試作 3 の結論 1〜7 を仕様に取り込む。バッファあふれを人工的に起こす検証と、ディレクトリ単位の操作の突き合わせを受け入れ条件に。flush の FILE_SHARE_READ 試行は Rust の OpenOptionsExt::share_mode(1)
  - sub-02 / sub-05: tools\track\Cargo.toml は edition 2021 + rust-version = "1.89"、windows-sys 0.61 (feature 7 つ)。pipe クライアントの ERROR_PIPE_BUSY 再試行、サーバ最初の 1 本は FILE_FLAG_FIRST_PIPE_INSTANCE。sub-05 の CI で bundled のビルドを確かめる (CC 指定が要るかもしれない)
  - sub-08 (フック): 終了コード 0 固定・stdout 空・timeout 明示・tool_use_id で対。本物の入力の実測をシナリオに。試作の計算 (出現数 0 / 2 以上で当てられない) をそのまま使える
  - sub-09 (Editor): StartChildProcess は bInheritHandles = TRUE でログのハンドルをサービスへ渡す。サービス用の起動口を足すか判断。Editor が Job に入る起動の確認
  - sub-16: C# の Field 復元は FieldFromJson 不可、DeserializeComponent + 1 キー JSON の分岐。実走で確かめる
  - 試作コード・ビルド成果物は %TEMP%\mye_m104b\ に残っている (コミット対象外)。削除は未実施。不要なら司会 / ユーザーの許可のうえで消す

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-14)。受け入れ条件 1〜6 を文書と差分で確認した。抜き取りで裏を取った点: `ScriptRuntime.cs` 655 行目 (`TryGetPropertyValue` が無いキーを `continue`)、`ManagedHost.cpp` 269 行目 (`kComponentScriptState | kComponentNoHash`)、`ChildProcess.cpp` 30 行目 (`bInheritHandles = TRUE`、`CREATE_NO_WINDOW`)、`tools\collab\src\watch.rs` の `DEBOUNCE = 300 ms` / `MAX_HOLD = 2 s`。
  - 逸脱 3 件 (試作 4 を実走していない / 試作 6(b) を本物のクライアントで試していない / Codex・Gemini が未確認) は、どれも sub-01 が許した扱いで、未確認として書かれている。coder の誤りではない。
  - 追加の決定 (デバウンスの値、BREAKAWAY の試行、`rust-version = "1.89"`、`windows-sys`) は採る。ただし「flush を `FILE_SHARE_READ` だけで開いて読む」は、追跡が開いている間に書き手の open / rename / delete が共有違反で失敗しうる (追跡が AI の編集を止める)。sub-03 で「探りは開いてすぐ閉じる、内容の読み取りは全共有」に直す (spec §2 D21)。
  - nit: 計画の進捗表の M104b が「実装済み・判定待ち」のままコミットされる。sub-02 が進捗表を更新するときに「完了」に直す。
  - 質問への回答: (1) 試作 6(b) は、MCP から起動する実装ができる sub-07 で、ユーザーの手動確認として確かめる (coder は自分の claude.exe を終わらせられない)。sub-05 では確かめない。(2) `rust-version = "1.89"` を採る (spec §2 D22)。(3) デバウンスの具体値は sub-03 の仕様に取り込んだ。(4) Edit の CRLF・引用符の扱いは sub-08 で本物の入力を記録して確かめる。外れても Claimed 止まりで安全側。
  - `%TEMP%\mye_m104b\` の試作コードはリポジトリの外で、コミット対象外。消すかどうかはユーザーの判断 (司会が聞く)。