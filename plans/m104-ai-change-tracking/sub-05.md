# sub-05: pipe サーバ、`hello`、取得の op、`track_verify.bat` (M104f)

- 依存: sub-04
- 状態: OK (M104f のコミット)
- 往復: 0

Phase 1 の最後のサブ。要求 Phase 1 の完了条件「再起動後も記録したファイル変更を取得でき、データ欠落時には不完全であることを判定できる」をここで満たす。

## 着手前にユーザー確認が要る未決事項

司会が着手時に聞く。

1. 計画の未決事項 2
   - 聞きたいこと: `tools\track` を CI (`C:\HAL\MyEngin\.github\workflows\ci.yml`) に入れるか。
   - 推奨: `cargo test --manifest-path tools\track\Cargo.toml` と `tools\track_verify.bat` を、collab の段 (ci.yml 136〜141 行目) の隣に入れる。collab と同じ立場 (Editor なしで完結する)。
   - 逆を選ぶと何が変わるか: 入れない場合、`rusqlite` bundled が CI のランナーでビルドできるか (試作 2 の残り) は未検証のまま残り、sub-01 の結果の文書にそう書く。ワークフローは触らない。
   - 入れる場合の注意: CI で確かめられるのは push の後。このサブの中では「ワークフローを書いた」までで、「CI で通った」は未検証として報告する。落ちたら次のサブへ進まずユーザーへ。
2. spec §2 D4
   - 聞きたいこと: `mye_track.exe` の置き場所と探し方。
   - 推奨 (仮置き): `tools\build_track.bat` が `bin\x64\Debug\` と `bin\x64\Release\` の両方へ置く。探す順は (1) 環境変数 `MYE_TRACK_EXE` (2) 自分の exe の隣 (3) MCP だけ `..\..\..\track\target\release\mye_track.exe`。
   - 逆を選ぶと何が変わるか: `tools\track\target\release\` を直接指す形 (`.mcp.json` と同じ) にすると `build_track.bat` が要らなくなるが、Editor が自分の隣から探せない。
   - 前提 (2026-11-14 のユーザー判断、spec §2 D5): エンジン配布物に `mye_track.exe` を入れることが決まっている (sub-20)。配布物には `tools\track\target\` が無いので、**「Editor が自分の exe の隣で見つけられる」ことは選択肢によらず必要**。逆を選ぶ場合も (2) の探し方は残し、sub-20 を書き直す。

## やること

ipc §1〜§3、§4.1、§4.3 の一部、§5、§6 を作る。Win32 API の呼び方は sub-01 の試作 1 で決まったもの。

sub-01 の試作 1 / 6 で決まったこと (2026-11-14 の見直し。ipc §1 と arch §3 が正):

- サーバ: SDDL `D:P(A;;GA;;;<自分の SID>)`、最初の 1 本だけ `FILE_FLAG_FIRST_PIPE_INSTANCE` (mutex と二重の防御)、`ConnectNamedPipe` が返ったらすぐ次のインスタンスを作る。ハンドルは `CloseHandle` を呼ぶ小さい RAII 型で持つ。
- クライアント (`client` サブコマンド): `ERROR_PIPE_BUSY` (231) は `WaitNamedPipeW` で待って (最大 2 秒) 開き直す。
- `rusqlite` bundled の CI でのビルドは未検証のまま (db §8)。未決事項 1 の答えが「入れる」なら、このサブのワークフローの追加が最初の確認になる。通らなければ `CC` の指定を試す。
- 本物の MCP クライアントの Job の中での生存 (試作 6(b)) は、**このサブでは確かめない** (MCP から起動する実装が無い)。sub-07 の手動確認へ回した。

1. 名前付きパイプのサーバ: `\\.\pipe\mye-track-<projectId>`、現在のユーザーだけの DACL、`PIPE_REJECT_REMOTE_CLIENTS`、複数接続、接続ごとに 1 スレッド、1 行 1 JSON (LF)、1 行の上限 1 MiB (`too_large`)。
2. 単一インスタンス: 名前付き mutex `Local\mye-track-<projectId>`。取れなければ即終了 (終了コードで「既に居る」を区別できるようにする)。
   - **二重の守り (sub-02 の VERDICT で追加。spec §2 D25)**: projectId はパスの文字列から作るので、同じディレクトリを別の名前で指す (`subst` のドライブ、ジャンクション、8.3 の短い名前、UNC とドライブ文字) と別の projectId になり、mutex も pipe の `FIRST_PIPE_INSTANCE` も効かない。**サービスは起動時に `.mye\ai_changes\service.lock` を共有なし (share mode 0) で開き、終了まで持ち続ける。** 開けなければ「同じ DB に別のサービスが居る」として、DB に触る前に終了する。このとき、どの名前で動いているか分からないので、起動を頼んだ側は追跡なしで続行する (pipe 名が違うので接続できない。`service.json` の `projectRoot` を標準エラーへ出す)。
   - sub-02 の `startup` が持っている「検査の 3〜6 で失敗した段と理由」を `status` の DB の健全性に含める。
3. `service.json`: PID、開始時刻、pipe 名、起動ごとの乱数トークン。乱数は OS の乱数源から取る (sim の決定性とは無関係のプロセスだが、`rand` crate は足さない)。
4. `hello`: 版 (`proto_mismatch`)、トークン (`bad_token`)、プロジェクト (`wrong_project`。クライアントが言ったパスで DB を開き直さない)、接続元 (`GetNamedPipeClientProcessId` の PID とプロセス開始時刻を証跡に残す)。`hello` より前の要求は `need_hello` で切る。
5. 役割 (ipc §3.1): `mcp` / `editor` / `hook`。役割の外の op は `forbidden`。このサブで実装する op は `hello`、`status`、`list`、`describe`、`sessions` (空の配列を返せるところまで)。未実装の op は `unknown_op`。知らない引数は `bad_args`。
6. `status`: 稼働時間、追跡中のファイル数、Blob の総量、欠落区間、DB の健全性、直近の処理時間 (ベースライン、1 件の記録)。
7. `list`: フィルタ (`path`、`from`、`to`、`changeType`。`sessionId` / `review` / `attribution` は引数として受けるが、このサブでは対応するデータが無い)、`cursor`、`limit` (既定 50、最大 200)。Blob と Hunk の本文を入れない。
8. `describe`: Before / After のハッシュとサイズ、Hunk の要約 (行番号と行数)、`completeness`、帰属 (このサブでは unknown)、復元可否 (Blob の有無)。
9. プロトコルの正本 `tools\track\src\protocol.rs` に `pub const PROTO_VERSION: u32 = 1;`。**規則 9 にはまだ足さない** (spec §2 D7)。
   - sub-03 の結果 (2026-11-14): 今の `serve` は標準入力の EOF で終わる。`DETACHED_PROCESS` で起動すると標準入力が無効で、すぐ EOF になって終了しうる。**このサブで、標準入力の EOF による終了をやめる** (終了は pipe の要求、無接続の時間切れ、Ctrl+C / コンソールを閉じる、だけ)。`track_verify` と `cargo test` の後始末は pipe 経由の終了を使う。`Tracker::status()` が phase / baseline_files / baseline_ms / baseline_bytes / first_run / overflows を返すので、`status` はこれを使う。
   - sub-21 の結果 (2026-11-15): `Tracker::status()` に `scan_done` / `scan_total` / `baseline_timings` / `verify_runs` / `verify_gaps` / `unrecorded_since_ms` / `unrecorded_reason` が増えた。`status` はベースラインの進捗、検証の走査の回数と拾った件数、**直近の検証の走査の所要時間** (未計測なので、このサブで取れるようにする)、未記録の区間、起動時に壊れていて消した Blob の数 (`blobs_corrupted`) を返す。ベースラインの最中 (このリポジトリで 12〜35 秒、32000 本で 1 分半〜2 分。`C:\HAL\MyEngin\docs\ai-change-tracking\performance.md`) も pipe は応答し、`list` は「ベースライン中」と分かる結果を返す。`mye_track bench` は pipe と無関係。
   - sub-04 の結果 (2026-11-15): Hunk の本文は `change_hunks` を `revision_id` で引く (`tests\revision.rs` の `revision_views` が見本)。`change_revisions.diff_mode` (通常 / `whole` / `binary` / `excluded` / `unavailable`) を `describe` の結果に含める (Hunk が無い理由が分かるように)。`changes.state = superseded` の Change は `list` に出すが、状態が分かるようにする。
   - **スキーマの凍結**: このサブで `serve` が実際に使える常駐になる。これ以降、`schema\v1.sql` は変えない。表や列を変えるときは移行 (v2 以降) を足す (db §8)。このサブで列が要るなら、コミットの前に v1 に入れてよい最後の機会。
   - db §6 に制限を 1 行足す (sub-04 の VERDICT): Hunk の書き込みに失敗した観測は events に残るので、その後に DB を作り直すと、検証の走査が足した `gap` の Revision と合わせて同じ変更が 2 件になる。
10. 寿命: 接続が 0 になってから 10 分 (設定可、テストでは短くできる) で自分で終わる。`service_stop` を残す。Writer のキューが上限を超えたら `busy`。
11. `tools\build_track.bat` (`C:\HAL\MyEngin\tools\build_collab.bat` と同じ形。cargo の解決、`NEQ 0` の判定) と `tools\track_verify.bat` / `tools\track_verify.ps1` (`C:\HAL\MyEngin\tools\mcp_verify.ps1` と同じ形)。
12. `track_verify`: シナリオは `tests\track\*.ndjson` と `*.expected.ndjson`。pipe は標準入出力ではないので、シナリオを流す口が要る。**`mye_track.exe` に `client` サブコマンド (標準入力の行を pipe へ送り、応答を標準出力へ出す) を足す**のを推奨する (sub-08 のフック CLI と pipe クライアントの実装を共有できる)。時刻・UUID・トークン・ハッシュ以外の環境依存の値は正規化して比べる。シナリオごとに一時プロジェクトを作り、サービスを起動し、終わったら必ず止める (プロセスを残さない)。
13. CI (未決事項 2 の答えが「入れる」のとき): `ci.yml` の collab の段の隣に 1 段。

## やらないこと (このサブでは)

- セッション・操作・報告の op (sub-06)。`hunks` / `scene_diff` / `blob_get` / `subscribe` / Editor からの通知 (sub-09 以降の着手時の見直しで、どのサブがサービス側を足すかを確定する)。
- `check_rules.ps1` の変更 (規則 9 は sub-07 から)。`tools\mcp\`、`src\` の変更。
- `make_dist.ps1` / `release.yml` (sub-20)。

## 触る場所 (planner の見立て)

- `C:\HAL\MyEngin\tools\track\src\` に `pipe.rs` (Win32)、`server.rs`、`protocol.rs`、`ops\`、`client.rs`、`tests\pipe.rs`。
- 新規: `C:\HAL\MyEngin\tools\build_track.bat`、`tools\track_verify.bat`、`tools\track_verify.ps1`、`C:\HAL\MyEngin\tests\track\*.ndjson`。
- `C:\HAL\MyEngin\.github\workflows\ci.yml` (未決事項 2 しだい)。
- `C:\HAL\MyEngin\docs\ai-change-tracking\ipc-contract.md` — exe の探し方、`client` サブコマンド、実装で決めた引数の形。`prototype-results.md` — 試作 2 の CI の結果 (確かめられたら)。
- `C:\HAL\MyEngin\plans\m104-ai-change-tracking.md` — 進捗表、未決事項 2 の結果。

## 受け入れ条件 (このサブ)

1. `hello` の失敗 4 種 (`need_hello`、`proto_mismatch`、`bad_token`、`wrong_project`) と成功がシナリオで確かめられる → `tools\track_verify.bat`。
2. 役割の外の op が `forbidden`、知らない op が `unknown_op`、知らない引数が `bad_args`、1 MiB を超える行が `too_large` → `tools\track_verify.bat`。
3. 同じプロジェクトで 2 つ目を起動すると即終了し、1 つ目が動き続ける。別のプロジェクトのサービスは同時に動ける → `cargo test`。
3b. 同じディレクトリを別の名前 (ジャンクション、または `subst`) で `--project` に渡した 2 つ目のサービスが、DB を開く前に終了し、1 つ目の記録が続く → `cargo test`。
4. ファイルを変える → `list` に出る → `describe` で Hunk の要約と `completeness` が返る。50 件を超える履歴が `cursor` でページ送りできる → `tools\track_verify.bat`。
5. **サービスを止めて起動し直しても、止める前の履歴が `list` / `describe` で取れる。止まっていた間の変更は `gap` として区別できる** (要求 Phase 1 の完了条件) → `tools\track_verify.bat` か `cargo test`。
6. 接続が 0 のまま時間切れ (テスト用に短くした値) で自分で終わり、`service_stop` が残る → `cargo test`。
7. pipe に接続できるのが現在のユーザーだけで、リモートを拒否する設定になっていることを、張った pipe の属性を読み戻して確かめる → `cargo test`。
8. `track_verify` を 3 回続けて回して全部通り、終わった後に `mye_track.exe` のプロセスが残っていない → `Get-Process mye_track` が空。
9. A20: `pwsh -NoProfile -File tools\check_rules.ps1` が 0 error。`src\` に差分が無い (`git diff --stat ca6a3991 -- src` が空) ので、selftest と replay_verify はこの Phase では回さなくてよい (回していないことを SELF_EVAL に書く)。

## 検証コマンド

- `cargo test --manifest-path tools\track\Cargo.toml`
- `tools\build_track.bat Release`
- `tools\track_verify.bat` (3 回)
- `pwsh -NoProfile -File tools\check_rules.ps1`
- `git diff --stat ca6a3991 -- src`

## 実装メモ (coder が追記)

### SELF_EVAL: sub-05 (round 1)

実装:
- `tools\track\src\protocol.rs` (新規) — `PROTO_VERSION = 1`、上限 (1 MiB、list 50/200)、役割と op の表 (`role_allows`)、エラーコード、要求の解析、`Args` (残りの引数を `bad_args`)。
- `tools\track\src\pipe.rs` (新規) — Win32: 自分の SID → SDDL `D:P(A;;GA;;;<SID>)`、`PipeListener` (最初の 1 本だけ `FILE_FLAG_FIRST_PIPE_INSTANCE`、`PIPE_REJECT_REMOTE_CLIENTS`)、`PipeInstance::connect` / `client_pid`、クライアントの `open_client` (`ERROR_PIPE_BUSY` を `WaitNamedPipeW` で最大 2 秒)、DACL の読み戻し。
- `tools\track\src\server.rs` (新規) — mutex → `service.lock` (share mode 0) → pipe の 1 本目 → `service.json` → DB → Tracker の起動順、受付ループ (接続ごとに 1 スレッド、次のインスタンスを先に作る)、`hello` の 5 段の検査と証跡 (`client_hello`)、役割の判定、`status` / `list` / `describe` / `sessions` / `shutdown`、1 MiB 行の読み捨て、接続 0 の時間切れ、終了コード 3 = 既に居る。
- `tools\track\src\ops.rs` (新規) — 読み取り専用接続での `list` (フィルタ・cursor・limit)、`describe`、`sessions`、`status` の DB 部分。
- `tools\track\src\client.rs` (新規) — `Client` と `mye_track client` (標準入力の行 → pipe → 標準出力)。
- `tools\track\src\main.rs` — `serve` を `server::run` に差し替え (標準入力の EOF による終了をやめた)、`--idle-timeout-ms`、`client` サブコマンド。
- `tools\track\src\writer.rs` — 積んで終わっていない数 (`pending`)、`submit_bounded` と `WriterError::Busy`。
- `tools\track\src\watch.rs`、`collector.rs` — `StatusReader` (Tracker を持たずに status を読む取っ手)、`TrackerStatus` に `tracked_files` / `last_record_us` / `verify_scan_ms`、検証の走査の前半だけの所要時間の計測。
- `tools\track\Cargo.toml` — windows-sys の feature (Pipes / IO / Security / Authorization / Cryptography)。`schema\v1.sql` は触っていない (凍結)。
- `tools\build_track.bat`、`tools\track_verify.bat`、`tools\track_verify.ps1`、`tests\track\*.ndjson` と `*.expected.ndjson` (11 シナリオ) — 新規。
- `.github\workflows\ci.yml` — collab の段の隣に「track service」の段 (cargo test → build_track → track_verify)。
- `tools\track\tests\pipe.rs` (新規 7 本)、`tests\storage.rs` (+2: `failed_steps`、Writer の `Busy`)、`tests\watch.rs` (stdin EOF の試験を、EOF で終わらず pipe の `shutdown` で終わる試験に差し替え)。
- docs: `ipc-contract.md` §9 (実装の要約)、`db-schema.md` (§6 に Hunk 書き込み失敗の制限、§8 に CI・Rust 1.89 の未検証とスキーマ凍結、§5.1 に `client_hello`)、`performance.md` §7 (検証の走査の実測)、`prototype-results.md` (試作 2 の CI の状況)、`plans\m104-ai-change-tracking.md` (進捗表の M104f、未決事項 2 の決着)。

仕様との差分:
- [追加] `shutdown` op (役割は editor だけ、ipc §4.1 / §3.1 に追記)。sub-05 の「pipe 経由の終了を使う」を実現する op が仕様の op 表に無かった。最も狭い (editor のみ) にしてあり、広げるのは 1 行。
- [追加] `hello` の成功ごとに `events` へ `client_hello` (role、pipe から得た PID、申告 PID、プロセス開始時刻) を残す (「接続元を証跡に残す」の解釈)。1 接続 1 行なので、ポーリングするクライアントが多いとイベントが増える (下の不安)。
- [追加] `hello` / 要求の形が読めない行 (JSON でない、id が無い等) は、`hello` の前でも `need_hello` ではなく `bad_args` (id が読めなければ `null`)。`too_large` は接続を切らず、行を LF まで捨てて続ける。
- [追加] list の細部 (仕様に無かった): 新しい順、`cursor` = 直前のページの最後の `revisionId`、`limit` の最大超えは 200 に切る、`path` は完全一致 (末尾 `/` は前方一致、リネーム前のパスにも当たる)、`from` / `to` は両端を含む。`sessionId` / `review` / `attribution` は実際の列に対して絞る (データが無いので `sessionId` と `review=approved` 等は常に空)。`describe` の `restorable` = Before の Blob が present、または新規 (added)。`sessions` の cursor = `<startedAt>:<sessionId>`。
- [追加] `status` の `verify.lastScanMs` と `recent.recordUs` のために `ScanProgress` / `TrackerStatus` に項目を足した (sub-21 の nit への対応。sub-05 の「検証の走査だけの所要時間を取れるようにする」)。`status` の `trackedFiles` は Tracker が公開する値 (ベースラインの管理下のファイル数)。
- [追加] `mye_track serve --idle-timeout-ms` (既定 10 分。試験で短くするための口)。起動からも数える (接続が 0 のまま起動した場合も 10 分で終わる)。
- [追加] 既存の `tests\watch.rs::serve_runs_until_stdin_eof_...` を差し替えた。標準入力の EOF による終了を消す仕様変更で、この試験は 60 秒待って落ち、子プロセスを残した (最初の全体実行で実測)。新しい試験は「EOF では終わらない」ことを確かめ、子プロセスを Drop で必ず止める。
- [逸脱] 「ハンドルは `CloseHandle` を呼ぶ小さい RAII 型」のうち、pipe と DACL 取得のトークンは自作の型ではなく `File::from_raw_handle` で持つ (Drop が `CloseHandle`)。mutex のハンドルだけ小さい型 (`HandleGuard`)。unsafe を増やさないため。
- [未実装] 仕様どおり: `subscribe`、セッション・操作・報告の op、`hunks` / `scene_diff` / `blob_get`。`status` の「CI での実走」は未確認 (下)。

検証:
- `cargo test --manifest-path tools\track\Cargo.toml` → 全部 PASS (lib 18、diff 21、pipe 7、project_id 3、reconcile 9 + 1 ignored (既存)、revision 13、storage 18、watch 19)。pipe と storage は繰り返し (pipe 5 回、storage 6 回) で安定。storage の `pending` の試験は 1 回落ち (結果送信と件数減算の隙間)、待ちを足して直した。
- `tools\build_track.bat Release` → 成功。`bin\x64\Debug` と `bin\x64\Release` の両方へ `mye_track.exe` をコピー (`bin\` は .gitignore 済みを `git check-ignore` で確認)。
- `tools\track_verify.bat` → 3 回続けて全 11 シナリオ PASS、終了後 `Get-Process mye_track` は 0 件 (最後の修正の後の 3 回)。途中でフレーク 2 件を見つけて直した: (1) トークンを全要求行で読んでいて、サービスが止まった後の行で 60 秒待った、(2) その失敗で `svcStopping` の状態が次のシナリオに漏れた。
- `pwsh -NoProfile -File tools\check_rules.ps1` → 0 error (61 warning は既存)。
- `git diff --stat ca6a3991 -- src` → 空。
- 検証の走査の実測 (作業ツリーのコピー、`.mye` は汚していない): 追跡対象 4105 ファイルで 615 ms、1 件の記録 1.6 ms (`performance.md` §7)。
- selftest / replay_verify は未実行 (`src\` に差分が無いので不要)。
- **CI での実走は未確認** (`ci.yml` を書いただけ。push していない)。rusqlite bundled の windows-2022 でのビルドも同じく未確認。**Rust 1.89 での実ビルドは未確認** (開発機は 1.98 のみ。ツールチェーンは足していない)。
- 後始末: 起動した `mye_track.exe` / cargo は残っていない (全体実行で残った 1 件は手で止めた)。一時ディレクトリ (`mye_track_pipe_*`、`mye_trackvf_*`、計測のコピー) は消した。既存の `%TEMP%\mye_track_*` と `%TEMP%\mye_m104b\` は触っていない。

自己採点 (1-5):
- 仕様適合: 4 — 受け入れ条件 1〜9 を試験・シナリオで確かめた。細部の解釈は上の [追加] に出した。CI と 1.89 は未確認。
- 正しさ: 4 — 二重起動 3 段、再起動をまたぐ履歴と gap、時間切れ、DACL の読み戻しを実プロセスで確認。別ユーザーの拒否とリモートの実接続は未確認、リモート拒否の試験は「開けない」ことだけ (フラグの有無の比較は試作 1)。
- コード品質: 4 — 既存の層に沿って追加。`server.rs` が大きい (約 680 行) ので sub-06 で op が増えるとき分割が要る。
- テスト: 4 — 実プロセスの結合試験 7 本 + シナリオ 11 本 + 単体。`busy` は Writer 単体で確認し、pipe 経由で 1024 件詰める試験は無い。

不安・質問:
- `client_hello` を 1 接続 1 イベントで残すため、ポーリングするクライアント (検証の `wait-recorded` は 200 ms 間隔) やフック (ツール呼び出しごと) でイベントが増える。DB を作り直すときの入れ直しも増える。「証跡を残す」の最小解釈だが、sub-06 以降で頻度が問題なら、同じ PID の重複を間引く案がある (planner の判断)。
- `shutdown` を editor だけにした。MCP が起動したサービスを MCP が止められない (止めたい場面は無い想定)。広げるなら `role_allows` の 1 行。
- idle の時間切れを「起動からも数える」にした。起動を頼んだ側が接続前に死ぬと 10 分で自分で終わる (孤児の防止)。ベースラインが 10 分を超える巨大なプロジェクトでは、接続が無いままベースラインの途中で終わりうる (実測は 32000 ファイルで 2 分弱)。

触ったファイル:
- `C:\HAL\MyEngin\tools\track\Cargo.toml`、`src\collector.rs`、`src\lib.rs`、`src\main.rs`、`src\watch.rs`、`src\writer.rs`、`src\client.rs`、`src\ops.rs`、`src\pipe.rs`、`src\protocol.rs`、`src\server.rs`、`tests\pipe.rs`、`tests\storage.rs`、`tests\watch.rs`
- `C:\HAL\MyEngin\tools\build_track.bat`、`tools\track_verify.bat`、`tools\track_verify.ps1`、`tests\track\` (22 ファイル)
- `C:\HAL\MyEngin\.github\workflows\ci.yml`
- `C:\HAL\MyEngin\docs\ai-change-tracking\ipc-contract.md`、`db-schema.md`、`performance.md`、`prototype-results.md`
- `C:\HAL\MyEngin\plans\m104-ai-change-tracking.md`

申し送り:
- sub-06: `server.rs` の `handle_request` に op を足す。記録を伴う op は `ctx.writer.submit_bounded(WRITER_QUEUE_LIMIT, ...)` を通し、`Busy` を `busy` で返す (`hello` の証跡が見本)。`list` の `sessionId` / `review` / `attribution` は引数と SQL が既に配線済みで、データが入れば効く。
- sub-08 / sub-09: `client.rs` の `Client` を共有できる。exe の探し方は ipc §9.5 (環境変数 → 自分の隣 → MCP だけ target\release)。`mye_track.exe` は `build_track.bat` が `bin\x64\Debug` と `Release` の両方へ置く。
- ベースラインの最中の `list` は `coverage.baselining = true` を返す (シナリオには入れていない。ベースラインを長く保つ口が試験用の `baseline_chunk_pause` しかなく、製品の `serve` に出していないため)。
- スキーマ `v1.sql` はこのサブのコミットで凍結。
- 検証用の `track_verify.ps1` は、`# write` などのディレクティブ付きの NDJSON で、MCP / フック CLI のシナリオにもそのまま使える (`client` が hello を自動でしないので、役割ごとの hello を書く)。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-15)。確かめたこと: `mye_track` / `cargo` の残存プロセスなし、リポジトリに `.mye\ai_changes\` なし、`git diff --stat ca6a3991 -- src` は空。planner が全体の `cargo test` を 1 回 (lib 18 / diff 21 / pipe 7 / project_id 3 / reconcile 9 (+1 ignored) / revision 13 / storage 18 / watch 19、33 秒) と `tools\track_verify.bat` を 1 回 (11 シナリオ PASS、10 秒、後にプロセスなし) 実走。`ci.yml` の差分は collab の段の隣の 1 段だけ。
  - 追加・逸脱の判定: `shutdown` (editor だけ)、`client_hello` のイベント、形が読めない行は `bad_args`、`too_large` で切らない、`list` の細部、`--idle-timeout-ms`、stdin EOF の試験の差し替え、ハンドルを `File::from_raw_handle` で持つ、は全部採る。
  - 質問への回答: (1) `client_hello` は 1 接続 1 イベントのままでよい (間引かない。フックは操作ごとに `op_begin` / `op_end` のイベントも残すので、桁は変わらない)。(2) `shutdown` は editor だけでよい。(3) 無接続の時間切れがベースラインの途中で発火しうる件は、sub-06 で「ベースライン・再走査の最中と、running のセッションがある間は発火させない」にする。
  - 未確認のまま残るもの: CI での実走 (push していない。`rusqlite` bundled の `windows-2022` でのビルドを含む)、Rust 1.89 での実ビルド、別のユーザーからの接続の拒否。CI は時間に依存する試験 (監視、負荷の下の保存) を含むので、最初の push で落ちたら、その試験と所要時間を見て直す。