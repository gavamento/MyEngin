# Git右クリック操作・状態アイコン 実装・検証記録

更新: 2026-10-01。元計画: [implementation_plan.md](implementation_plan.md)。
この文書では実装・機械的検証・実画面確認を区別する。

## 実装

- ソース管理とアセットブラウザーにGitメニューを追加。既存のステージ・解除・破棄ボタンも共通要求へ接続。
- 右クリック時のパス・コミット・ブランチを保持し、Gitサービスのプレビューを確認後に単回tokenで実行。
- index/statusによる対象列挙、既存.meta/.terrain.editの関連付け、追跡解除、ignore追記と部分失敗表示。
- `.gitignore`は確認開始から実行直前までの**ディスク上の変更検出**。未保存内容の検出ではない。
  内容・存在・更新日時・サイズを照合する。既存バイト列・BOM・改行を保持し、否定ルールと重複を検査する。
- 履歴のコピー・第1親差分・ブランチ作成・soft/mixed reset・コミットrevert・cherry-pick。
- ローカルブランチ作成・名前変更・非強制削除。現在ブランチの削除、未マージ、別worktree使用中を保護。
- revert/cherry-pick/sequencer状態の検出と、継続・中止・再取得。失敗時の変更集合も再読み込み処理へ渡す。
- 性能比較Gitlineは独立セッションを使い、そのGitlineのリポジトリへ差分要求を送る。
- Font Awesome 6の状態アイコンを共通化し、既存のテーマ色と状態文字を維持。追加UIを日英翻訳。
- EngineAPIのABIとGitサービスのC ABI・プロトコル版は変更していない。

## 動作上の注意

- stageは既存のアセットID維持のため、不足.metaをプレビュー前に生成する。取消し時も.metaは保持する。
  追跡解除では.metaを生成しない。確認取消しでindexや.gitignoreは変更しない。
- 非UTF-8パス、不正なルート相対パス、シンボリックリンク等は拒否する。
- 任意の外部Gitプロセスと完全に原子的な排他を保証するものではない。確認後の差分を検出して停止し、
  Git自身のindex/refロックと失敗応答を併用する。
- 現在の対象列挙はindex全体を読み取るため、大規模リポジトリでの実測は未実施。

## 確認済み

- Debug/Release x64ビルドが成功。
- Rust releaseビルドが成功。DLL/CLIをDebug・Releaseの出力先へ配置。
- 追加実Gitテスト17件成功（削除を伴う1件を除外）。
  追跡解除の内容保持、同じstatusでの内容変更、ignoreの外部変更・BOM/CRLF/Unicode/特殊文字、
  同じ内容の書換え検出、否定ルール・重複・取消し、root差分、soft/mixed、revert/cherry-pick、競合の継続・中止・再取得、
  フォルダ関連ファイル、現在ブランチの名前変更、確認後の先端移動を含む。
- Rustパーサー24件、監視フィルター5件が成功。
- 静的ルール検査: 0 errors / 0 warnings。git diff --checkも成功。

ログ: `cache/git-context-debug-build.log`、`cache/git-context-release-build.log`、
`cache/git-context-rust-release.log`、`cache/git-context-actions.log`、
`cache/git-context-parser-watch.log`、`cache/git-context-rules.log`。
fixtureは`cache/git-context-tests/`に残している。実際のプロジェクト履歴は検証操作に使用していない。

## 未実施・許可待ち

- 全Rustテスト・collab_verify・Editor --selftest・replay_verifyは、既存テストがfixtureやキャッシュを削除する。
  AGENTS.md第1章の明示許可待ち。削除なしの追加テスト／パーサー／監視フィルターのみ実行した。
- 追加テストのブランチ削除、破棄による未追跡削除、マルチバッチ途中失敗、ignore追記の途中I/O失敗、
  複数コミットsequencer、別worktree使用中の全組合せは未検証。
- Editor実画面の右クリック、日英、テーマ、狭幅、ドックリサイズ、Gitlineドラッグは未検証。
  このセッションにはWindowsネイティブ操作のnode_replが公開されていない。画面キャプチャは未取得。
- C++回帰テストは追加したが、SelfTestの実行完了は未確認。
- 既存CLI期待値の更新は追加フィールドのみ。全シナリオの再照合は未実施。
- コミット・push・実プロジェクトでの破壊的Git操作は行っていない。

## 削除許可を求めた範囲

用途は検証専用fixture・再生成可能なクックキャッシュ・過去の検証ログ。
理由は隔離テストとコールド検証の初期化。影響は以下の検証生成物に限定する。

- `C:/HAL/MyEngin/cache/git-context-tests/`配下の検証生成物
- `C:/HAL/MyEngin/cache/collab_verify/`配下の検証生成物
- `C:/HAL/MyEngin/bin/x64/Debug/cache/cooked/`
- `C:/HAL/MyEngin/bin/x64/Release/cache/cooked/`
- `C:/HAL/MyEngin/cache/replay_logs/`
- `C:/HAL/MyEngin/cache/*.mismatch.txt`
