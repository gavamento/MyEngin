# sub-04: GameLift 窓: 共通部と Anywhere の手順

- 依存: sub-01 (`--stop-event` / 行単位ログ)、sub-03 (CLI 層)
- 状態: 未着手
- 往復: 0

## やること

spec 4.1.4 (共通) と 4.1.5 (Anywhere)、4.3 のうち Anywhere タブと確認ダイアログ。

1. **窓** `GameLiftWindow` を Window メニューの Network の隣に足す (`EditorApp.cpp:1977` 付近)。既定サイズ 640x560。
   上段の状態 (CLI の場所と版 / アカウント / リージョン / 再確認 / aws login を開く)、タブ (Anywhere / EC2 = EC2 は「sub-05 で実装」の空タブでよい)、
   下段 (実行中の CLI と取消、直近のエラー)。
2. **手順の状態機械** (UI から切り離した型。偽 CLI を差して自己テストで回せること):
   準備 / サーバ起動 / セッションを作って参加 / サーバ停止 / 片付け。各段の CLI の順序・冪等性・再試行・待ちの上限は spec 4.1.5 の表のとおり。
   1 窓で同時に走る CLI は 1 本。
3. **確認ダイアログ** (モーダル、既定フォーカスは「やめる」): 準備 (作成)・片付け (削除)。文言に資源名とリージョン。
4. **Server.exe の起動と監視**: トークンは `get-compute-auth-token` (秘密フラグ) で取り、子の環境だけへ。名前付き stop イベントを作って
   `--stop-event` で渡す。kill-on-close のジョブに入れる (`HostServerProcess.cpp:85` と同じ考え方)。stdout/stderr は
   `<project>\cache\gamelift\server-<日時>.log` へ (sub-01 で行単位に届く)。窓はログを読んで `ProcessReady sent` で「待機中」、
   プロセス終了で exit code とログ末尾 8 行を出す。停止 = イベント → 10 秒待つ → 終わらなければ TerminateProcess + 警告。
   エディタ終了時も同じ停止手順。
5. **クライアントの起動**: `<exeDir>\Runtime.exe --project <proj> --net-connect <ip>:<port> --player-session-id <id>`
   (player session を作った同じ流れで直ちに。作業ディレクトリ = プロジェクト)。
6. **設定の保存**: `<project>\.mye\editor_settings.json` の `gameLift` (D17)。トークンは書かない。
7. **aws login を開く**: 新しいコンソール窓で `aws login` (対話はユーザーがそちら)。完了待ちはしない。
8. **UI 文字列**: `LocalizationTable.inl` に両言語。識別子の接頭辞は既存と衝突しない名前 (例 `GameLift_`)。
9. 自己テスト (偽 CLI = プロセス内の差し替え。必要なら `cmd.exe`) と、一時プローブ + `--screenshot` の撮影 (プローブはコミットしない)。

## やらないこと (このサブでは)

- EC2 タブの中身 (sub-05)、起動時の課金チェック (sub-05)
- Network 窓 (`NetWindow.cpp`) の変更
- 実 AWS への呼び出しを自己テストで行うこと

## 触る場所 (planner の見立て)

- 新規 `src/Editor/Windows/Project/GameLiftWindow.{h,cpp}`
- 新規 `src/Editor/Tools/GameLift/` に状態機械 (例 `GameLiftAnywhereFlow.{h,cpp}`)、Server 子プロセスの管理、設定の読み書き
- `src/Editor/App/EditorApp.{h,cpp}` (窓の所有・メニュー・終了時の停止)
- `src/Editor/App/EditorSettings.{h,cpp}` (`gameLift` の往復。既存キーを変えない)
- `src/Engine/Core/Localization/LocalizationTable.inl`
- `src/Editor/SelfTest/SelfTestSuites.cpp`
- 参考: `NetWindow.cpp` の `themeColor` の使い方、`BuildSettingsWindow` の非同期段の作り
- ★Editor 層から `Engine/Engine/Online/HostServerProcess` を流用しない (Engine 側の型で、ロビー用。考え方だけ参考にする)

## 受け入れ条件 (このサブ)

spec 5. の 13〜19。

1. 偽 CLI で CLI 無し / v1 / ログイン切れ / リージョン無し / プロジェクト無し / GameLogic.dll 無しのとき、該当ボタンが無効で理由が返る (状態機械の問い合わせで検査)。[E]
2. 「準備」: 何も無い → location → fleet → compute の順に 3 本の作成。全部ある → 作成 0 本。`custom-` は 1 回。`custom-x` の入力は拒否。
   確認で「やめる」→ CLI 0 本。保存 → 読み込みで ID が戻り、`editor_settings.json` の中身に token の文字列が無い。[E]
3. 「サーバ起動」: Server.exe の argv が spec どおりでトークンを含まない。子の環境に `MYE_GAMELIFT_AUTH_TOKEN` が入る
   (Server の代わりに `cmd.exe` を差して確かめてよい)。ログの `ProcessReady sent` で待機中。「停止」で stop イベントが立つ。[E]
4. 「セッションを作って参加」: 容量不足を 2 回返す偽 CLI → 再試行 → ACTIVATING → ACTIVE → player session → クライアント起動の argv が spec どおり。[E]
5. 「片付け」: deregister → delete-fleet → (偽 CLI が 2 回「まだある」→ 消えた) → delete-location の順。[E]
6. スクショ: Anywhere タブ (準備済み・待機中の偽状態)、確認ダイアログ。日本語・英語。画像パスを SELF_EVAL に書く。[P]
7. `tools\check_rules.ps1` 0 error (翻訳の両言語・書式指定子・規則 22 を含む)。[R]

## 検証コマンド

- `tools\gen_project_files.ps1` → ビルド (Debug / Release)
- `MYE_SELFTEST_ONLY=<新スイート>` で `bin\x64\Debug\Editor.exe --selftest`
- `tools\check_rules.ps1`
- 一時プローブ + `bin\x64\Release\Editor.exe ... --screenshot ...` (窓を出す起動はリポジトリ外のプロジェクトで。imgui.ini を汚さない — メモリの M104 の注意)

## 実装メモ (coder が追記)

## フィードバック履歴
