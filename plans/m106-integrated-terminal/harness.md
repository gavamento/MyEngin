# harness 台帳: m106-integrated-terminal

- 依頼原文: このエンジンでターミナルが開けるようにしたい
  - 追加指示: 「ConPTY+ImGui描画 は？」「vimやCLIなどにも対応させたい」「vimは対応対象外でいいけどclaudecodeCLIなどは対象に入れて」「git・ビルド・Claude Code CLIなども使えるMyEngin統合ターミナルにするなら」
- 開始: 2026-11-19 / 基点コミット: 9e44a4bb69201beca557be445dae13290658f324 / ブランチ: claude/nice-goodall-kwzmxi
- フェーズ: 実装

## サブ進捗
| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|
| sub-01 libvterm 同梱 | OK | 1 | 9fcd5a4 | MSVCビルド・上流sha256照合は未確認 (nit) |
| sub-02 Win32 非依存コア | OK | 1 | bcf3dbc | MSVC・terminalcore 実行は Windows で要確認 (nit) |
| sub-03 ConPTY ホストとセッション | OK | 1 | 25754ab | ConPTY 実経路は未実行 (should, Windows で確認) |
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
- (2026-11-19 ユーザー指示) 役定義の正本はユーザーがアップロードした harness-planner.md / harness-coder.md / harness-reviewer.md (/root/.claude/uploads/a4d9b156-b135-54e6-b68d-c08729d36d62/ 配下、一時領域)。coder 版に技術解説の手順は無いが、Notion ページと AGENTS.md 第8章に従い coder への指示で技術解説を求め続ける。
- (2026-11-19 ユーザー指示) coder は Sonnet で起動する (sub-04 以降。sub-03 は起動済みのため Opus のまま)。
- (planner PLAN) libvterm 0.3.3 は GitHub/launchpad が 403、Ubuntu ミラー http://archive.ubuntu.com/ubuntu/pool/universe/libv/libvterm/libvterm_0.3.3.orig.tar.gz から取得可 (sha256 09156f43…7226e0)。展開物: /tmp/claude-0/-home-user-MyEngin/a4d9b156-b135-54e6-b68d-c08729d36d62/scratchpad/libvterm-probe/libvterm-0.3.3/ 。gcc -std=c99 -Wall 警告0、スモーク (1049/2004/truecolor/全角/OSC8) 確認済み。DECSET 2026 は未対応 (無視)。
- (sub-01) MSVC で libvterm が通らなければ最小パッチ + external/libvterm/PATCHES.md、VERSIONS.md の「改変なし」を直す。include は <vterm.h>、Editor のみ参照。sb_pushline は全画面スクロール時のみ。
- (sub-02) 貼付は単独LFもCRへ正規化 / 自動検出時 -NoLogo なし / 環境変数名の非ASCII並び順の制約を sub-04 で engine_spec §9.4 に1行 / スクロールバック上限のクランプは sub-04 設定側 / Feed は1フレーム256KB、TakeOutput はキー入力がなくても毎フレーム送る
- (sub-03) 窓は表示状態に関係なく毎フレーム Pump() / OnShutdown でセッション破棄 / finder は FindTerminalExecutable / ErrorMessage は英語、Tr は窓側 / SendPaste は同期書込 / Job 失敗時の制約を engine_spec §9.4 に1行 / Windows 確認項目は sub-04 で test_checklists へ
- (planner PLAN) cargo は /root/.cargo/bin/cargo にある。g++ あり。spec は「策定中」、未決は PLAN_RESULT の [ユーザーに聞ける] を参照。
