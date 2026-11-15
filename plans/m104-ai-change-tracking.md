# M104: AI Change Tracking

AI エージェントが行ったコード・シーン・アセットの変更を追跡し、Editor で差分表示・承認・部分復元する。
要求の正本は `docs\ai-change-tracking\spec-v1.1.md`、設計は同じディレクトリの 5 本と `docs\adr\ADR-039-ai-change-tracking.md`。

## 再開手順 (セッション開始時に読む)

1. `git log --oneline --grep "^M104"` で終わったサブを確かめる (進捗の一次情報)
2. 下の進捗表で次のサブを選び、設計文書の該当する節を読む。**この計画は実装済みの証拠ではない** — 現状はコードで確かめる
3. 「未決事項」に、そのサブを止めるものが残っていないか見る。残っていればユーザーに確認してから始める
4. サブごとに検証 (下の「検証」) → コミット (`M104x: ...`)
5. 終わったら進捗表を更新し、計画と違った事実と後続への申し送りだけを追記する。設計が変わったら設計文書と ADR-039 を直す

## 進捗

| サブ | Phase | 状態 | 内容 |
|---|---|---|---|
| M104a | 0 | 完了 | 設計文書 6 本、ADR-039、この計画 |
| M104b | 0 | 完了 | 試作 6 件 (下の「試作で確かめること」)。結果は [prototype-results.md](../docs/ai-change-tracking/prototype-results.md)。設計文書へ反映済み |
| M104c | 1 | 完了 | `tools\track` の骨組み、SQLite の初期化と移行、Blob ストア、イベントジャーナル |
| M104d | 1 | 完了 | ファイル監視、ベースライン、スナップショット、欠落区間の記録、起動時の検査 |
| M104d2 | 1 | 完了 | 取りこぼしの検出 (検証の走査)、記録に失敗した観測の欠落区間、ベースラインの中断と進捗、ベースライン・記録の計測と軽量化 (1 件ごとの `sync_all` をやめた)。結果は [performance.md](../docs/ai-change-tracking/performance.md) |
| M104e | 1 | 完了 | 行 diff (Myers)、Hunk、Change / Revision の確定 (`write_revision`)、リネームのまとめ (窓をまたぐ預かり込み)、DB の作り直しの再構成、Diff ワーカー。保存形式は db-schema.md §3.2、規則と上限の実測は architecture.md §5 |
| M104f | 1 | 実装済み (判定待ち) | pipe サーバ (`mye_track serve`)、`hello`、単一インスタンス (mutex + `service.lock` + pipe 名)、`status` / `list` / `describe` / `sessions` / `shutdown`、`mye_track client`、`tools\build_track.bat`、`tools\track_verify.bat` (シナリオ 11 本)、CI への段の追加 (CI での実走は未確認)。プロトコルは ipc-contract.md §9 |
| M104g | 2 | 実装済み (判定待ち) | `session_begin` / `session_end` / `op_begin` / `op_end` / `report` (pipe。mcp・hook)、帰属 (根拠 10 種 + `conclude`、後から届く根拠で計算し直す)、セッションの基準 (時刻とジャーナルの位置、D30)、管理された操作の即時の読み取り (D31)、`clientOpId` (D32)、`worktree` の構造確認 (D33)、宣言された編集の適用、`.git` の読み取り (`baseCommit`)、寿命 (中断・放棄・無接続の時間切れの条件)。セッション・操作・根拠はイベントから作り直せる。`tools\track` の中だけ (MCP は M104h、フックは M104i)。設計文書 (attribution-rules / ipc-contract / db-schema) を同時に更新 |
| M104g2 | 2 | 実装済み (判定待ち) | 監視の取りこぼしを減らす。先に計測 (`mye_track bench flood` / `burst` / `plan`。洪水の最中の追跡対象 200 件が、CPU 負荷のもとで 8〜21 件 `gap` になると確認)、監視を分ける (`tools\track\src\watch_plan.rs`: 無視するディレクトリを含む所は非再帰、含まない子は再帰。付け替え、上限 256 本、`status` の `watches`)。分けた後は 12 回とも `gap` 0。自前の `ReadDirectoryChangesW` は、入れる根拠の実測が無く、試作の欠陥の原因も未解明なので入れていない。値と手順は [performance.md](../docs/ai-change-tracking/performance.md) §8 |
| M104h | 2 | 実装済み (判定待ち) | MCP の 5 ツール (`change_session_begin` / `change_session_end` / `change_report` / `change_list` / `change_describe`)、`tools\mcp\src\track_client.rs` (pipe クライアント、サービスの切り離し起動、SHA-256 と projectId を自前で持つ。依存は増やさない)、規則 15-c / 15-d の許可表と規則 9 の版定数の組、`tools\mcp_verify.ps1` の `# env`、`tools\track_verify.ps1` の `# mcp` / `# mcp-nostart` / `# wait-watching` とシナリオ 16 / 17 (MCP がサービスを起動して記録が通る)、ADR-032 と `docs\mcp-server.md` の更新。起動のとき自分の標準入出力を継承不可にする (しないとサービスが MCP の stdout を掴む。`# mcp` が実測で検出した)。**未確認**: 本物の Claude Code 終了後のサービスの生存 (手順は `docs\test_checklists.md`) |
| M104i | 2 | 完了 | フック CLI (`mye_track.exe hook`)、Claude Code のフック設定の手順 (本物のフックの実走は未確認) |
| M104j | 3 | 実装済み (判定待ち) | Editor の pipe クライアント (`src\Editor\ChangeTracking\`。スレッドなし・`Poll()` は待たない・overlapped)、`ChangeModel` (通知で古い印・500 ms の間引き・`resync`)、`ChangeTrackingSession` (`EditorApp` のメンバ。Shutdown は接続を閉じるだけ)、サービスの切り離し起動 (`bInheritHandles = FALSE`)、`EditorSettings` の `changeTrackingEnabled` (既定 false) と `allowChangeTracking = !automation`、サービス側の `subscribe` と通知 5 種 + `resync` (`tools\track\src\notify.rs`)、規則 19 と規則 9 の C++ の版定数、`changetracking` スイート (偽の I/O。`MYE_TRACK_PROBE=1` で実サービスとの結合)。窓は M104k |
| M104k | 3 | 実装済み (判定待ち) | AI Changes 窓 (`src\Editor\Windows\Project\AiChangesWindow`。状態の帯、有効・無効の切り替えと確認、セッション / 変更 / 詳細の 3 列、Unknown・Inferred の別の節、根拠の一覧、`completeness` の注記)、Diff 表示部品 (`src\Editor\Widgets\DiffView.h`、クラス名は `DiffViewer`。インライン / 左右、`ImGuiListClipper`)、文面の整形 (`ChangeFormat`、ImGui を呼ばない)、サービスの `hunks` (本文つき・ページ送り・単独で大きい Hunk は `oversize`) と `blob_get` (512 KiB ずつ)、`sessions` に `changeCount`。`editor_saved` は M104k2 |
| M104k2 | 3 | 実装済み (判定待ち) | Editor の保存をサービスへ知らせる (`editor_saved`)。サービス: op (role = editor だけ)、観測が先でも知らせが先でも同じ照合 (前後 10 秒、ハッシュ一致)、`events` から作り直せる根拠、スキーマ版 2 (`events(kind, at)` の索引)、`list` の `attribution` に配列。Editor: 保存直後の通知の口 (`EditorApp::NoteFileSaved`) から接続中だけ、書いた後のファイルのハッシュを `Poll()` で 1 回 2 件まで送る。経路の一覧は `docs\ai-change-tracking\editor-save-routes.md`。AI Changes 窓: 「エディタ / 復元による変更」の節 (切り替えで表示)、「セッションなし」をサービスの絞り込みで取る。M104k の後、M104l の前 |
| M104l | 3 | 実装済み (判定待ち) | シーン構造 diff (サービス)、プレハブの区別、`scene_diff` |
| M104m | 3 | 実装済み (判定待ち) | シーンの変更の集約 (サービスの `scene_overlay` / `document_for_path`)、Editor の `SceneChangeOverlay` (表・取り直しの契機・「ここまで確認済みにする」)、Hierarchy のバッジ (通常表示と検索表示の両方、子孫の薄い印)、AI Changes 窓の帯と「シーンの変更」の一覧、一覧の行から `Selection` への同期。sub-23 から持ち越した `editor_saved` の大きさ・更新時刻の確認も含む。Inspector の Field 差分・Scene View のワイヤ箱・色の設定は M104m2 |
| M104m2 | 3 | 未着手 | Inspector の Field 差分、Scene View のワイヤ箱、色の設定 (4 色)。M104m の後、M104n の前 |
| M104m3 | 3 | 実装中 | 追跡クライアントの試験から実時間への依存を外す。`TrackIo::NowMicros` を足し、`Poll()` の時間の上限を差し替えられる時計で測る (製品の挙動は同じ)。試験は偽の時計で決定的にする |
| M104n | 3 | 未着手 | 未保存シーンへの外部変更の確認 (`ReloadHub` の口)。他のサブから独立しているので前倒しできる |
| M104o | 4 | 未着手 | レビュー (範囲ごとの承認・却下・要確認・再開)、集約表示 |
| M104p | 5 | 未着手 | ゲートの共有、ファイルと Hunk の復元、復元ジャーナル、起動時の回復 |
| M104q | 5 | 未着手 | ライブのシーンの復元 (Entity / Component / Field / Override)、Undo 連携 |
| M104r | 6 | 未着手 | 削除エンティティのゴースト、Scene Overlay |
| M104s | 6 | 未着手 | Before / After のプレビュー、左右並べた 3D 比較 |
| M104t | 6 | 未着手 | プレハブ伝播の表示、大きいシーンでの表示の計測と軽量化 |

仕様書 §19 の順序 (記録 → 帰属 → 可視化 → 承認 → 復元 → 3D 比較) を守る。Phase をまたいで先へ進まない。

## 予約する番号

| もの | 値 | 備考 |
|---|---|---|
| ADR | ADR-039 | 作成済み |
| `check_rules` の規則 | 規則 19 | `src\Editor\ChangeTracking\` の封じ込め (M104j で実装済み。次の番号は 20) |
| ABI / SimSnapshot / TypeId | 使わない | 変えたら設計違反 ([architecture.md](../docs/ai-change-tracking/architecture.md) §9) |
| プロトコル版 | 1 | `kTrackProtoVersion` / `PROTO_VERSION` (規則 9 に組を足す) |
| DB スキーマ版 | 1 | |
| 依存の版 (`tools\track`) | `rusqlite` 0.42 (bundled)、`sha2` 0.11、`notify` 8、`windows-sys` 0.61、`rust-version = "1.89"` | M104b の試作で確かめた組。`sha2` の出力は `{:x}` で整形できない |
| pipe の名前 / mutex の名前 | `\\.\pipe\mye-track-<projectId>` / `Local\mye-track-<projectId>` | [ipc-contract.md](../docs/ai-change-tracking/ipc-contract.md) §1、[architecture.md](../docs/ai-change-tracking/architecture.md) §3 |

番号は着手時に現在の末尾を確かめ直す (他のマイルストーンが先に使っていたら次へずらす)。

## 試作で確かめること (M104b)

設計が前提にしているが、まだ確かめていないこと。外れたら後続が無駄になるので最初にやる。
M104b (2026-11-14) で 6 件とも走らせた。試作コードはコミットせず、手順・コマンド・観測だけを [prototype-results.md](../docs/ai-change-tracking/prototype-results.md) に残した。

| # | 確かめること | 外れた場合 | 結果 (2026-11-14) |
|---|---|---|---|
| 1 | Rust から、現在のユーザーに限りリモートを拒否する名前付きパイプを張り、C++ (overlapped I/O) と往復できる。Win32 API は自前の `extern "system"` 宣言と `windows-sys` のどちらが保ちやすいか | 手段を変える。責務の分け方は変えない | 往復できた。**`windows-sys` 0.61 を採る**。クライアントは `ERROR_PIPE_BUSY` を再試行する。別ユーザーの拒否は未確認 |
| 2 | `rusqlite` の bundled が開発機と CI のランナーでビルドできる (C コンパイラが要る) | C++ 側に SQLite の単一ソースを `external\` へ置く形は取れない (サービスは Rust)。ビルド済みの SQLite を置くか、別の保存形式を検討する | 開発機でビルド・WAL の読み書きを確認 (`rusqlite` 0.42、最低 Rust 1.89)。**CI は未確認で、sub-05 (M104f) で確かめる** |
| 3 | `notify` が、短い間隔の連続書き込み・削除・リネーム・「一時ファイルへ書いて rename」をどう返すか | デバウンスの幅と、リネームのまとめ方 ([architecture.md](../docs/ai-change-tracking/architecture.md) §5) を直す | 直した。パスごとに 300 ms / 最大 2 秒、書き込み中は共有違反で見分ける、別ディレクトリへの移動は `Remove` + `Create`、ディレクトリ単位の操作はイベントに頼れない |
| 4 | C# コンポーネントのフィールドを `FieldFromJson` で書けるか | C# コンポーネントは Entity 単位の復元だけにする ([restore-safety.md](../docs/ai-change-tracking/restore-safety.md) §4.2) | **書けない** (コードで確定)。ただし `DeserializeComponent` に 1 キーの JSON を渡す別の分岐で Field も戻せる見込み。実走は sub-16 |
| 5 | Claude Code のフックが渡す入力の項目名と設定方法 (公式の文書で確かめる)。Codex CLI と Gemini CLI に同等の仕組みがあるか | 無いクライアントは Claimed 止まり ([attribution-rules.md](../docs/ai-change-tracking/attribution-rules.md) §6.1) | Claude Code は確認済み (終了コード 2 を返さない、`timeout` を明示、`tool_use_id` で対にする)。入力から After のハッシュを計算できた。Codex / Gemini は**未確認**。本物の入力の実測は sub-08 |
| 6 | サービスが親と運命を共にしないか (親の Job Object)。MCP 経由と Editor 経由の起動 | 縮退を書く: 親と一緒に終わったら次の起動で `service_down` の欠落を記録する ([architecture.md](../docs/ai-change-tracking/architecture.md) §3) | Job 次第と実測した。**`CREATE_BREAKAWAY_FROM_JOB` で起動し、`ERROR_ACCESS_DENIED` なら外して起動し直す**。本物の `mye_mcp.exe` は Job の中にいた (性質は読めず)。本物のクライアントを終わらせる実験は未実施 |

## 未決事項 (ユーザーの判断が要る)

| # | 決めること | 推奨 | 止まるサブ |
|---|---|---|---|
| 1 | Play 中に、開いているシーンへ外部の変更が来たときの扱い。今は Play 中の World へそのまま適用され、Stop で消える | Play 中は保留し、Stop の後で未保存かどうかに従って適用か確認 | M104n |
| 2 | `tools\track` を CI に入れるか (collab は入れている、mcp は入れていない) | **決着 (2026-11-15)**: `cargo test` と `track_verify.bat` を CI に入れる (collab の段の隣)。CI での実走は push の後まで未確認 | M104f |
| 3 | 追跡を既定で有効にするか | **推奨どおり裁定 (2026-11-15、ユーザーの回答待ち)**: 既定で無効 (`EditorSettings.changeTrackingEnabled`)。Phase 3 が終わった後 (M104m の後) にもう一度決める。逆の答えなら、既定値 1 つと試験の期待値を直す | M104j |
| 4 | Blob の除外パターンの既定値と、上限・保存期間の既定値 (2 GiB、90 日) | 設計どおり (2026-11-14 にユーザーが確定)。設定で変えられる | M104c |
| 5 | ハイライトの色を利用者が変えられるようにする範囲 (色の設定を保存する前例が無い) | `EditorSettings` に 4 色だけ持つ。設定の画面は AI Changes 窓の歯車 | M104m |
| 6 | 実装を `/harness` に載せるか、1 サブ 1 セッションで進めるか | 規模からは `/harness` | M104b 以降の全部 |

## 実装時に直す既存のもの

設計文書の作成 (M104a) では既存のファイルを 1 つも変えていない。実装で触るのは次のとおり。

| ファイル | 変更 | サブ |
|---|---|---|
| `tools\check_rules.ps1` | 規則 9 に版定数の組、規則 15-c / 15-d の許可表に `track_client.rs`、規則 19 を新設 | M104h (規則 9 と 15 は済み)、M104j (規則 19) |
| `tools\mcp\src\tools.rs`、`main.rs` | 道具 5 つの定義と振り分け、`mod` 宣言 | M104h (済み) |
| `tests\mcp\tools_list.expected.ndjson` | 撮り直し (道具が 10 になる) | M104h (済み) |
| `docs\adr\ADR-032-engine-mcp-server.md`、`docs\mcp-server.md` | 「道具は 5 つ」と許可表の記述 | M104h (済み) |
| `src\Engine\Engine\HotReload\ReloadHub.h` / `.cpp` | 開いているシーンの外部変更を適用してよいか問う口 | M104n |
| `src\Editor\App\EditorApp.h` / `.cpp` | 窓の登録、サービスの起動、ゲート入力の共有、外部変更の確認 | M104j、M104k、M104n、M104p |
| `src\Editor\Windows\Scene\HierarchyWindow.cpp`、`InspectorWindow.cpp`、`SceneViewWindow.cpp` | バッジ、Field 差分、ワイヤ箱 | M104m |
| `src\Editor\SelfTest\SelfTestSuites.cpp` | `changetracking` スイートの登録 | M104j |
| `src\Engine\Core\Localization\LocalizationTable.inl` | UI 文字列 (英日) | M104k 以降 |
| `build\Editor.vcxproj` と `.filters` | 新しいソース (`tools\gen_project_files.ps1`) | M104j 以降 |
| `engine_spec.md` | ホットリロードの章 (外部のシーン変更の扱い)、エディタの章 | M104n、M104k |
| `docs\test_checklists.md` | 手動確認の項目 | Phase 3 以降 |

## 検証

| 対象 | 手段 |
|---|---|
| サービス | `cargo test --manifest-path tools\track\Cargo.toml`。一時 git リポジトリは `tools\collab\tests\service.rs` の `temp_repo()` の前例。`tools\track_verify.bat` で pipe 越しのシナリオを照合 (`tools\mcp_verify.ps1` と同じ形) |
| MCP | `tools\mcp_verify.bat` |
| Editor | `MYE_SELFTEST_ONLY=changetracking` で `Editor.exe --selftest`。サービスなしで動くよう、偽の応答行を流し込む (`SourceControlSelfTest` の前例) |
| 規則 | `tools\check_rules.ps1`。新しい規則は、違反を仕込んだ一時コピーで落ちることを確かめる |
| 決定性 | 追跡を有効にした場合と無効にした場合で `tools\replay_verify.bat` が一致。ハイライトの有無で `HashWorld` が一致 |
| 受け入れ | 仕様書 §16 の各項目を、サービスのテスト・Editor のスイート・手動確認のどれで確かめるかを、サブの着手時に割り当てる |

仕様書 §18 のとおり、変更検出・帰属・差分生成・承認状態・復元処理・永続化・障害回復を、それぞれ独立して自動テストできるようにする。
サービスのテストは Editor なしで、Editor のスイートはサービスなしで動くことがその条件。

## 申し送り

- M104b (2026-11-14、試作 6 件。結果の詳細は [prototype-results.md](../docs/ai-change-tracking/prototype-results.md)):
  - 後続のサブが使う決定: pipe は `windows-sys` 0.61 (feature 7 つは ipc-contract §1)。クライアントは `ERROR_PIPE_BUSY` を `WaitNamedPipeW` で再試行する。サーバの最初の 1 本は `FILE_FLAG_FIRST_PIPE_INSTANCE`。
  - M104d (ファイル監視) が使う決定: デバウンスはパスごと 300 ms / 最大 2 秒。flush は `FILE_SHARE_READ` だけで開き、共有違反なら 250 ms 後にやり直す (30 秒で打ち切り `coalesced`)。別ディレクトリへの移動は `Remove` + `Create` なので、削除 + 同じハッシュの作成のまとめが主たる手段。ディレクトリ単位の操作 (名前変更・移し入れ・移し出し) は中のファイルのイベントが来ないので、ベースラインと突き合わせる。バッファあふれは再現しなかったので、sub-03 で人工的に起こして確かめる。
  - M104d (実装の結果、2026-11-14): 上の「`FILE_SHARE_READ` だけで開く探り」は採らなかった (書き手の rename が `ERROR_ACCESS_DENIED` で失敗した)。全共有で読み、サイズが続けて同じになるまで待つ方式に変えた ([architecture.md](../docs/ai-change-tracking/architecture.md) §4)。`notify` 8.2.2 は Windows で本物のバッファあふれを報告しない (ソースで確認)。あふれの経路は差し込みで検証した。
  - M104f (CI): `rusqlite` の bundled が CI の `windows-2022` でビルドできるかを確かめる (未確認)。最低の Rust が 1.89 に上がる (collab の「古い stable でも通る」方針は track では成り立たない)。
  - M104i (フック): 終了コード 2 を返さない、`timeout` を明示、`tool_use_id` で対にする。本物の Claude Code の入力の実測 (キー名・パスの形・CRLF や引用符の扱い) は未確認。Codex / Gemini は未確認のまま (v1 は Claude Code だけを約束する)。
  - M104j (Editor): **決着 (2026-11-15)**: `StartChildProcess` は使わず、`TrackLaunch.cpp` に専用の起動口を足した (`bInheritHandles = FALSE`、`CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB`、`ERROR_ACCESS_DENIED` なら外してやり直す)。Job の中から起動した結果は [architecture.md](../docs/ai-change-tracking/architecture.md) §3。Visual Studio のデバッグ起動の Editor からの起動は未確認。
  - M104q (ライブのシーンの復元): C# コンポーネントの Field は `FieldFromJson` では書けない。`DeserializeComponent` へ 1 キーの JSON を渡す分岐を作り、実走で確かめる。
- M104a (2026-11-13): 既存コードの監査で、仕様書の「既存機能の再利用」の前提の多くが成り立たないと分かった。差は [architecture.md](../docs/ai-change-tracking/architecture.md) §1。
  ユーザー判断は 4 つ: 今回は設計文書だけ、設計の範囲は Phase 0〜6 の全部、サービスは独立した exe、未保存のシーンへの外部変更は確認を出す。
- 監査のついでに見つけた、この機能とは別の事実 (直していない):
  - `AGENTS.md` §4.1 は生の D3D 型の境界を「規則15」と書いているが、`check_rules.ps1` では規則 16。
  - `docs\mcp-server.md` と ADR-032 は `verify_job` を「22 ジョブ」と書いているが、`tools\mcp\src\verify.rs` の表は 24。
  - Inspector のプレハブの Apply は Undo に記録されない。コメントにあるとおり意図した設計 (他のインスタンスとアセットファイルも更新するため) だが、追跡から見ると「Undo で戻せないシーンとファイルの変更」になる。
  - `EditorApp` の `freshBlockers` は `tilePaletteDirty` だけキャッシュの値を使っている。
