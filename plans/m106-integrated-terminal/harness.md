# harness 台帳: m106-integrated-terminal

- 依頼原文: このエンジンでターミナルが開けるようにしたい
  - 追加指示: 「ConPTY+ImGui描画 は？」「vimやCLIなどにも対応させたい」「vimは対応対象外でいいけどclaudecodeCLIなどは対象に入れて」「git・ビルド・Claude Code CLIなども使えるMyEngin統合ターミナルにするなら」
- 開始: 2026-11-19 / 基点コミット: 9e44a4bb69201beca557be445dae13290658f324 / ブランチ: claude/nice-goodall-kwzmxi
- フェーズ: 策定

## サブ進捗
| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|

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

## 申し送り (セッション跨ぎ)
- 実行環境は Linux のクラウドコンテナ。MSVC ビルド・Editor.exe --selftest・check_rules.ps1・ConPTY・実画面確認はできない (Windows 側で要確認)。
- ハーネス参照資料: Notion「harness」 https://app.notion.com/p/harness-41c02024a4d48019b57ec311190638f9
