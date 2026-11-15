# harness 台帳: m104-ai-change-tracking

- 依頼原文: この間計画作成した差分表示のやつ実装したい
- 司会が付けた補足 (/harness の引数): M104 AI Change Tracking を M104b から実装する。計画は `plans\m104-ai-change-tracking.md` (進捗表・試作項目・未決事項)、要求の正本は `docs\ai-change-tracking\spec-v1.1.md`、設計は同ディレクトリの 5 本と `docs\adr\ADR-039-ai-change-tracking.md`。M104a (設計文書) は `ca6a3991` で完了済み。サブ分割は計画の進捗表 (M104b〜M104t、Phase 0〜6) を踏襲し、Phase をまたいで先へ進まない。
- 開始: 2026-11-14 / 基点コミット: ca6a3991ac6ea6bd217d3a3290aab24291b73a4a
- フェーズ: 実装

## サブ進捗
| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|
| sub-01 (M104b) 試作 6 件と設計文書への反映 | OK | 1 | 723bf633 | should 1 件は sub-03 で対応 (共有モード)、nit は sub-02 で対応 (進捗表) |
| sub-02 (M104c) tools\track の骨組み、DB、Blob、ジャーナル | OK | 2 | 79008917 | round 1 REWORK (projectId が末尾の区切りで変わる。仕様側の誤り) → round 2 OK。nit: failed_steps の発生経路のテストは sub-05 で足す |
| sub-03 (M104d) ファイル監視、ベースライン、欠落区間 | OK | 2 | 76d0e2df | round 1 REWORK (負荷の下で 1 回の保存が coalesced になる) → round 2 OK。nit: sub-03.md の実装メモに round 2 が無い |
| sub-21 (M104d2) 取りこぼしの検出と、ベースライン・記録の計測 | OK | 1 | 35da2917 | should: 試験の一時ディレクトリの後始末は sub-04 で対応。nit: 検証の走査の所要時間は sub-05 の status で実測 |
| sub-04 (M104e) 行 diff、Hunk、Revision、リネーム | OK | 1 | 8a07401a | nit: Hunk 書き込み失敗 + DB 作り直しで Revision が 2 件になる制限を sub-05 で db-schema.md §6 に書く。sub-05 のコミット以降は v1.sql を凍結 |
| sub-05 (M104f) pipe サーバ、hello、取得の op、track_verify.bat | OK | 1 | 14ddde31 | nit 2 件は sub-06 で対応 (時間切れの発火条件、pipe 越しの busy の試験)。CI での実走は未確認 (未 push)。Phase 1 の最後 |
| sub-06 (M104g) セッション、操作、帰属 | OK | 3 | ee65e43e | round 1 REWORK (検証の走査が記録途中の変更を gap と取り違える) → round 2 REWORK (原因は notify の 16 KiB バッファの本物の取りこぼし = 試験の前提と条件 19 が誤り。試験の分け直しと verify_gap の帰属 D35 を追加) |
| sub-22 (M104g2) 監視の取りこぼしを減らす | OK | 1 | a0f6fa85 | sub-06 の VERDICT round 2 で新設。順序は sub-06 → sub-22 → sub-07。Notion Q-4〜6 (推奨で進行) |
| sub-07 (M104h) MCP の 5 ツール | OK | 1 | 0a598efb | should 2 件は sub-08 で対応 (検証用 mye_mcp.exe の出力先を分ける、verify_job の期待値の撮り直し)。条件 12 (本物の Claude Code 終了後の生存) は未確認。Notion Q-9〜11 |
| sub-08 (M104i) フック CLI | OK | 1 | 0c511cac | nit: bad_token 再試行などの試験が MCP / フック側に無い、hook.rs の 8 秒のしきい値は CI で見る。条件 13 (本物のフックの実走) は未確認。Phase 2 の最後。Notion Q-12〜15 |
| sub-09 (M104j) Editor の pipe クライアント、モデル、規則 19 | OK | 1 | 17caa0ec | 詳細化済み (条件 10 件)。追跡は既定で無効 (planner の推奨、Notion Q-12)。初めて src\Editor を触る |
| sub-10 (M104k) AI Changes 窓、Diff 表示部品 | OK | 1 | c6b93988 | nit: 「セッションなし」の取り方は sub-23、oversize の表示は sub-15 で対応。未確認: 見た目の目視 (cache/m104k_shots の 5 枚)、手での GUI 操作 (条件 10)。Notion Q-16〜20 |
| sub-23 (M104k2) Editor の保存をサービスへ知らせる | OK | 0 | 3f14e59d | should: 読み直しの穴を狭める確認は sub-12、nit: ipc-contract.md の制御文字は sub-11。DB スキーマ版 2。未確認: 実 Editor での保存 (条件 6)、スクショの目視 (cache/m104k2_shots)。Notion Q-21〜23 |
| sub-11 (M104l) シーン構造 diff | OK | 0 | d6619bf0 | DB スキーマ版 3。nit: v1.sql のコメントに BEL (凍結なので直さない)、.meta と本体の観測順の組み合わせ試験は sub-19 の候補。計測は 1 回 (2 万エンティティ 162.9 ms)。Notion Q-24〜27 |
| sub-12 (M104m) シーンの変更の集約、一覧、Hierarchy のバッジ、選択の同期 | OK | 0 | 235887d9 | nit: 強調の入・切の保存は sub-24、pipe.rs の unused_mut は次に tools/track を触るサブ、cache/m104m_shots の古いログは残置。未確認: 見た目の目視 (cache/m104m_shots の 4 枚)、実 Editor の操作。Notion Q-28〜30 |
| sub-24 (M104m2) Inspector の Field 差分、Scene View のワイヤ箱、色の設定 | OK | 0 | 98355cb7 | should: Debug の changetracking が負荷で落ちる (既存の実時間依存。planner も 5/5 で再現) → sub-25 で直す。Debug での新規試験の通過は sub-25 の後に planner が確かめ直す。nit: 箱の可視判定は原点だけ (sub-19 で見直す)、名前・タグの印なし。未確認: 見た目の目視 (cache/m104m2_shots の 3 枚)。Notion Q-31〜33 |
| sub-25 (M104m3) 追跡クライアントの試験から実時間への依存を外す | OK | 0 | d1d4d8e7 | 負荷下の Debug changetracking 20/20 (coder)、planner も 6/6。sub-24 の新規試験の Debug 通過も確認。nit: Poll 1 回の実時間の計測は sub-19、pipe.rs の unused_mut は残り。別件: Debug の physics スイートがスタックオーバーフロー (D38。M104 では直さず、sub-14 の Debug は physics を除く)。Notion Q-34〜35 |
| sub-13 (M104n) 未保存シーンへの外部変更の確認 | OK | 0 | 42a1b552 | replay_verify PASS、Release --selftest-full 125 スイート、Debug は physics を除く 124 (いずれも coder)。should: 自分の保存で確認が出る件は sub-14。nit: Debug の Editor.exe のビルドし直しと ADR-039 の文面は sub-14。未確認: 実 Editor の操作、モーダルの目視 (cache/m104n_shots)。Notion Q-36〜38 |
| sub-14 (M104o) レビュー | OK | 0 | c9b14f4c | round 1 で OK。操作全体の一括の導線と、ファイル単位の集約表示は sub-15 へ。Rust 側の時間依存の試験の揺れは sub-26 へ |
| sub-26 (M104o2) サービス側の時間に依存する試験を、機械の負荷に強くする | OK | 0 | a53053db | round 1 で OK。負荷で結果が変わる 4 試験は `--test watch -- --ignored` へ。pipe.rs:811 はクライアントの不具合 (実走の記録なし) |
| sub-15 (M104p) 復元の判定 — ゲート、競合の検証、パスの検査、プレビュー | OK | 0 | (このコミット) | round 1 で OK。ファイルを書かない (planner が grep で確認)。「復元する」は常に押せない。list に operationId を追加 |
| sub-27 (M104p2) ファイルと Hunk の適用、復元ジャーナル、巻き戻し、起動時の回復 | 実装中 | 0 | | 着手可 (sub-15 の VERDICT で詳細化)。初めて利用者のファイルを書くサブ。試験は一時プロジェクトだけ |
| sub-16 (M104q) ライブのシーンの復元 | 未着手 | | | 骨子。Phase 5 の最後 |
| sub-17 (M104r) 削除エンティティのゴースト、Overlay | 未着手 | | | 骨子。着手前に Phase 6 へ進むかユーザー確認 |
| sub-18 (M104s) Before / After のプレビュー、左右の 3D 比較 | 未着手 | | | 骨子 |
| sub-19 (M104t) プレハブ伝播の表示、大きいシーンの計測 | 未着手 | | | 骨子。Phase 6 の最後 |
| sub-20 (M104u) エンジン配布物へのサービス同梱 | 未着手 | | | 骨子。依存 sub-19 (Phase 6 を止めたら planner が依存を付け替える) |

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|

