# harness 台帳: m106-integrated-terminal

- 依頼原文: このエンジンでターミナルが開けるようにしたい
  - 追加指示: 「ConPTY+ImGui描画 は？」「vimやCLIなどにも対応させたい」「vimは対応対象外でいいけどclaudecodeCLIなどは対象に入れて」「git・ビルド・Claude Code CLIなども使えるMyEngin統合ターミナルにするなら」
- 開始: 2026-11-19 / 基点コミット: 9e44a4bb69201beca557be445dae13290658f324 / ブランチ: claude/nice-goodall-kwzmxi
- フェーズ: 実装

## サブ進捗
| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|
| sub-01 libvterm 同梱 | 未着手 | 0 | | |
| sub-02 Win32 非依存コア | 未着手 | 0 | | 依存 sub-01 |
| sub-03 ConPTY ホストとセッション | 未着手 | 0 | | 依存 sub-02 |
| sub-04 Terminal 窓 | 未着手 | 0 | | 依存 sub-03 |
| sub-05 VS 開発者環境とエンジン環境変数 | 未着手 | 0 | | 依存 sub-04 |
| sub-06 AI 変更追跡の紐付け (track v4) | 未着手 | 0 | | 依存 sub-02 (窓表示は sub-04 後) |

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|

## ユーザー判断
- 質問の運用: その場で聞く
- 方式: ConPTY + ImGui自前描画の埋め込みパネル
- 既定シェル: 自動検出で PowerShell 優先 (pwsh → powershell → cmd)、設定で上書き可
- 対象: PowerShell / git / ビルド出力 / Claude Code CLI などの対話型 CLI。vim など全画面エディタは対象外
- セッション: まず 1 パネル 1 シェル (タブは対象外。妨げない構造)
- 連携: AI 変更追跡との紐付け / VS 開発者環境の読み込み / エンジン環境変数と tools/ の PATH。git 実行後の SCM 窓更新は対象外
- (2026-11-19 planner 裁定をユーザー承認) AI 追跡: sessions.host_terminal 列追加 (スキーマ v4)、source は "terminal" のまま / VS 環境: 読み込み完了を待って起動、30 秒タイムアウトで素の環境 / 既定レイアウト: 下段 Console のドックにタブ / IME: 確定文字のみ送信
- (2026-11-19) 仕様確定: 受け入れ条件 1〜11 で実装へ

## 申し送り (セッション跨ぎ)
- 実行環境は Linux のクラウドコンテナ。MSVC ビルド・Editor.exe --selftest・check_rules.ps1・ConPTY・実画面確認はできない (Windows 側で要確認)。
- ハーネス参照資料: Notion「harness」 https://app.notion.com/p/harness-41c02024a4d48019b57ec311190638f9
- (planner PLAN) libvterm 0.3.3 は GitHub/launchpad が 403、Ubuntu ミラー http://archive.ubuntu.com/ubuntu/pool/universe/libv/libvterm/libvterm_0.3.3.orig.tar.gz から取得可 (sha256 09156f43…7226e0)。展開物: /tmp/claude-0/-home-user-MyEngin/a4d9b156-b135-54e6-b68d-c08729d36d62/scratchpad/libvterm-probe/libvterm-0.3.3/ 。gcc -std=c99 -Wall 警告0、スモーク (1049/2004/truecolor/全角/OSC8) 確認済み。DECSET 2026 は未対応 (無視)。
- (planner PLAN) cargo は /root/.cargo/bin/cargo にある。g++ あり。spec は「策定中」、未決は PLAN_RESULT の [ユーザーに聞ける] を参照。
