# sub-09: Editor の pipe クライアント、モデル、規則 19 (M104j)

- 依存: sub-08
- 状態: 未着手
- 往復: 0
- 書き込みの深さ: **骨子** (spec §6.1)。sub-08 の VERDICT で planner が Phase 3 を見直してから着手する。

## 着手前にユーザー確認が要る未決事項

司会が着手時に聞く (計画の未決事項 3)。

- 聞きたいこと: 追跡を既定で有効にするか。
- 推奨: Phase 3 が終わる (sub-13 まで) までは既定で無効。`EditorSettings` の設定で有効にする。見る手段が無いうちに記録だけ溜めない。
- 逆を選ぶと何が変わるか: 既定で有効にすると、Editor を対話起動するたびにサービスが起動し `.mye\ai_changes\` が育つ。受け入れ条件 3 (非対話起動では触れない) は変わらない。既定値の定数とテストの期待値が変わる。
- 「Phase 3 完了時に既定を有効へ切り替えるか」は、sub-13 の後でもう一度聞く。

## やること

Editor 側の土台。**C++ 側にスレッドを作らない** (arch §3.1)。

1. `src\Editor\ChangeTracking\` を新設: `TrackProtocol.h` (`kTrackProtoVersion`)、`TrackClient` (overlapped I/O、毎フレーム `Poll()`、接続 → 無ければ `StartChildProcess` でサービス起動 → やり直し、`hello` (`role = editor`)、`bad_token` で `service.json` を読み直して 1 回だけやり直す、切断後は数秒おきに再接続)、`ChangeModel` (一覧・セッション・状態の保持、ページ単位の取得)。
   - sub-01 の結果 (2026-11-14 の見直し): pipe は `CreateFileW(FILE_FLAG_OVERLAPPED)`、`ERROR_IO_PENDING` は保留して次の `Poll()` で `GetOverlappedResult(bWait = FALSE)`。`ERROR_PIPE_BUSY` は**待たずに**次のフレーム以降でやり直す (`WaitNamedPipeW` はフレームを止めるので使わない)。
   - サービスの起動に `StartChildProcess` をそのまま使わない: `bInheritHandles = TRUE` でログファイルのハンドルを渡すので、サービスが生きている間そのハンドルを持ち続け、Editor の他の継承可能なハンドルも渡る (`C:\HAL\MyEngin\src\Editor\App\ChildProcess.cpp` 30 行目)。**ハンドルを継がせない、サービス用の起動口を `src\Editor\ChangeTracking\` に置く** (`bInheritHandles = FALSE`、`CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB`、`ERROR_ACCESS_DENIED` なら BREAKAWAY を外してやり直す。arch §3)。`StartChildProcess` 自体は変えない。arch §3 の「Editor は既存の `StartChildProcess` を使う」をこのサブで直す。
   - Editor が Job の中で起動された場合 (Visual Studio のデバッグ起動) にサービスが生き残るかを 1 回確かめ、結果か「未確認」を arch §3 に書く。
2. projectId の計算を C++ 側に持ち、sub-02 の固定表と同じ入出力でテストする (`C:\HAL\MyEngin\src\Engine\Platform\Sha256.h`)。
   - sub-02 の VERDICT (spec §2 D25): projectId は「`NormalizePathKey` の結果から末尾の区切りを落としたもの (ドライブのルートは除く)」のハッシュ。固定表は `C:\HAL\MyEngin\tools\track\tests\project_id_cases.tsv` (4 列)。C++ 側のテストはこのファイルを読むか、同じ行を持つ。`towlower` は "C" ロケールで ASCII だけを変換する前提で Rust を合わせてあるので、Editor が `setlocale` を呼んでいないことを確かめ、表の非 ASCII の行が通ることで検出する。
   - sub-21 の計測 (2026-11-15、`C:\HAL\MyEngin\docs\ai-change-tracking\performance.md`): このリポジトリのベースラインは Blob が約 2950 本・約 488 MB で、所要 12〜35 秒。既定の上限 2 GiB の約 4 分の 1 を最初のベースラインだけで使い、最新のベースラインは回収されない (db §7)。Editor は、ベースラインの進捗 (`status` の `scan_done` / `scan_total`) と Blob の総量を、追跡の状態として持つ (表示は sub-10)。
3. 設定: `EditorSettings` に追跡の有効 / 無効。プロジェクトなしの起動では追跡しない (db §1)。
4. **非対話起動 (`--selftest*`、`--screenshot`、リプレイ検証、`--dump-index` など) では、設定にかかわらず接続も起動もしない** (spec §2 D11)。判定に使う既存のフラグを `EditorApp` / `EngineCli` で確かめて使う。
5. 状態の表示は最小限: サービスの状態 (接続中 / 停止中 / 版が違う / DB が使えない) を持つだけ。窓は sub-10。
6. `changetracking` スイート (`ChangeTrackingSelfTest.cpp` / `.h`、`SelfTestSuites.cpp` に登録、Fast 層): 偽の応答行を流し込んで、応答の解釈・順不同の `id`・通知・エラーコードごとの扱い・1 行が複数回の読み取りに分かれる場合・切断と再接続を確かめる (`SourceControlSelfTest` の前例)。
7. `check_rules.ps1` に規則 19: `src\Editor\ChangeTracking\` を Engine / Runtime / GameLogic / Shared / Server から include しない (規則 15-a の形。spec §2 D8)。番号は着手時に末尾を確かめる。`check_rules_selftest.ps1` に違反の一時コピーの検査。規則 9 の組に `TrackProtocol.h` を足す (spec §2 D7)。
8. `tools\gen_project_files.ps1` で `build\Editor.vcxproj` と `.filters` を更新。
9. サービス側に `subscribe` と通知が要る場合、このサブで足す (見直しで確定)。

## やらないこと (このサブでは)

- 窓・バッジ・ハイライト (sub-10、12)。レビュー・復元 (sub-14〜16)。
- Engine 層の変更。`Runtime.exe` / `Server.exe` からの参照。

## 触る場所 (planner の見立て)

- 新規: `C:\HAL\MyEngin\src\Editor\ChangeTracking\`
- `C:\HAL\MyEngin\src\Editor\App\EditorApp.h` / `.cpp`、`EditorSettings.h`、`C:\HAL\MyEngin\src\Editor\SelfTest\SelfTestSuites.cpp`
- `C:\HAL\MyEngin\tools\check_rules.ps1`、`check_rules_selftest.ps1`、`C:\HAL\MyEngin\build\Editor.vcxproj` / `.filters`
- 参考: `C:\HAL\MyEngin\src\Editor\SourceControl\CollabClient.cpp`、`SourceControlSelfTest.cpp`、`C:\HAL\MyEngin\src\Editor\App\ChildProcess.h`

## 受け入れ条件 (このサブ)

1. `changetracking` スイートがサービスなしで通る (Debug と Release) → `MYE_SELFTEST_ONLY=changetracking` で `Editor.exe --selftest`。
2. 規則 19 が通り、Engine 側の一時コピーに include を仕込むと落ちる。規則 9 の 3 site の組が通る → `tools\check_rules.ps1`、`tools\check_rules_selftest.ps1`。
3. `Editor.exe --selftest` の実行中と実行後に `mye_track.exe` のプロセスが存在しない。追跡を有効にした設定でも同じ → 実行して `Get-Process` で確かめる。
4. サービスの exe が無い、`proto_mismatch`、`db_degraded`、応答なし (時間切れ) のそれぞれで、クライアントが「追跡停止」の状態になり、例外も無限の再試行も起こさない → スイート (要求 §16.5 の 3、4、7 番目)。
5. `Poll()` が 1 フレームで処理する量に上限がある (大量の通知でフレームを止めない) → スイート。
6. C++ の projectId が sub-02 の固定表と一致する → スイート。
7. 追跡を無効にした Editor は、`src\Editor\ChangeTracking\` のコードが pipe にも `.mye\ai_changes\` にも触れない → スイート (偽の I/O の口が 1 回も呼ばれないこと)。

## 検証コマンド

- `MyEngine.sln` を `Debug|x64` と `Release|x64` でビルド
- `MYE_SELFTEST_ONLY=changetracking` で `bin\x64\Debug\Editor.exe --selftest`、続けて `bin\x64\Debug\Editor.exe --selftest`
- `pwsh -NoProfile -File tools\check_rules.ps1` / `pwsh -File tools\check_rules_selftest.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴
