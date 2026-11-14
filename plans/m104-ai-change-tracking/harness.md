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
| sub-04 (M104e) 行 diff、Hunk、Revision、リネーム | 実装中 | 0 | | |
| sub-05 (M104f) pipe サーバ、hello、取得の op、track_verify.bat | 未着手 | | | 着手前にユーザー確認 (未決事項 2、exe の置き場所)。Phase 1 の最後 |
| sub-06 (M104g) セッション、操作、帰属 | 未着手 | | | 骨子。planner の見直し後に着手 |
| sub-07 (M104h) MCP の 5 ツール | 未着手 | | | 骨子 |
| sub-08 (M104i) フック CLI | 未着手 | | | 骨子。Phase 2 の最後 |
| sub-09 (M104j) Editor の pipe クライアント、モデル、規則 19 | 未着手 | | | 骨子。着手前にユーザー確認 (未決事項 3) |
| sub-10 (M104k) AI Changes 窓、Diff 表示部品 | 未着手 | | | 骨子。分割の可能性あり |
| sub-11 (M104l) シーン構造 diff | 未着手 | | | 骨子 |
| sub-12 (M104m) Hierarchy / Inspector / Scene View の表示 | 未着手 | | | 骨子。着手前にユーザー確認 (未決事項 5) |
| sub-13 (M104n) 未保存シーンへの外部変更の確認 | 未着手 | | | 骨子。依存 sub-08。着手前にユーザー確認 (未決事項 1) |
| sub-14 (M104o) レビュー | 未着手 | | | 骨子。依存 sub-12, sub-13。Phase 4 の最後 |
| sub-15 (M104p) ファイルと Hunk の復元、ジャーナル、回復 | 未着手 | | | 骨子。分割の可能性あり |
| sub-16 (M104q) ライブのシーンの復元 | 未着手 | | | 骨子。Phase 5 の最後 |
| sub-17 (M104r) 削除エンティティのゴースト、Overlay | 未着手 | | | 骨子。着手前に Phase 6 へ進むかユーザー確認 |
| sub-18 (M104s) Before / After のプレビュー、左右の 3D 比較 | 未着手 | | | 骨子 |
| sub-19 (M104t) プレハブ伝播の表示、大きいシーンの計測 | 未着手 | | | 骨子。Phase 6 の最後 |
| sub-20 (M104u) エンジン配布物へのサービス同梱 | 未着手 | | | 骨子。依存 sub-19 (Phase 6 を止めたら planner が依存を付け替える) |

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|

## ユーザー判断
- 質問の運用: その場で聞く (2026-11-14)
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
- 未確認 (sub-05 の着手前に聞く): 開発機に Rust 1.89 のツールチェーンを入れて最低版でのビルドを確かめるか (planner の裁定は「確かめない」)。
- 2026-11-15: `%TEMP%\mye_track_*` (試験が残した一時ディレクトリ 931 個) は消さずに残す (planner の裁定どおり)。
- 未確認 (sub-09 の着手前に一緒に伝える): このリポジトリで追跡を有効にすると最初のベースラインで `.mye\ai_changes\blobs\` が約 488 MB になる。
- 未確認 (sub-07 / sub-08 の着手時に聞く): 本物の Claude Code 終了時のサービス生存の手動確認、本物のフック入力の記録への協力。

## 申し送り (セッション跨ぎ)
- planner (2026-11-14、PLAN): AskUserQuestion が使えなかったので確定確認は未取得。spec.md §7.1 の 5 件を司会が聞く。
- planner: sub-01〜05 は詳細、sub-06〜19 は骨子。planner は sub-01 の VERDICT と、各 Phase の最後のサブ (sub-05 / 08 / 14 / 16) の VERDICT で後続のサブを見直す (spec.md §6.1)。骨子のサブは見直しが済んでから着手する。
- planner: 着手前にユーザー確認が要るサブは sub-02 / 05 / 09 / 12 / 13 / 17 (spec.md §6.2。各 sub の冒頭の節)。
- planner (2026-11-14、PLAN 補足): ユーザーの回答を受けて sub-20 (M104u、配布物への同梱) を sub-19 の後に足した。spec.md は確定。Phase 6 を sub-17 の前で止めた場合も sub-20 は行えるが、依存 (sub-19) の付け替えが要るので planner に戻すこと。
- planner (2026-11-14、sub-03 の VERDICT round 1): sub-21 (M104d2) を足した。順序は sub-03 → sub-21 → sub-04。`src\` を触らないサブは sub-01〜08 と sub-21。
- planner: `src\` を触らないサブ (sub-01〜08) は selftest / replay_verify を回さなくてよい。回していないことを SELF_EVAL に書かせる。
