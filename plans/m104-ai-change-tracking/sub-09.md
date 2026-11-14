# sub-09: Editor の pipe クライアント、モデル、規則 19 (M104j)

- 依存: sub-08
- 状態: OK (commit は台帳 harness.md を参照)
- 往復: 0
- 書き込みの深さ: **詳細** (2026-11-15、sub-08 の VERDICT で planner が書き直した)。Phase 3 の最初のサブで、**このマイルストーンで初めて `src\` を触る**。
- 質問の運用: 推奨で進め、Notion に後回し。

## 着手前の確認の扱い (推奨で裁定済み。計画の未決事項 3)

- 聞きたいこと: 追跡を既定で有効にするか。
- **裁定 (推奨どおり): 既定で無効。** `EditorSettings` の `changeTrackingEnabled` (既定 false) を true にしたプロジェクトでだけ、Editor がサービスへ接続・起動する。理由: (1) 見る窓 (sub-10) が無いうちに記録だけ溜めない (2) 有効にすると最初のベースラインで `.mye\ai_changes\blobs\` に追跡対象の内容が複製される (このリポジトリで約 488 MB、12〜35 秒。`C:\HAL\MyEngin\docs\ai-change-tracking\performance.md`)。黙って始めるには大きい。
- 逆の答えが来たら差し戻す範囲: `EditorSettings` の既定値 1 つと、それを確かめている試験の期待値。受け入れ条件 3 (非対話起動では触れない) は変わらない。
- 「Phase 3 が終わったら既定を有効にするか」は sub-13 の後でもう一度裁定する。

## 確認済みの事実 (2026-11-15 に planner が調べた。着手時に行番号はずれうる)

- 非対話起動の判定は既にある: `C:\HAL\MyEngin\src\Editor\App\EditorMain.cpp` 301〜307 行目の `const bool automation` (`maxFrames > 0`、`IsCaptureRun()`、リプレイの記録・検証、`autoPlay`、`--package` など)。586 行目で `app.allowAutoUpdateCheck = !automation;` として `EditorApp` に渡している。`--selftest*` / `--dump-index` / `--img-diff` / 各ベンチは `EditorApp` を作る前に `return` する (323〜522 行目)。
- 前例は `SourceControlSession` (`C:\HAL\MyEngin\src\Editor\SourceControl\SourceControlState.h` 240 行目): `Start(exeDir, projectRoot, ...)` / `Poll()` / `Shutdown()` / `State()`。`EditorApp` のメンバ `scm_` (`EditorApp.h` 189 行目)、`OnStart` で `Start` (`EditorApp.cpp` 257 行目)、`OnImGui` の先頭で `Poll` (751 行目)、`OnShutdown` で `Shutdown` (533 行目)。`CollabClient` は試験が行を流し込める公開の `DispatchLine` を持つ。
- `EditorSettings` (`C:\HAL\MyEngin\src\Editor\App\EditorSettings.h` / `.cpp`): 置き場所は `<project>\.mye\editor_settings.json`。bool の足し方の前例は `feelMuteCamera` (`.h` 44 行目、`.cpp` 39 行目と 89 行目)。保存は自分のキーだけを上書きする。
- 自己テスト: `struct SelfTestSuite { const char* name; bool (*run)(); SelfTestTier tier; const char* note; }`、表は `C:\HAL\MyEngin\src\Editor\SelfTest\SelfTestSuites.cpp` 142 行目の `kSuites`。スイートは `bool RunXxxSelfTest()` で、`check(cond, what)` のラムダで数えて `failCount == 0` を返す (`SourceControlSelfTest.cpp` 100〜111 行目)。
- プロジェクト: `ctx.projectRoot` (空ならプロジェクトなし)。`NormalizePathKey` (`C:\HAL\MyEngin\src\Engine\Platform\PathUtil.h` 36 行目)、`Sha256Hex(std::string_view)` (`Sha256.h` 14 行目)。`src\` に `setlocale` / `std::locale::global` の呼び出しは無い (projectId の ASCII 小文字化の前提が成り立つ)。
- `StartChildProcess` (`C:\HAL\MyEngin\src\Editor\App\ChildProcess.h`) は `bInheritHandles = TRUE` でログのハンドルを子へ渡す。サービスの起動には使わない (spec §2 D24)。
- 状態の表示の前例: `SourceControlWindow.cpp` 85〜104 行目の `UnavailableText(Unavailable)` と 255〜269 行目 (理由を 1 つずつ出す)。

## やること

### A. Editor 側 (`src\Editor\ChangeTracking\`、新設)

1. `TrackProtocol.h`: `constexpr int kTrackProtoVersion = 1;`、op 名とエラーコードの文字列定数。
2. `TrackProjectId.h/.cpp`: `ProjectIdSource(root)` (`NormalizePathKey` の結果から末尾の `\` を全部落とす。ドライブのルート `c:\` は落とさない) と `ProjectId(root)` (その UTF-8 の `Sha256Hex` の先頭 16 桁)。**固定表 `C:\HAL\MyEngin\tools\track\tests\project_id_cases.tsv` (4 列) の全行と一致すること** (spec §2 D25)。試験は表を読むか、同じ行を持つ (表を読むなら、リポジトリのルートが分からないときは SKIP ではなく、行を埋め込んだ側で確かめる)。
3. `TrackLaunch.h/.cpp`: サービスの起動。**`StartChildProcess` を使わない。** `CreateProcessW` を `bInheritHandles = FALSE`、`CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB` で呼び、`ERROR_ACCESS_DENIED` なら BREAKAWAY を外してやり直す。引数は `serve --project <root>`。プロセスのハンドルはすぐ閉じる (待たない・Job に入れない)。exe の探し方は ipc §9.5 (環境変数 `MYE_TRACK_EXE` があればそれだけ。無ければ Editor の exe の隣の `mye_track.exe`)。見つからなければ「サービスなし」の状態にして、探し直しは設定の切り替えか再起動まで行わない。
4. `TrackClient.h/.cpp`: 名前付きパイプのクライアント。**スレッドを作らない。`Poll()` は待たない。**
   - `CreateFileW(FILE_FLAG_OVERLAPPED)`。`ERROR_FILE_NOT_FOUND` → 起動を 1 回頼んで後でやり直す。`ERROR_PIPE_BUSY` → 待たずに次のフレーム以降でやり直す (`WaitNamedPipeW` は使わない)。
   - 読み書きは overlapped。`ERROR_IO_PENDING` は保留し、次の `Poll()` で `GetOverlappedResult(bWait = FALSE)`。1 行が複数回の読み取りに分かれる・1 回の読み取りに複数行が入る、の両方を扱う。1 行の上限 1 MiB (超えたら切断して状態を出す)。
   - 接続したら `hello` (`role = editor`、`proto`、`token` = `<project>\.mye\ai_changes\service.json` の値、`projectRoot`、`pid`)。`bad_token` は `service.json` を読み直して 1 回だけやり直す。`hello` の結果の `projectId` が自分の計算と違えば切断して「プロジェクト違い」。
   - `Request(op, args, onDone)` (応答は `id` で対にする。順不同)。時間切れは読み取り 30 秒 / 記録 5 秒 (フレームの時刻ではなく単調時計で測る)。通知 (`id` の無い行) はハンドラへ。
   - 切断したら数秒 (定数) おきに再接続を試す。再試行が負荷にならないこと (試行ごとの仕事は `CreateFileW` 1 回と、必要なら `service.json` の読み取り)。
   - **1 フレームの `Poll()` が処理する量に上限** (行数と時間。定数)。残りは次のフレーム。
   - I/O (パイプの開く・読む・書く、`service.json` の読み取り、起動) は小さいインターフェースの後ろに置き、試験が偽物に差し替えられるようにする。行の解釈は、試験が直接流し込める公開の口 (`DispatchLine`) を持つ (`CollabClient` の前例)。
   - 状態: `Disabled` (設定で無効 / 非対話起動) / `NoProject` / `Connecting` / `Ready` / `NoService` (exe が無い) / `ProtoMismatch` / `WrongProject` / `DbDegraded` / `Disconnected` (再接続待ち)。`ProtoMismatch` と `WrongProject` は再接続しない。
5. `ChangeModel.h/.cpp`: 表示のための保持。`status` (稼働時間、追跡中のファイル数、Blob の総量、ベースラインの進捗 `scan_done` / `scan_total`、欠落区間、監視の本数)、セッションの一覧 (`sessions`)、変更の一覧 (`list`。ページ単位。フィルタ)、選んだ Revision の詳細 (`describe`)。通知を受けたら「古くなった」印を付け、次に必要になったときに取り直す (通知ごとに全部は取り直さない。間引きの最短間隔を定数にする)。
6. `ChangeTrackingSession.h/.cpp`: 持ち主 (`SourceControlSession` の形)。`Start(exeDir, projectRoot, enabled)` / `Poll()` / `Shutdown()` / `State()` / `Model()`。`Shutdown()` は接続を閉じるだけで、**サービスを止めない** (Editor を閉じても記録が続く。arch §3)。
7. `EditorApp`: メンバを足し、`OnStart` で `Start` (条件: `allowChangeTracking && settings_.changeTrackingEnabled && !ctx.projectRoot.empty()`)、`OnImGui` の `scm_.Poll()` の隣で `Poll()`、`OnShutdown` で `Shutdown()`。`EditorApp.h` に `bool allowChangeTracking = false;`、`EditorMain.cpp` で `app.allowChangeTracking = !automation;` (`allowAutoUpdateCheck` の隣。**`allowAutoUpdateCheck` を使い回さない**。意味が違う)。
8. `EditorSettings`: `bool changeTrackingEnabled = false;`、キー `"changeTrackingEnabled"`。切り替えの UI は sub-10 (このサブでは設定ファイルと試験からだけ)。
9. 文字列: このサブで画面に出すものは無い (状態の文面は sub-10)。ログは既存の `MYE_LOG_*` で、接続・切断・状態の変化を 1 行ずつ (毎フレーム出さない。WARN はトーストに集められるので、再接続のたびに WARN を出さない)。
10. `tools\gen_project_files.ps1` で `build\Editor.vcxproj` と `.filters` を更新する。

### B. サービス側 (`tools\track`): `subscribe` と通知

ipc §4.1、§4.5。`role = editor` だけ。

- `subscribe` (`events`: 名前の配列。知らない名前は `bad_args`)。その接続へ、以後の通知を 1 行 1 JSON (`id` 無し、`event` あり) で送る。
- 通知: `change_recorded` (Revision が確定した: `revisionId`、`changeId`、`path`、`changeType`、`attribution`)、`attribution_updated` (`revisionId`、新しい `actor` / `attribution`)、`coverage_gap` (区間、理由)、`session_changed` (`sessionId`、`status`)、`shutting_down` (理由)。
- 通知は記録の経路を待たせない: 接続ごとに上限つきの送信キューを持ち、あふれたら古い通知を捨てて `resync` の通知 (「取りこぼした。取り直せ」) を 1 つ送る。読まないクライアントがいても Writer と監視は止まらない。
- `schema\v1.sql` は変えない。`protocol.rs` の `IMPLEMENTED_OPS` と `role_allows`、ipc の表を更新する。プロトコル版は上げない (op の追加だけ。ipc §7)。
- 試験: `tests\pipe.rs` とシナリオ (`tests\track\`)。

### C. 自己テストと規則

1. `ChangeTrackingSelfTest.cpp/.h` (`src\Editor\ChangeTracking\`)、`SelfTestSuites.cpp` の `kSuites` に `{ "changetracking", mye::RunChangeTrackingSelfTest, SelfTestTier::Fast, "M104j" }`。**サービスなしで動く** (偽の I/O と、流し込む行だけ)。
2. 実サービスとの結合は、環境変数 `MYE_TRACK_PROBE=1` があるときだけ動く節にする (`MYE_COLLAB_PROBE` の前例)。一時プロジェクトを作り、`bin\x64\<構成>\mye_track.exe` を起動して `hello` → `status` → ファイルを書く → `change_recorded` の通知 → `list`、最後に `shutdown` (`role = editor`) で止めて、プロセスが残らないことを確かめる。既定では動かない (`--selftest` がサービスを起動しない)。
3. `C:\HAL\MyEngin\tools\check_rules.ps1`:
   - **規則 19** (着手時に末尾の番号を確かめる。2026-11-15 時点で末尾は 18): `src\Editor\ChangeTracking\` を Engine / Runtime / GameLogic / Shared / Server から include しない。形は規則 15-a (`Test-CodeLines $f '#\s*include\s*[<"][^>"]*ChangeTracking[\\/]' 'rule 19' ...`)。冒頭のコメントに、規則 12 と同じ理由 (追跡が sim に触れないことを include で機械検査する) を書く。
   - 規則 9: sub-07 が足した `PROTO_VERSION (track) / PROTO_VERSION (mcp)` の組に、`'src\Editor\ChangeTracking\TrackProtocol.h' = 'constexpr\s+int\s+kTrackProtoVersion\s*=\s*(\d+)'` を足す (label も 3 つの名前に直す)。
4. `C:\HAL\MyEngin\tools\check_rules_selftest.ps1`: 規則 19 の検査を足す (今は規則 18 だけ)。一時の `-SourceRoot` に、`Engine\...\Bad.cpp` (include あり → 落ちる)、`Editor\...\Good.cpp` (Editor からの include → 落ちない)、コメントの中の include (落ちない) を置く。既存の規則 18 の検査を壊さない。

### D. 文書

- `C:\HAL\MyEngin\docs\ai-change-tracking\architecture.md` §3 (「Editor は既存の `StartChildProcess` を使う」を直す。Editor が Job の中で起動された場合の結果か「未確認」)、§7、§9 (規則 19 の番号)。`ipc-contract.md` (`subscribe`、通知、`resync`)。
- `C:\HAL\MyEngin\engine_spec.md` §6.3 は触らない (D3D の許可リストは関係ない)。エディタの章への追記は sub-10 (窓ができてから)。
- `C:\HAL\MyEngin\plans\m104-ai-change-tracking.md`: 進捗表、予約する番号 (規則 19 の確定)、未決事項 3 の裁定。

## やらないこと (このサブでは)

- 窓・バッジ・ハイライト・設定の UI (sub-10、12)。`editor_saved` の通知 (sub-10 で、Editor の保存の経路と一緒に入れる)。`hunks` / `blob_get` (sub-10)。レビュー・復元 (sub-14〜16)。
- Engine 層 (`src\Engine`)、Runtime、Server、GameLogic、Shared の変更。ECS・ABI・SimSnapshot・TypeId。
- `StartChildProcess` の変更。`SourceControl\` の変更。
- 容量の回収。

## 受け入れ条件 (このサブ)

1. `changetracking` スイートがサービスなしで通る (Debug と Release) → `MYE_SELFTEST_ONLY=changetracking` で `Editor.exe --selftest`。スイートが確かめること:
   - 応答の解釈: 順不同の `id`、1 行が 3 回の読み取りに分かれる、1 回の読み取りに 3 行、空行、JSON でない行、1 MiB を超える行。
   - `hello` の失敗ごとの状態: `proto_mismatch` → `ProtoMismatch` (再接続しない)、`wrong_project` / projectId の不一致 → `WrongProject`、`bad_token` → 読み直して 1 回だけやり直す、`db_degraded`。
   - 時間切れ: 応答が来ない要求が、読み取り 30 秒 / 記録 5 秒 (試験では偽の時計) で失敗のコールバックを呼ぶ。
   - 切断と再接続: 切断 → 保留中の要求が全部失敗で返る → 定数の間隔で再試行 → 成功。再試行の間、1 フレームあたりの I/O の呼び出しが上限以下。
   - `Poll()` の上限: 1 万行を一度に流し込んでも、1 回の `Poll()` が処理する行数が定数以下で、数フレームで全部処理される。
   - 通知: `change_recorded` でモデルに「古い」印が付き、間引きの間隔より短い連続の通知で取り直しが 1 回。`resync` で全部取り直す。
   - projectId: 固定表の全行 (非 ASCII の行を含む)。
2. **設定が無効、または `allowChangeTracking` が偽のとき、I/O の口が 1 回も呼ばれない** (偽の I/O の呼び出し回数が 0。パイプも `service.json` も起動も) → スイート (要求 §16.5 の 7 番目)。
3. **非対話起動でサービスを起動しない**: `Editor.exe --selftest` の実行中と実行後に `mye_track.exe` が存在しない。`changeTrackingEnabled` を true にした一時プロジェクトで `--screenshot` (や `--frames`) を回しても存在しない → 実行して `Get-Process` で確かめる (spec §2 D11)。
4. サービスの exe が無い / 応答しない / 途中で死ぬ、のそれぞれで Editor が落ちず、状態が変わるだけで、フレームが止まらない (`Poll()` 1 回の時間が上限以下) → スイート (要求 §16.5 の 3、4 番目)。
5. **実サービスとの結合 (`MYE_TRACK_PROBE=1`)**: 一時プロジェクトで、起動 → `hello` → `subscribe` → ファイルを書く → `change_recorded` → `list` に出る → `shutdown`。終わった後に `mye_track.exe` が残らない。Editor が起動したサービスが、Editor のハンドルを継いでいない (起動の引数が `bInheritHandles = FALSE` であることをコードで、サービスが Editor の終了後も生きることを実行で) → `MYE_TRACK_PROBE=1` で `Editor.exe --selftest` を 1 回。回せなければ「未確認」と書く (成功扱いにしない)。
6. サービス側: `subscribe` が `editor` 以外で `forbidden`。通知 5 種が出る。読まない接続がいても記録が止まらず、あふれたら `resync` が 1 つ出る → `cargo test --manifest-path tools\track\Cargo.toml` + `tools\track_verify.bat`。
7. 規則 19 が通り、`tools\check_rules_selftest.ps1` が規則 18 と 19 の両方で PASS。規則 9 の 3 site の組が通り、`TrackProtocol.h` の値を一時的に 2 にすると落ちる (確かめた後で戻す) → 実行結果。
8. 層: `git diff --stat ca6a3991 -- src\Engine src\Runtime src\Server src\GameLogic src\Shared src\Scripting` が空。`src\Editor\ChangeTracking\` の include に `Engine\Core\Ecs` の World / TickRunner など sim を動かすヘッダが無い (要るなら理由を書く)。
9. 既存の検証: `Editor.exe --selftest` (Debug / Release) が全部通る。`tools\check_rules.ps1` が 0 error。Phase 1〜2 の `cargo test` / `track_verify` / `mcp_verify` が通る。`tools\replay_verify.bat` は、Engine 層を触っていないのでこのサブでは回さなくてよい (回していないことを書く。Phase 3〜4 の横断は sub-14)。
10. `build\Editor.vcxproj` / `.filters` の差分が、新しいソースの追加だけ (`gen_project_files.ps1` の出力)。

## 検証コマンド

- `MyEngine.sln` を `Debug|x64` と `Release|x64` でビルド (MSBuild の場所は既存の `tools\replay_verify.bat` か `tools\make_dist.ps1` の探し方に合わせる)
- `MYE_SELFTEST_ONLY=changetracking` で `bin\x64\Debug\Editor.exe --selftest`、続けて `bin\x64\Debug\Editor.exe --selftest` と `bin\x64\Release\Editor.exe --selftest`
- `tools\build_track.bat Release` の後、`MYE_TRACK_PROBE=1` で `bin\x64\Release\Editor.exe --selftest` (スイートを絞ってよい)
- `cargo test --manifest-path tools\track\Cargo.toml`、`tools\track_verify.bat`
- `pwsh -NoProfile -File tools\check_rules.ps1`、`pwsh -File tools\check_rules_selftest.ps1`
- exe の実行は PowerShell から行う (Bash の `cmd /c` は実行されないことがある)。コンソールのコードページを変えたら 65001 に戻す。

## 実装メモ (coder が追記)

SELF_EVAL: sub-09 (round 1)

実装:
- `src\Editor\ChangeTracking\` (新設 15 ファイル): `TrackProtocol.h` (版・op・通知・エラーコード)、`TrackProjectId` (`ProjectIdSource` / `ProjectId` / `TrackPipeName`)、`TrackLaunch` (`FindTrackExe` / `QuoteCommandLineArg` / `LaunchTrackService` = `CreateProcessW`、`bInheritHandles = FALSE`、`CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB`、`ERROR_ACCESS_DENIED` で BREAKAWAY を外して再試行、ハンドルはすぐ閉じる)、`TrackIo` (I/O の口 + Win32 実装 = overlapped の pipe、service.json、起動、単調時計)、`TrackClient` (状態 9 種、hello、`bad_token` の 1 回再試行、時間切れ 30 s / 5 s、切断 → 保留の全失敗 → 3 s おきの再接続、1 回の Poll の上限 = 256 行・2 ms・読み取り 4 回、1 MiB の行の上限)、`ChangeModel` (status / sessions / list / describe、通知で古い印、最短 500 ms の間引き、`resync` は全部を即取り直し)、`ChangeTrackingSession` (`Start` / `Poll` / `Shutdown` / `State` / `Model`。Shutdown は接続を閉じるだけ)、`ChangeTrackingSelfTest`。
- `EditorApp.h/.cpp` (メンバ `changeTracking_`、`allowChangeTracking`、OnStart / OnImGui / OnShutdown)、`EditorMain.cpp` (`app.allowChangeTracking = !automation`)、`EditorSettings.h/.cpp` (`changeTrackingEnabled` 既定 false)、`SelfTestSuites.cpp` (`changetracking`、末尾)。
- サービス側: `tools\track\src\notify.rs` (新設: 購読ごとの上限つきキュー、DB を読み取り専用で引くポーラ、`poll_once`)、`server.rs` (`subscribe`、購読した接続の送り出し、`shutting_down`)、`protocol.rs` (`IMPLEMENTED_OPS`、`role_allows`、`EVENT_NAMES`)、`pipe.rs` (`bytes_available`)、`client.rs` (`wait_line`)、`main.rs` (`--notify-queue`)、`tests\pipe.rs` (3 本)、`tests\track\23_subscribe.ndjson`。
- `tools\check_rules.ps1` (規則 19、規則 9 に C++ の版定数)、`tools\check_rules_selftest.ps1` (規則 19)、`build\Editor.vcxproj` / `.filters` (`gen_project_files.ps1` の出力 15 ファイル分)、文書 (architecture §3 / §7、ipc-contract §4 / §9.8 / §9.9、計画の進捗・未決事項 3・申し送り)。

仕様との差分:
- [追加] 通知の作り方: 記録の経路 (Writer / 監視) に hook を足さず、購読者がいる間だけ読み取り専用の DB 接続が 200 ms ごとに新しい行を引く方式にした (`notify.rs`)。理由: Writer・監視・帰属の計算 (複数箇所で `change_revisions` を書く) を 1 行も変えずに、通知が記録を待たせないことを構造で保証できる。代償: 通知は最大 200 ms 遅れる。`attribution_updated` は「知らせ済みの Revision に根拠が足された」ときに出る。
- [追加] `change_recorded` / `attribution_updated` に `actor` を付けた (仕様の項目は `attribution` だけ)。`coverage_gap` は `gapId`、`fromAt`、`toAt`、`reason`、`scope`。
- [追加] サービスの `serve --notify-queue <n>` (試験が小さなキューであふれを作るため。既定 256)。
- [追加] 購読した接続は、同期ハンドルの `ReadFile` 中に別スレッドから `WriteFile` できない (止まる) ので、接続のスレッドが `PeekNamedPipe` で入力を見ながら 10 ms ごとに通知を送り出す (`pipe::bytes_available`)。
- [追加] `shutting_down` を受けたあとの切断では、サービスを起動し直さない (止めた意図を覆さない。pipe を見に行くだけは 3 秒おきに続く)。仕様には無い。
- [追加] `db_degraded` は hello の失敗 (`db_degraded`) と、接続中の `status` の `db.state != recording` の両方で `DbDegraded` にする (今のサービスの hello は `db_degraded` を返さず、`status` で分かるため)。
- [追加] `MYE_TRACK_PROBE=leave`: サービスを止めずに終わる (Editor.exe の終了後もサービスが生きることを手で確かめるため)。`1` が仕様の本体。
- [逸脱] 条件 3 の「`changeTrackingEnabled` を true にした一時プロジェクトで `--screenshot` (や `--frames`)」は `--project <一時> --frames 40` で実施した。`--screenshot` は回していない。
- [未実装] 仕様どおり、窓・設定の切り替え UI・`editor_saved`・`hunks` / `blob_get` は無い。

検証:
- `MSBuild MyEngine.sln` Debug|x64 と Release|x64 (どちらも `/p:MyeWarnAsError=true`) → 成功 (警告 0)。
- `MYE_SELFTEST_ONLY=changetracking Editor.exe --selftest` (Debug) → PASS 84 項目。サービスなしで動く。
- `Editor.exe --selftest` Debug → 112 スイート 0 失敗 (76.9 s)。Release → 112 スイート 0 失敗 (24.6 s)。どちらも実行中・実行後に `mye_track.exe` が存在しなかった (200 ms ごとに `Get-Process`)。
- `MYE_TRACK_PROBE=1 MYE_SELFTEST_ONLY=changetracking Editor.exe --selftest` を Debug と Release の両方で → 全 PASS (起動 → Ready → `subscribe` → ファイルを書く → `change_recorded` → `list` に出る → Editor が接続を閉じてもサービスは生存 → 別セッションが再接続して `shutdown` (role editor) → プロセスが残らない)。`MYE_TRACK_PROBE=leave` (Debug) では Editor.exe の終了後もサービス (pid 39300) が生きていた (後で自分で止めた)。
- 実 Editor (Release) の対話起動 (一時プロジェクト、`.mye\editor_settings.json` に `changeTrackingEnabled: true`、`LOCALAPPDATA` を一時へ): サービスが `"…\mye_track.exe" serve --project "…\proj"` で起動し、ログに `Connecting -> Ready`。Editor を強制終了してもサービスは生きていた。同じプロジェクトを `--project … --frames 40` で回すとサービスは起動せず、`.mye\ai_changes` も作られなかった (条件 3)。後始末: 自分が起動した Editor と mye_track のプロセスを止め、一時ディレクトリ (`%TEMP%\mye_ct_accept`、`%TEMP%\mye_selftest_44012`) を消した。
- `cargo test --manifest-path tools\track\Cargo.toml` → 全 PASS (pipe.rs 10 本 = 新規 3 本を含む、lib 32 本 = notify の単体 4 本を含む。新規 3 本は 5 回続けて PASS)。`tools\build_track.bat Release` → 成功。`tools\track_verify.bat` → 23 シナリオ全 PASS (`23_subscribe` を新規)。
- `pwsh tools\check_rules.ps1` → 0 error (61 warning は既存)。`pwsh tools\check_rules_selftest.ps1` → 規則 18 と 19 の両方 PASS。`TrackProtocol.h` の値を 2 にすると規則 9 が落ちることを確認して 1 に戻した。
- `git diff --stat ca6a3991 -- src\Engine src\Runtime src\Server src\GameLogic src\Shared src\Scripting` → 空。`ChangeTracking` の include に sim のヘッダは無い (Engine からは `PathUtil.h` / `Sha256.h` / `Log.h` だけ)。`build\Editor.vcxproj` / `.filters` の差分は追加だけ (63 行)。`C:\HAL\MyEngin\.mye` は作っていない。
- 未実行: `Editor.exe --selftest-full` (sub の検証コマンドに無く、重い Full 層は触れていない)、`tools\replay_verify.bat` (Engine 層を触っていない。sub のとおり)、`tools\mcp_verify.ps1` (`tools\mcp\src` を変えておらず、`tools\mcp\target\release` の exe をユーザーのセッションが使用中)、`--screenshot`、CI での実走、Rust 1.89 での実ビルド。

自己採点 (1-5):
  仕様適合: 4 — 条件 1〜10 を実行で確かめた。`mcp_verify` と `--selftest-full` は未実行、`--screenshot` は `--frames` で代用。通知の作り方は仕様に無い方式 (ポーリング) を選んだ (差分に書いた)。
  正しさ: 4 — 偽の I/O の試験 84 項目、実サービスとの結合 (Debug / Release)、実 Editor の対話起動、`cargo test` / `track_verify` で確認。Visual Studio のデバッグ起動など別の Job の性質、別ユーザー、長時間の運用は未確認。
  コード品質: 4 — 入口 (`TrackIo`) で I/O を分け、スレッドなし・待たない。`ChangeModel` の `Detail` などは窓 (sub-10) が使い始めてから形が固まる。
  テスト: 4 — 受け入れ条件の各項目に対応する検査がある。モデルの試験は 1 本の長い流れで、失敗すると後続の検査も巻き込む。

不安・質問:
- 通知がポーリング (最大 200 ms 遅れ) でよいか。即時にするなら `Store` の書き込みに通知の口を足すことになるが、`write_revision` / `attribution` の更新 / `coverage_gaps` / セッションの更新が別々の経路にあり、Writer の経路を触る (sub-06 / 22 の試験を巻き込む)。窓の表示にとって 200 ms は十分と見た。
- `Start-Process -Wait` (PowerShell の Job) から Editor を起動したら、Editor が終わってもサービスが生きていて `-Wait` が返らなかった。サービスが BREAKAWAY できず Job に残った (= ERROR_ACCESS_DENIED のフォールバック) ためと推定するが、Job の性質は直接読んでいない。architecture §3 に「推定・未確認」と書いた。
- 追跡の既定は無効のまま (planner の裁定)。逆の答えなら `EditorSettings.h` の既定値 1 つと `ChangeTrackingSelfTest.cpp` の `settings: tracking is off by default` を直す。
- 1 回の Poll の時間の上限 (2 ms) は実時間で測るので、遅い環境 (Debug) では 1 万行の処理が 150 フレームほどに広がりうる (試験は 500 フレームまで許す)。

触ったファイル:
- C:\HAL\MyEngin\src\Editor\ChangeTracking\ (新設 15 ファイル: TrackProtocol.h / TrackProjectId.h,.cpp / TrackLaunch.h,.cpp / TrackIo.h,.cpp / TrackClient.h,.cpp / ChangeModel.h,.cpp / ChangeTrackingSession.h,.cpp / ChangeTrackingSelfTest.h,.cpp)
- C:\HAL\MyEngin\src\Editor\App\EditorApp.h, EditorApp.cpp, EditorMain.cpp, EditorSettings.h, EditorSettings.cpp
- C:\HAL\MyEngin\src\Editor\SelfTest\SelfTestSuites.cpp
- C:\HAL\MyEngin\build\Editor.vcxproj, Editor.vcxproj.filters
- C:\HAL\MyEngin\tools\track\src\notify.rs (新設), server.rs, protocol.rs, pipe.rs, client.rs, lib.rs, main.rs
- C:\HAL\MyEngin\tools\track\tests\pipe.rs
- C:\HAL\MyEngin\tests\track\23_subscribe.ndjson, 23_subscribe.expected.ndjson
- C:\HAL\MyEngin\tools\check_rules.ps1, check_rules_selftest.ps1
- C:\HAL\MyEngin\docs\ai-change-tracking\architecture.md, ipc-contract.md
- C:\HAL\MyEngin\plans\m104-ai-change-tracking.md
- C:\HAL\MyEngin\plans\m104-ai-change-tracking\sub-09.md (実装メモ)

申し送り:
- sub-10: `ChangeTrackingSession::Model()` に `SetInterest(mask)` (窓が見ている部分)、`SetFilter`、`LoadMore`、`SelectRevision` がある。窓が開いたら `SetInterest` を立てること (立てない部分は取りに行かない)。切り替えの UI は `EditorSettings::changeTrackingEnabled` を変えたあと `ChangeTrackingSession::Start` をやり直す (`NoService` の探し直しも `Start` し直しで行う)。`editor_saved` は `role_allows` と `IMPLEMENTED_OPS` に足してから。
- sub-10: 通知の `actor` は `change_recorded` と `attribution_updated` に付く。`resync` は購読の対象でなく必ず届く (`ChangeModel` が処理済み)。
- reviewer: モデルの試験は `RunChangeTrackingSelfTest` の (g)。フレーム時間の試験は実時間で測っている (閾値は 50〜200 ms と緩い)。
- `tests\track\23_subscribe.ndjson` を足したので、`track_verify.bat` は 23 シナリオになった。
- この作業が残したプロセス・一時ファイルは無い (止めたもの: 自分が起動した Editor.exe 1 本と mye_track.exe 2 本、消したもの: `%TEMP%\mye_ct_accept`、`%TEMP%\mye_selftest_44012`、`%TEMP%\ct_*.txt` / `st_*.txt`)。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-15)。確かめたこと: `mye_track` / `Editor` の残存プロセスなし (MSBuild の待機ノードが 5 本残っているが、ビルドの後に自分で消える常駐で、止めていない)、リポジトリに `.mye\ai_changes\` なし、下位の層 (`src\Engine` ほか 5 つ) の差分は空、`bin\x64\Debug|Release\Editor.exe` は最後のソースの変更より新しい。planner の実走: `MYE_SELFTEST_ONLY=changetracking` の Debug が終了コード 0、Release の `--selftest` 全体が 112 スイート 0 失敗 (23 秒) で、実行中 (300 ms ごとに確認) も実行後も `mye_track.exe` なし、`tools\check_rules.ps1` 0 error、`tools\check_rules_selftest.ps1` が規則 18 と 19 の両方で PASS、`cargo test` (track) 194 件通過、`tools\track_verify.bat` 全シナリオ PASS。コードで確かめたこと: `TrackLaunch.cpp` 70 行目の `CreateProcessW` が `bInheritHandles = FALSE`、`EditorSettings.h` 48 行目の既定が false、`EditorMain.cpp` 587 行目の `allowChangeTracking = !automation`、`ChangeTracking\` が include する Engine のヘッダは `Log.h` / `PathUtil.h` / `Sha256.h` だけ。
  - 追加の判定: 通知を「購読者がいる間だけ読み取り専用の接続で 200 ms ごとに引く」方式にした、通知に `actor` を足した、`--notify-queue`、`shutting_down` の後は起動し直さない、`DbDegraded` を `status` からも判定、`MYE_TRACK_PROBE=leave`、は全部採る。
  - 逸脱 (`--screenshot` の代わりに `--frames 40` で確かめた): 可。どちらも `automation` を真にする同じ経路。
  - 質問への回答: (1) 通知は 200 ms の遅れでよい。記録の経路に手を入れない方を採る。(2) Job の中から起動された Editor のサービスが Job に残る件は「推定・未確認」のままでよい。(3) 既定は無効のまま。(4) `Poll` の 2 ms の上限はそのままでよい。
  - 申し送りどおり、計画の進捗表の M104i の行が古い。sub-10 で直す。