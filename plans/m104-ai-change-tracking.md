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
| M104b | 0 | 未着手 | 試作 3 件 (下の「試作で確かめること」)。結果を設計文書へ反映する |
| M104c | 1 | 未着手 | `tools\track` の骨組み、SQLite の初期化と移行、Blob ストア、イベントジャーナル |
| M104d | 1 | 未着手 | ファイル監視、ベースライン、スナップショット、欠落区間の記録、起動時の検査 |
| M104e | 1 | 未着手 | 行 diff (Myers)、Hunk、Change / Revision の確定、リネームのまとめ |
| M104f | 1 | 未着手 | pipe サーバ、`hello`、単一インスタンス、`status` / `list` / `describe`、`tools\track_verify.bat` |
| M104g | 2 | 未着手 | セッションと操作の登録、帰属の判定、セッションの基準、複数セッション |
| M104h | 2 | 未着手 | MCP の 5 ツール、`track_client.rs`、規則 15 の許可表、ADR-032 と `docs\mcp-server.md` の更新 |
| M104i | 2 | 未着手 | フック CLI (`mye_track.exe hook`)、Claude Code のフック設定の手順 |
| M104j | 3 | 未着手 | Editor の pipe クライアント、モデル、規則 19、`changetracking` スイート |
| M104k | 3 | 未着手 | AI Changes 窓、セッション履歴、Diff 表示部品 (インライン / 左右) |
| M104l | 3 | 未着手 | シーン構造 diff (サービス)、プレハブの区別、`scene_diff` |
| M104m | 3 | 未着手 | Hierarchy のバッジ、Inspector の Field 差分、Scene View のワイヤ箱、選択の同期 |
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
| `check_rules` の規則 | 規則 19 | `src\Editor\ChangeTracking\` の封じ込め (M104j) |
| ABI / SimSnapshot / TypeId | 使わない | 変えたら設計違反 ([architecture.md](../docs/ai-change-tracking/architecture.md) §9) |
| プロトコル版 | 1 | `kTrackProtoVersion` / `PROTO_VERSION` (規則 9 に組を足す) |
| DB スキーマ版 | 1 | |

番号は着手時に現在の末尾を確かめ直す (他のマイルストーンが先に使っていたら次へずらす)。

## 試作で確かめること (M104b)

設計が前提にしているが、まだ確かめていないこと。外れたら後続が無駄になるので最初にやる。

| # | 確かめること | 外れた場合 |
|---|---|---|
| 1 | Rust から、現在のユーザーに限りリモートを拒否する名前付きパイプを張り、C++ (overlapped I/O) と往復できる。Win32 API は自前の `extern "system"` 宣言と `windows-sys` のどちらが保ちやすいか | 手段を変える。責務の分け方は変えない |
| 2 | `rusqlite` の bundled が開発機と CI のランナーでビルドできる (C コンパイラが要る) | C++ 側に SQLite の単一ソースを `external\` へ置く形は取れない (サービスは Rust)。ビルド済みの SQLite を置くか、別の保存形式を検討する |
| 3 | `notify` が、短い間隔の連続書き込み・削除・リネーム・「一時ファイルへ書いて rename」をどう返すか | デバウンスの幅と、リネームのまとめ方 ([architecture.md](../docs/ai-change-tracking/architecture.md) §5) を直す |
| 4 | C# コンポーネントのフィールドを `FieldFromJson` で書けるか | C# コンポーネントは Entity 単位の復元だけにする ([restore-safety.md](../docs/ai-change-tracking/restore-safety.md) §4.2) |
| 5 | Claude Code のフックが渡す入力の項目名と設定方法 (公式の文書で確かめる)。Codex CLI と Gemini CLI に同等の仕組みがあるか | 無いクライアントは Claimed 止まり ([attribution-rules.md](../docs/ai-change-tracking/attribution-rules.md) §6.1) |

## 未決事項 (ユーザーの判断が要る)

| # | 決めること | 推奨 | 止まるサブ |
|---|---|---|---|
| 1 | Play 中に、開いているシーンへ外部の変更が来たときの扱い。今は Play 中の World へそのまま適用され、Stop で消える | Play 中は保留し、Stop の後で未保存かどうかに従って適用か確認 | M104n |
| 2 | `tools\track` を CI に入れるか (collab は入れている、mcp は入れていない) | `cargo test` と `track_verify.bat` を CI に入れる。試作 2 の結果で確定 | M104f |
| 3 | 追跡を既定で有効にするか | Phase 3 が終わるまでは既定で無効 (設定で有効にする)。見る手段が無いうちに記録だけ溜めない | M104j |
| 4 | Blob の除外パターンの既定値と、上限・保存期間の既定値 (2 GiB、90 日) | 設計どおり。設定で変えられる | M104c |
| 5 | ハイライトの色を利用者が変えられるようにする範囲 (色の設定を保存する前例が無い) | `EditorSettings` に 4 色だけ持つ。設定の画面は AI Changes 窓の歯車 | M104m |
| 6 | 実装を `/harness` に載せるか、1 サブ 1 セッションで進めるか | 規模からは `/harness` | M104b 以降の全部 |

## 実装時に直す既存のもの

設計文書の作成 (M104a) では既存のファイルを 1 つも変えていない。実装で触るのは次のとおり。

| ファイル | 変更 | サブ |
|---|---|---|
| `tools\check_rules.ps1` | 規則 9 に版定数の組、規則 15-c / 15-d の許可表に `track_client.rs`、規則 19 を新設 | M104h、M104j |
| `tools\mcp\src\tools.rs`、`main.rs` | 道具 5 つの定義と振り分け、`mod` 宣言 | M104h |
| `tests\mcp\tools_list.expected.ndjson` | 撮り直し (道具が 10 になる) | M104h |
| `docs\adr\ADR-032-engine-mcp-server.md`、`docs\mcp-server.md` | 「道具は 5 つ」と許可表の記述 | M104h |
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

- M104a (2026-11-13): 既存コードの監査で、仕様書の「既存機能の再利用」の前提の多くが成り立たないと分かった。差は [architecture.md](../docs/ai-change-tracking/architecture.md) §1。
  ユーザー判断は 4 つ: 今回は設計文書だけ、設計の範囲は Phase 0〜6 の全部、サービスは独立した exe、未保存のシーンへの外部変更は確認を出す。
- 監査のついでに見つけた、この機能とは別の事実 (直していない):
  - `AGENTS.md` §4.1 は生の D3D 型の境界を「規則15」と書いているが、`check_rules.ps1` では規則 16。
  - `docs\mcp-server.md` と ADR-032 は `verify_job` を「22 ジョブ」と書いているが、`tools\mcp\src\verify.rs` の表は 24。
  - Inspector のプレハブの Apply は Undo に記録されない。コメントにあるとおり意図した設計 (他のインスタンスとアセットファイルも更新するため) だが、追跡から見ると「Undo で戻せないシーンとファイルの変更」になる。
  - `EditorApp` の `freshBlockers` は `tilePaletteDirty` だけキャッシュの値を使っている。
