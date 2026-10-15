# harness 台帳: m87-mcp-server

- 依頼原文: M87の実装をしたい
  (方針確認への回答) 1.全部ハーネスで 2.すべて 3.87bで決める
- 開始: 2026-10-15 / 基点コミット: 32d1e1e206dde87d6e21a0d9f10516397bb0b903
- フェーズ: 完了

## ユーザー判断
- 2026-10-15: M87a〜d をすべてハーネスで進める
- 2026-10-15: verify_job の対象は replay_verify.bat の現行 `:job_*` 全 23 個 (司会の推奨は「bat のラベルから enum を作り、件数を check_rules で見張る」)
- 2026-10-15: 日英同義語表の粒度は M87b で決める
- 2026-10-15 (planner の [ユーザーに聞ける] への回答): verify_job のジョブ数は 23 ではなく 21 (司会の数え違い、事実訂正)。フィールド差分は v1 では返さない (裁定どおり)。MCP は legacy + modern の両方に対応 (裁定どおり)。EngineIndexDumpSelfTest は --selftest に入れる (裁定どおり)。**`.mcp.json` はコミットする (裁定と逆)**。接続確認は coder が一時 --mcp-config で試す (裁定どおり、報告のみ)

## サブ進捗
| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|
| 1 | PASS | 4/4/4/4 | minor 7 件 (review-1.md) |
| sub-01 | OK | 1 | 54f22f1 | --dump-index / gen_engine_index.ps1 / 規則 15-a/b/f |
| sub-02 | OK | 0 | 75d3004 | crate 骨格 dual-era / search / describe / mcp_verify / .mcp.json |
| sub-03 | OK | 0 | bc8f5d4 | verify_job |
| sub-04 | OK | 0 | de38970 | notes / パストラバーサル拒否 / ADR-032 / docs |

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|

## 申し送り (セッション跨ぎ)
- 作業ツリーの未追跡 `docs/deepmodal/training-cleanup-20261015.md` は M87 と無関係。コミットに含めない
- 既存の check_rules エラー 3 件 (EditorMain の `#ifdef _DEBUG`、engine_dist の .bat が LF) は M87 と無関係
- (planner 2026-10-15) verify_job の対象は実ファイルで数え直して **21 個** (依頼の 23 は数え違い)。spec.md 2. に記録
- (planner 2026-10-15) MCP の最新仕様は 2026-07-28 (initialize 廃止の modern 方式)。dual-era で実装する裁定。sub-02 の条件 15 で実クライアント接続を確かめる
- (planner 2026-10-15) AskUserQuestion なしで裁定した論点は PLAN_RESULT の未決事項 `[ユーザーに聞ける]` を参照
- (review round 1 PASS、minor 7 件 = review-1.md) 2026-10-15 にユーザー指示「直して」で司会が直接修正 (harness 不使用、M87 追補コミット)。7 件とも対応済み:
  1. [coder] docs	est_checklists.md:722-723 の `.mye
otes` が `
` で改行に化けている
  2. [coder] 規則 15-d が `File::options()` を見逃す (Get-McpWritePattern に `File\s*::\s*options` を足す)
  3. [coder] Cargo.toml のコメントは panic で落ちないと読めるが catch_unwind が無い (main.rs で包むかコメントを直す)
  4. [coder/planner] note_read の 64 KB 上限が生バイトで、応答は JSON エスケープ + 2 重載せで大きく超える
  5. [coder] docs\mcp-server.md の「Debug の Editor.exe が要る」は誤り (Release 優先、無ければ Debug)
  6. [planner] verify_job の重複拒否はプロセス内だけ。MCP 再起動直後・複数セッションの同名ジョブが通る (docs に追記するかロック)
  7. [planner] spec.md:234 に生の CR が混入
- 未検証 (test_checklists の M87 節で手動確認): 実クライアントからの note_* / verify_job、legacy での実クライアント接続、`claude mcp add --scope local` と project スコープの優先順位、実端末ウィンドウでの chcp 不変
- minor の修正で決めたこと: #4 は上限を JSON 文字列に直した長さで数える (同じ再現で応答 1 行 393,403 B → 196,842 B)。#6 はロックを入れず docs の注意に追記 (v1 は許容)。#3 の catch_unwind は経路を実際に panic させての確認はしていない (ビルドと既存テストのみ)
