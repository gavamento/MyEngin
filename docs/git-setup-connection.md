# エディタ内Gitセットアップ・接続

Source Controlの「接続・設定」から、現在のプロジェクトを初期化し、既存のGitHub HTTPSリポジトリへ接続する。Git本体とGit Credential Manager (GCM) が必要。gh、GitHub APIクライアント、エンジン独自のトークン保存は不要。

## 操作

1. 未初期化なら、絶対パスと初期ブランチ名（既定 `main`）を確認して「Gitを初期化」。親リポジトリ内には初期化しない。
2. 非公開リポジトリなど認証が必要な場合は「GitHubへログイン」。GCMのブラウザ認証を完了する。
3. 作者名・メール・必要に応じ保存済みGitHubアカウントを選び保存。設定はこのプロジェクトのローカルGit設定のみ。
4. `https://github.com/owner/repository[.git]` を入力。「接続確認して登録」で変更前後のoriginを確認して適用する。
5. 接続先に履歴があれば、Fetchして確認する。初回コミットやPushは別途内容を確認して実行する。

接続確認は読み取り権限の確認のみ。保存済みアカウントは有効なアクセス権の保証ではない。個別Push URLのあるoriginはこの画面で変更できない。既存SSHリモートは従来機能を使う。

推奨gitignore行は表示内容を確認して明示的に追記する。既存行は保持する。セットアップ自体はステージ・コミット・Pull・Pushを実行しない。

## 実装契約

Editor / collabプロトコルを2へ更新。DLLのC ABIは変更なし。操作は `setup_state`, `repo_init`, `identity_save`, `github_login`, `remote_connect`, `setup_cancel`。

セットアップは通常の未初期化ゲートと分離し、再生中・ビルド中・ネットセッション中・競合操作中・別操作実行中はUIから実行不可。操作ロックはworkerの応答まで保持する。ログインの期限は300秒、接続確認は30秒。キャンセルはworker FIFOを経由しない世代番号による通知。Windows Job Objectで子プロセスを含めて終了し、出力読み取りスレッドをjoinしてから応答する。

GCMログイン出力はエディタへ返さない。GitHubアカウント選択時にはリポジトリローカルのGitHub向けcredential username/helperを設定する。グローバルGit設定は変更しない。

## 検証記録（2026-10-01）

- Rust: 全87件成功（Windowsのcore.autocrlfを実行プロセス内でfalseに固定）。追加4件はURL検証、空HEAD・Unicodeパス・作者設定復元・親リポジトリ拒否、期限切れ、実行中キャンセルを検証。
- Debug Editorビルド成功。静的ルール検証は0 error / 0 warning。Editor全SelfTestは最終ビルドでも終了コード0。
- 実CLIで公開GCMリポジトリへの接続・origin登録・同じURLへの再接続・既存origin変更成功。実Fetchも成功し、取得後もローカルHEADは空・作業ツリーに変更なし。GCM利用可否・保存済みアカウントの形式確認成功。
- collab既存回帰: 通常権限では7/9成功。08_conflict / 09_merge_abortは既存応答のoperationフィールドが既存の未コミット期待値にない差。今回、その応答経路・期待値ファイルは変更していない。
- ブラウザでの実ログイン、GUI実操作・画面確認、Pushは未検証。機械テストをこれらの証明として扱わない。

ログは `cache/git_setup_connection_full_access.log`, `cache/git_setup_collab_verify_full_access.log`, `cache/git_setup_build.log`, `cache/git_setup_fetch.log`, `cache/git_setup_selftest_final.log`。検証用一時プロジェクトはローカルに残し、削除していない。CI設定と既存のcollab期待値の未コミット変更は保持。
