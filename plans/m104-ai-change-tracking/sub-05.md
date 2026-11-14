# sub-05: pipe サーバ、`hello`、取得の op、`track_verify.bat` (M104f)

- 依存: sub-04
- 状態: 未着手
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

## フィードバック履歴
