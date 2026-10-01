# harness 台帳: m81-dedicated-server

- 依頼原文: M81 汎用 Dedicated Server + AWS GameLift 対応。ユーザーと合意済みの設計案は C:\HAL\MyEngin\plans\m81-dedicated-server\design-draft.md (slug: m81-dedicated-server)。planner はこれを起点に仕様を確定し、サブへ分割すること。決定論を壊さないこと (design-draft §3) が全サブ共通の必須条件。
  - 元のユーザー発言: 「汎用Dedicated Server機能を作り、そのホスティング先の一つとしてAWS GameLiftを対応させたい」「決定論を壊さないように開発/ネットワーク管理を行うこと」
- 開始: 2026-10-01 / 基点コミット: 4e67907b5548b1e2ca48585d50519d6c1b895530
- フェーズ: 実装

## ユーザー判断 (プランモードでの合意。design-draft.md に詳細)
- 同期方式 = 入力確定型サーバ / サーバ OS = Windows 先行 / GameLift = Server SDK 組込 + Anywhere 疎通まで / 最大 4 人
- 遅延入力 = 期限で確定 (前 tick の入力を繰り返す) / 再接続と途中参加まで扱う / ゲーム API = 読取 + 参加離脱の通知 (ABI bump)
- スナップショットは構造変更 Commit 後 (ApplyStructuralChanges 直後) に撮る。SnapshotMeta / SessionConfig / SimProvenance (engine/protocol/api/schema/game/content/initialSnapshotHash) を持たせ、.rep ヘッダにも記録
- ~~NetIsServer/NetIsClient を ABI に足し tick 中はエラー + 0~~ → D3 で「ABI に足さない」に変更 (2026-10-02 ユーザー判断)
- システムイベントにサーバ発行の単調増加 eventSeq、playerId はレーンと別

## サブ進捗
- (harness Phase 1 後, 2026-10-02) D3 NetIsServer/NetIsClient は ABI に足さない / D4 サーバ構成クライアントの kNetMaxSpeculation=12 (P2P は 8) / D5 contentHash は除外リスト方式 / D6 gameVersion = DLL ハッシュ + --allow-game-mismatch。いずれも planner 裁定どおり

| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|
| sub-01 ヘッドレス Server.exe + golden .rep 照合 | OK | 1 | c562782 | 9 シーン全部ヘッドレスで一致 |
| sub-02 Session 型・システム入力・.rep v9 | OK | 1 | 0cc7504 | v8 .rep 読込可、規則 13 追加 |
| sub-03 SimProvenance と NetIdentity 統合 | OK | 2 | (次コミット) | round2 で配布物 manifest の不一致 2 件を修正 |
| sub-04 プロトコルと 1 プロセス内検証 | 未着手 | 0 | | |
| sub-05 Server 実運用ループ・LocalHosting・server_verify | 未着手 | 0 | | |
| sub-06 ABI v23 | 未着手 | 0 | | sub-02 後なら並列可 |
| sub-07 GameLiftHosting + SDK 5.x | 未着手 | 0 | | |
| sub-08 Anywhere 実疎通 (ユーザー手動) | 未着手 | 0 | | |
| sub-09 NetWindow と文書 | 未着手 | 0 | | |

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|

## 申し送り (セッション跨ぎ)
- (planner 2026-10-01) spec.md 確定・sub-01〜09 作成。AskUserQuestion 不可のため D3/D4/D5/D6 を裁定し [ユーザーに聞ける] として PLAN_RESULT に返した。
- 着手順: sub-01 → 02 → 03 → 04 → 05 → 07 → 08 (ユーザー手動)。sub-06 は sub-02 後なら並列可、sub-09 は 05 と 06 の後。
- sub-01 の結果 (どのシーンがヘッドレスで通るか) で後続の前提が変わりうる。VERDICT で範囲を裁定する。
- (司会 2026-10-02) sub-01 OK (c562782)。既存の問題 (M81 範囲外): Source control self test 2 項目が基点から失敗 / /p:MyeWarnAsError=true が ProjectComputeRunnerSelfTest.cpp の C4127 で失敗。C3 はこの 2 項目のみ除外
- (司会 2026-10-02) sub-01 の作業物 (C:\HAL\_m81_sub01_base、cache\sub01_baseline\、cache\sub01_neg\、tmp\sub01_*.log) はユーザー許可を得て削除済み (復元不可、いずれも再生成可能な検証用)
- (sub-01 → 後続) HeadlessSim は複数インスタンス交互実行時に tick 直前で Activate() が必要。確定入力の差し込み口は VerifyReplay の ctx.inputs[p] 代入。sub-04 では入力置換を EngineLoop と HeadlessSim で共通関数にする
- (司会 2026-10-02) sub-02 OK。planner 指摘: 規則 13-a の EngineApiTable.cpp 全面許可を NetRuntime.h の include だけに絞る (sub-03 やること 10)。再シム時の SystemInputTick 差し替え・TimeTravel の扱いは sub-04 要件へ。sub-03 は configBits を埋めても net_verify の --rep-diff が割れないこと (やること 9)
- (司会 2026-10-02) sub-03 OK (round 2)。未解明: 修正後の初回 `Editor.exe --package` が 1 回だけ exit 1 (再実行・DDS 版は PASS)。CI の package smoke か以後のサブで再現したら stdout/stderr を別ファイルに残して調べる
- (司会) assets\scenes\main.scene.json は git 未追跡。CI の package smoke での出どころは未確認 (sub-03 では CI の挙動は変わらないと planner 判断)
- (sub-03 → sub-04) サーバ構成の照合は NetIdentity を経由させず CompareProvenance + 必要項目の別 payload。Provenance は HeadlessSim::Provenance()。SessionConfig.deadlineTicks / rejoinTimeoutTicks は sub-04 で埋める