## ユーザー判断
- Notion の質問表: https://app.notion.com/p/41802024a4d481ac9376f3ebae2ee055 (Q-1 セッションの基準、Q-2 worktree の Verified 条件、Q-3 Phase 6 へ進むか。すべて推奨で進行中)
- 質問の運用: 推奨で進めて Notion に後回し (2026-11-15、sub-05 の実装中にユーザーが就寝のため切り替え。それまでは「その場で聞く」)。着手前確認のあるサブ (sub-07 / 08 / 09 / 12 / 13 / 17) も planner の推奨で止めずに進め、質問表 (ID / 聞きたいこと / 推奨 / 逆を選ぶと / 回答) を Notion 活動記録に書く。ユーザーの手作業が要る確認 (本物の Claude Code 終了、本物のフック入力) は未確認として残す。削除と push はしない。
- 2026-11-14: 計画の未決事項 6 (進め方) は `/harness` に載せる。
- 2026-11-14: 計画の未決事項 1〜5 は今は確定させない。それぞれが止めるサブ (M104c / f / j / m / n) の着手時にユーザーへ確認する。
- 2026-11-14 (spec.md §7.1 の 5 件への回答):
  - 仕様 (受け入れ条件 A1〜A20、19 サブ) は確定して sub-01 へ進める。
  - 試作コードはコミットせず、結果の文書 (`docs\ai-change-tracking\prototype-results.md`) だけ残す (planner の裁定どおり)。
  - Phase 6 (sub-17〜19) は含め、sub-17 の着手前に続行を確認する (裁定どおり)。
  - Editor の非対話起動 (`--selftest*` / `--screenshot` / リプレイ検証) ではサービスへ接続も起動もしない (裁定どおり)。
  - **裁定と違う答え**: エンジン配布物 (`tools\make_dist.ps1`) に `mye_track.exe` を入れる作業を M104 に含める。planner へ補足として送り、仕様とサブを直させる。
