# sub-09: Editor の pipe クライアント、モデル、規則 19 (M104j)

- 依存: sub-08
- 状態: 未着手
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

## フィードバック履歴
