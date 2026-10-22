# ADR-032: AI クライアント向けの MCP サーバをエンジンの外側の Rust ツールとして置く

- 状態: 採用 (2026-10-15、M87)
- 出所: 計画書 [plans\エンジンMCPサーバ.md](../../plans/エンジンMCPサーバ.md) (設計の正本。§3 の決定と §9 の却下案はそちらを見る)。
  仕様と受け入れ条件は `plans\m87-mcp-server\spec.md`。使い方は [docs\mcp-server.md](../mcp-server.md)

## 背景

AI クライアントがエンジンで作業するとき、(1) `replay_verify.bat` の 150 KB 前後のログから割れた箇所を拾う、
(2) `engine_spec.md` (385 KB) を grep せずに component / field / CLI / ABI / ADR / golden を引く、
(3) プロジェクト固有の現在地をセッションをまたいで残す、の 3 つが高くつく。bat を包むだけの道具はシェルで足りるので価値が無い。

## 決定

1. **エンジンの外側の独立ツールにする。** `tools\mcp\` の Rust crate (`mye_mcp`、stdio の JSON-RPC 2.0)。依存は serde / serde_json だけ。
   MCP の SDK は使わない (プロトコルの面が小さく、依存が増えるほど cargo build が通らない同僚が増える)。
   MCP の legacy (2025-03-26〜2025-11-25) と modern (2026-07-28) の両方に答える。
2. **エンジン (sim / Runtime / GameLogic / Shared / Server) から索引ダンプ (`Editor/Tools/EngineIndexDump`) を include しない。**
   索引ダンプは Editor 層の機能で、`Editor.exe --dump-index` が実行時の登録表から出す。規則 15-a / 15-b。
3. **道具は 5 つ**: `search` / `describe` (索引。読み取り専用)、`verify_job` (固定 22 ジョブの起動と結果の構造化)、
   `note_read` / `note_append` (追記専用のメモ)。
4. **外へ手を伸ばす経路をファイル単位で閉じ込める** (`tools\check_rules.ps1` の規則 15-c / 15-d)。
   子プロセスを起動してよいのは `verify.rs` だけ。ファイルを書く API を使ってよいのは (ファイル x API) の組で許可された所だけ:
   `verify.rs` は `create_dir_all`、`notes.rs` は `create_dir_all` と `OpenOptions` (追記モード)。
   job 名は enum で受け、シェルに任意の文字列を渡さない。
5. **notes は追記専用で、書き先は `<project>\.mye\notes` (`--project`) か `<repo>\.mye\notes`。** 上書き・削除・改名の手段を持たない
   (自前ループの LLM が無条件に叩く前提で、不可逆な操作を持たせない)。topic は `[A-Za-z0-9_-]{1,64}` と Windows の予約デバイス名の拒否、
   書き先のシンボリックリンク・ジャンクションの拒否、正規化後のパスの検査で、notes の外に書けない。
   git には載せない (`.gitignore` の `.mye/` と、notes ディレクトリ内の `.gitignore` = `*`)。
6. **CI と `Editor.exe --selftest` に MCP を入れない。** 検証は `tools\mcp_verify.bat` を MCP を触ったときに手で回す。
   MCP が壊れても sim と CI は影響を受けない (障害の局所化)。

## 記録場所の住み分け

記録場所は 3 つあった (`docs\adr\` / `plans\` / Claude Code のメモリ)。notes は 4 つ目で、
先に役割を分けておかないと「どれにも書かれない」か「全部に重複」になる。

| 場所 | 書くもの | 読む主体 |
|---|---|---|
| `docs\adr\` | 覆らない設計判断 | 人 + AI |
| `plans\` | これからやること | 人 + AI |
| Claude Code のメモリ | 作業のクセ・踏んだ罠 | Claude Code のみ |
| `.mye\notes\` | プロジェクト固有の実測値・現在地 (コミットできない / したくない状態) | 全 LLM |

notes に残す価値があるものは、人が `docs\` へ昇格させる。AI が書いたものがレビューなしにリポジトリへ溜まるのを防ぐ 2 段構え。

## 却下した案

- **notes を git で追跡する**: レビューされていない記述がコミットに混ざる。
- **notes の上書き・削除を許す**: 自前ループの LLM が過去の記録を壊せる。訂正は次のエントリで行う。
- **MCP をエディタ (C++) に内蔵する**: エディタを起動しないと使えず、MCP の不具合がエディタに波及する。Rust の別 exe なら cargo だけでビルドと検証が閉じる。
- **MCP の SDK (rmcp) を使う**: 上の決定 1。

## 影響

- ABI、snapshot、TypeId、sim の挙動に変更なし。
- 新しい `check_rules` の規則 15-a〜15-f。
- 運用上の注意 (`verify_job` の子は MCP 終了後も走り続ける、全体実行の `replay_verify.bat` と同時に叩かない、など) は
  [docs\mcp-server.md](../mcp-server.md)。