- 2026-11-14 (sub-02 の着手前):
  - Blob の既定値は設計どおり (除外 `.env`、`.env.*`、`*.pem`、`*.key`、`*.pfx`、`*.p12`、`id_rsa*`、`credentials*.json`、上限 2 GiB、保存期間 90 日)。計画の未決事項 4 は決着。
  - `tools\track` の最低 Rust は 1.89 を明示して採る (planner の裁定どおり)。
  - `%TEMP%\mye_m104b\` の試作コードは消さずに残す。
- 2026-11-14 (sub-03 round 1 の VERDICT の 2 件、どちらも planner の裁定どおり):
  - ベースラインで追跡対象の全ファイルの内容を `.mye\ai_changes\blobs\` に複製する (このリポジトリで約 391 MiB)。
  - sub-21 (M104d2) を Phase 1 に足す。順序は sub-03 → sub-21 → sub-04。
- 2026-11-15 (sub-05 の着手前の 3 件、すべて planner の裁定どおり):
  - `tools\track` の検査 (cargo test と track_verify.bat) を CI (ci.yml) に入れる。計画の未決事項 2 は決着。
  - `mye_track.exe` は `tools\build_track.bat` が `bin\x64\Debug` と `bin\x64\Release` の両方へ置き、Editor は自分の exe の隣を探す。
  - 開発機に Rust 1.89 のツールチェーンは足さない。1.89 での実ビルドは未確認として文書に残す。
- 2026-11-15: `%TEMP%\mye_track_*` (試験が残した一時ディレクトリ 931 個) は消さずに残す (planner の裁定どおり)。
- 未確認 (sub-09 の着手前に一緒に伝える): このリポジトリで追跡を有効にすると最初のベースラインで `.mye\ai_changes\blobs\` が約 488 MB になる。
- 未確認 (sub-07 / sub-08 の着手時に聞く): 本物の Claude Code 終了時のサービス生存の手動確認、本物のフック入力の記録への協力。

## 申し送り (セッション跨ぎ)
- planner (2026-11-14、PLAN): AskUserQuestion が使えなかったので確定確認は未取得。spec.md §7.1 の 5 件を司会が聞く。
- planner: sub-01〜05 は詳細、sub-06〜19 は骨子。planner は sub-01 の VERDICT と、各 Phase の最後のサブ (sub-05 / 08 / 14 / 16) の VERDICT で後続のサブを見直す (spec.md §6.1)。骨子のサブは見直しが済んでから着手する。
- planner: 着手前にユーザー確認が要るサブは sub-02 / 05 / 09 / 12 / 13 / 17 (spec.md §6.2。各 sub の冒頭の節)。
- planner (2026-11-14、PLAN 補足): ユーザーの回答を受けて sub-20 (M104u、配布物への同梱) を sub-19 の後に足した。spec.md は確定。Phase 6 を sub-17 の前で止めた場合も sub-20 は行えるが、依存 (sub-19) の付け替えが要るので planner に戻すこと。
- planner (2026-11-14、sub-03 の VERDICT round 1): sub-21 (M104d2) を足した。順序は sub-03 → sub-21 → sub-04。`src\` を触らないサブは sub-01〜08 と sub-21。
- planner (2026-11-15、sub-05 の VERDICT): Phase 2 を見直した。sub-06 は詳細、sub-07 / 08 は骨子 + Phase 1 の結果。planner は sub-06 の VERDICT で sub-07 / 08 をもう一度見直す。未確認のまま残っているもの: CI での実走、Rust 1.89 での実ビルド、別ユーザーの接続拒否、本物のクライアントの Job の中での生存。
- planner (2026-11-15、sub-06 の VERDICT round 2): sub-22 (M104g2) を足した。順序は sub-06 → sub-22 → sub-07。`src\` を触らないサブは sub-01〜08、sub-21、sub-22。
- planner (2026-11-15、sub-08 の VERDICT): Phase 3 を見直した。sub-09 は詳細、sub-10〜13 は骨子 + 追記。planner は sub-09 の VERDICT で sub-10 を、以後は各サブの VERDICT で次のサブを詳細にする。sub-09 から `src\` を触るので、ビルドと `Editor.exe --selftest` が検証に入る。
- planner (2026-11-15、sub-09 の VERDICT): sub-23 (M104k2) を足した。順序は sub-10 → sub-23 → sub-11。sub-23 は骨子で、sub-10 の VERDICT で詳細にする。
- planner (2026-11-15、sub-11 の VERDICT): sub-24 (M104m2) を足した。Phase 3 の順序は sub-12 → sub-24 → sub-13、その後 sub-14。sub-24 は骨子で、sub-12 の VERDICT で詳細にする。
- planner (2026-11-15、sub-24 の VERDICT): sub-25 (M104m3) を足した。順序は sub-24 → sub-25 → sub-13 → sub-14。sub-25 が済むまで、Debug の `Editor.exe --selftest` は機械が重いと `changetracking` で落ちる (sub-24 までの検証で Debug が赤く見えても、まず機械の負荷を見ること)。
- planner (2026-11-15、sub-14 の VERDICT): sub-26 (M104o2) を足した。順序は sub-14 → sub-26 → sub-15。sub-26 が済むまで、`cargo test` (track) の全体は機械が重いと 1 件ずつ落ちることがある (落ちた試験が `tests\watch.rs` / `tests\pipe.rs:811` なら既知)。planner は sub-26 の VERDICT で Phase 5 (sub-15、16) を詳細にする。
- planner (2026-11-15、sub-26 の VERDICT): Phase 5 を sub-15 (判定とプレビュー。書かない) → sub-27 (適用・ジャーナル・回復) → sub-16 (シーンの復元) の 3 本にした。sub-27 は骨子で、sub-15 の VERDICT で詳細にする。復元の試験は必ず一時プロジェクトで行う。
- planner: `src\` を触らないサブ (sub-01〜08) は selftest / replay_verify を回さなくてよい。回していないことを SELF_EVAL に書かせる。
