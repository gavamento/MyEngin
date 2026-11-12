# MyEngin AI Change Tracking
## 技術仕様・実装計画書 v1.1

> この文書は 2026-11-13 に受け取った仕様書の原文をそのまま保存したもの (要求の正本)。
> 既存実装と照合した結果と、それを受けた設計は [architecture.md](architecture.md) 以下の設計文書と
> [ADR-039](../adr/ADR-039-ai-change-tracking.md) にある。原文と設計が食い違う箇所は設計文書側に差と理由を書いてある。

**対象エンジン：** MyEngin Deterministic Engine  
**対象環境：** Windows / C++ / DirectX11 / ImGui / Rust  
**対象リポジトリ：** `gavamento/MyEngin`  
**実装状態：** 新規提案・未実装  
**優先度：** P1（開発支援機能。安全性関連要件はP0）  
**設計方針：** Editor専用・既存機能再利用・決定論非干渉

---

# 1. プロジェクト概要

## 1.1 目的

MyEnginに接続したAIエージェントが行ったコード・シーン・アセットの変更を追跡し、開発者が変更箇所、変更元、変更内容を確認できる機能を実装する。

変更履歴を可視化するだけでなく、各変更に対する承認・却下・部分復元を提供する。

AIによる編集を開発者が監督できる状態を構築することを目的とする。

## 1.2 提供する主要機能

1. AIセッション別の変更履歴管理
2. コード変更箇所の差分表示
3. シーン内Entity・Component・Fieldの変更追跡
4. Before / After比較
5. ファイル・Hunk・Entity・Field単位の復元
6. AI変更の承認・却下フロー
7. 変更元の確実性表示
8. Editor再起動後の履歴復元
9. MCPおよびAI CLIとの連携
10. Git・UndoStack・SceneSerializerとの統合

## 1.3 設計原則

- 既存のMCP、Git統合、UndoStack、SceneSerializer、FileWatcher、Selectionを再利用する。
- AIの変更処理自体を原則ブロックせず、追跡処理を非同期化する。
- 変更の検出と変更者の帰属を分離する。
- 根拠が不十分な変更をAIによるものと断定しない。
- 承認とGit Commitは独立した操作とする。
- 復元前には必ず現在状態を確認する。
- 変更記録はRevision単位で保持する。
- 追跡用メタデータを決定論シミュレーションに混入させない。
- EditorやMCPが異常終了しても、保存済み履歴を保全する。
- Git Indexおよび既存の未コミット変更を勝手に変更しない。
- 取得できなかった変更履歴を推測で補完しない。

## 1.4 v1.1の実装範囲

初期実装では、次の対象を優先する。

- C / C++ / C# / HLSL
- JSON / YAML / Markdown / その他UTF-8テキスト
- Scene JSON
- Prefab / Actor JSON
- Assetメタデータ
- MyEngin Editor内のシーン編集
- MCPに接続したAI CLI
- 外部ファイル変更

バイナリファイルはファイル単位の変更記録と復元を対象とし、行差分は実装しない。

### 初期対象外

- ASTによるコード変更の意味解析
- AIによる自動承認
- MCPからの直接Restore
- 実行時Worldの状態履歴
- AIの推論過程の保存
- 異なるマシン間での履歴同期
- 完全な3D Side by Side比較

これらは将来拡張とする。

---

# 2. 既存MyEnginとの統合

## 2.1 既存実装

| サブシステム | 主な既存実装 | 利用用途 |
|---|---|---|
| MCP | `tools/mcp/` | AIとの通信 |
| Git | `tools/collab/` | Git状態・差分取得 |
| Source Control | `src/Editor/SourceControl/` | Git UIとの統合 |
| Undo | `src/Editor/Undo/` | Editor変更のUndo/Redo |
| SceneSerializer | `src/Engine/Engine/Scene/` | Scene JSON生成・適用 |
| FileWatcher | `src/Engine/Core/Util/` | ファイル変更検出 |
| Selection | `src/Editor/Scene/` | Entity選択 |
| Scene View | `src/Editor/Windows/Scene/` | 差分ハイライト描画 |
| GitTransaction | `src/Editor/SourceControl/` | 安全な書き込みゲート |

## 2.2 変更禁止・変更許容範囲

既存MCPの5ツールは互換性を維持する。

- `search`
- `describe`
- `verify_job`
- `note_read`
- `note_append`

既存のGit Commit・Stage・Unstage処理は変更せず、必要な範囲で共通サービスを利用する。

既存UndoStackの保存形式をAI専用に変更しない。

Engine層への変更は既存公開APIの再利用を原則とし、追加が必要な場合も、追跡メタデータをRuntimeやシミュレーション状態へ持ち込まない。

---

# 3. システムアーキテクチャ

## 3.1 全体構成

AI Change Trackingを独立したサービスとして実装する。

構成は次のとおりとする。

**AIクライアント層**

- Claude Code
- Codex CLI
- Gemini CLI
- Other MCP Client
- 将来実装するMyEngin統合ターミナル

**接続層**

- 既存 `mye_mcp.exe`
- AI CLIフック・操作イベント
- Windows Named PipeによるローカルIPC

**Change Tracking Service**

- Session Registry
- Operation Registry
- Change Collector
- Change Attribution
- Snapshot Manager
- Diff Engine
- Event Journal
- History Database

**Editor Integration**

- AI Change Manager
- AI Changes Window
- Code Diff Viewer
- Scene Diff Viewer
- Review Manager
- Restore Manager
- Scene Highlight Overlay

**Persistence**

- SQLite
- Content-addressed Blob Store
- Recovery Journal

## 3.2 サービスの責務

Change Tracking Serviceを履歴データの正本とする。

サービスは、変更イベントの受付、スナップショット保存、差分生成、帰属判定、履歴検索を担当する。

Editorは可視化、ユーザー確認、Scene状態の取得・適用、承認操作、復元操作を担当する。

MCPはAIとの通信インターフェースとして使用し、Editor内部状態への直接アクセスを持たない。

## 3.3 IPC仕様

初期通信方式としてWindows Named Pipeを採用する。

IPCには以下の情報を含める。

- Protocol Version
- Request ID
- Project ID
- Session ID
- Operation ID
- Command
- Payload
- Error Code

通信はローカル限定とする。

Named Pipeは現在のユーザーにアクセスを制限し、セッション認証情報を検証する。

クライアントが指定したプロジェクトパスだけを信用せず、サービス側で許可対象プロジェクトを照合する。

サービスが利用できない場合はEditorの通常操作を継続する。

## 3.4 サービスの起動方式

追跡サービスは、EditorまたはMCPから必要に応じて起動できる方式とする。

同一プロジェクトに対して、履歴DBへ書き込むサービスは原則1つとする。

排他制御を行い、二重起動時は既存サービスへ接続する。

サービスが停止している間の変更は、再起動後にディスク状態との差分から検出できる範囲で補完する。ただし、停止期間中の中間編集や変更者を保証しない。

---

# 4. AIセッション管理

## 4.1 Sessionモデル

各AIセッションにUUIDを割り当てる。

| フィールド | 型・内容 |
|---|---|
| `sessionId` | UUID |
| `provider` | Claude / Codex / Gemini / Other |
| `processId` | PID |
| `processStart` | プロセス開始識別情報 |
| `projectId` | 対象プロジェクト |
| `workspace` | 作業ディレクトリ |
| `baseCommit` | 開始時のGit HEAD |
| `startedAt` | 開始日時 |
| `endedAt` | 終了日時 |
| `status` | Running / Finished / Interrupted |
| `source` | Terminal / MCP / External |
| `trackingCoverage` | 追跡範囲・欠落情報 |

PIDの再利用を考慮し、PID単体でセッションを識別しない。

## 4.2 Operationモデル

セッション内の編集操作をOperation単位で管理する。

1回のOperationは複数のファイル変更を含められる。

記録項目：

- `operationId`
- `sessionId`
- `startedAt`
- `finishedAt`
- `operationType`
- `reportedTargets`
- `observedTargets`
- `result`
- `evidence`

これにより、同一AIが複数ファイルを変更した場合でも、一連の作業として表示できる。

## 4.3 変更元の帰属

帰属判定は以下の4段階とする。

| 信頼区分 | 意味 |
|---|---|
| Verified | 管理された編集経路などで変更元を検証済み |
| Claimed | MCP・CLIが変更元を申告した |
| Inferred | 観測時刻などから推定された |
| Unknown | 変更元を特定できない |

MCPから `change_report` を受信しただけではVerifiedとしない。

Before/Afterスナップショット、操作識別子、隔離された作業領域などを照合し、変更元を検証できる場合にVerifiedとする。

## 4.4 複数AIの同時実行

複数セッションは独立して管理する。

同一ファイルへ同時に変更が発生した場合は、観測できる変更範囲をOperationへ関連付ける。

帰属が確定できない場合はUnknownとして保存する。

複数の帰属候補を保持できるデータ構造とする。

---

# 5. 変更検出・スナップショット

## 5.1 検出方式

以下の変更源を監視する。

1. MCPから報告される操作
2. CLIフックや操作イベント
3. 専用ワークツリー内の変更
4. Editorの操作イベント
5. FileWatcherによる外部ファイル変更

## 5.2 スナップショット取得

管理された操作では、変更適用前にBeforeを取得する。

処理順序：

1. Operation開始
2. Beforeスナップショット保存
3. 編集処理
4. Afterスナップショット保存
5. 差分生成
6. Revision確定
7. 履歴DBへ登録

スナップショットには内容ハッシュと取得時刻を記録する。

## 5.3 非管理操作への対応

外部AIが直接ファイルを書き換える場合、変更前スナップショットを必ず取得できるとは限らない。

この場合、最後に確認できた状態をBeforeとして扱う。

変更前状態が存在しない場合は、完全な差分として扱わず、部分観測であることを記録する。

短時間の複数回編集で中間状態を取り逃した場合も同様とする。

## 5.4 Blob保存

ファイル内容をSHA-256などのハッシュで識別し、重複排除する。

- 同一内容は1つのBlobとして保存する。
- ファイル本来のバイト列を保持する。
- JSONの構造比較用データと原本Blobを分離する。
- 容量上限を設定可能にする。
- 古い履歴の削除時は参照関係を確認する。
- 復元に必要なBlobは保護する。

---

# 6. コード差分追跡

## 6.1 対応内容

- ファイルの追加
- ファイルの削除
- ファイルのリネーム
- 行の追加
- 行の削除
- 行の変更
- 同一ファイル内の複数Hunk

## 6.2 Diffモデル

各変更をFileChangeとして保存する。

FileChangeに以下を含める。

- File ID
- Path
- Old Path
- Before Hash
- After Hash
- Change Type
- Operation ID
- Revision ID
- Hunks

Hunkには以下を記録する。

- Old Start / Count
- New Start / Count
- Before Text
- After Text
- Context
- Hunk ID

## 6.3 既存Gitとの統合

既存Source ControlのDiff表示機能を再利用する。

ただし、Git HEADとの差分をAI変更の正本として使用しない。

比較基準は保存済みのBefore/Afterスナップショットとする。

既存Source Controlの256 KiBの表示上限は、復元用データには適用しない。

Git Indexは読み取り対象として扱い、AI Change Tracking自身が勝手にStage・Unstageしない。

## 6.4 Code Diff Viewer

表示機能：

- インラインDiff
- 左右分割Diff
- 追加・削除行の色分け
- Hunk単位の選択
- 変更元セッション表示
- 承認状態表示
- 復元操作
- 対象ファイル・行への移動

初期段階は読み取り専用とする。

---

# 7. Scene変更追跡

## 7.1 Entity識別

Scene内では既存の永続 `fileId` を使用する。

履歴上の識別キーは次の組み合わせとする。

- Project ID
- Document ID
- Entity fileId

`EntityID` は永続識別子として使用しない。

Document IDはシーンの移動・リネーム後も同一性を追跡できる識別子とする。

## 7.2 Scene Diffモデル

比較対象：

- Entity
- Name
- Parent
- Child Index
- Component
- Field
- Prefab Reference
- Prefab Override

変更種別：

| 種別 | 内容 |
|---|---|
| EntityAdded | Entity追加 |
| EntityRemoved | Entity削除 |
| EntityModified | Entity変更 |
| ComponentAdded | Component追加 |
| ComponentRemoved | Component削除 |
| FieldModified | Field変更 |
| HierarchyChanged | 親子関係・順序変更 |
| PrefabAssetChanged | Prefabアセット変更 |
| PrefabInstanceChanged | Prefabインスタンス変更 |
| PrefabOverrideChanged | Override変更 |
| PrefabPropagation | ベース変更による派生変更 |

## 7.3 Scene JSON比較

SceneSerializerが出力するJSONを構造的に比較する。

JSONオブジェクトのキー順序のみの違いを変更として扱わない。

一方、保存された数値の比較は厳密性を優先し、表示上の丸めによって変更を隠さない。

スキーマバージョンの変更がある場合は、比較可能性を検証し、未対応形式は競合または解析不可として扱う。

## 7.4 Prefabの取り扱い

Prefabアセットとシーン上のインスタンスを区別する。

- Prefabのベース変更
- Overrideの変更
- 構造Overrideの変更
- Instanceの参照変更
- ベース変更の伝播

これらを個別に記録する。

派生変更には原因となったChange IDを関連付ける。

## 7.5 保存前のScene状態

Sceneの状態を以下に分離する。

- Disk Snapshot
- Editor Snapshot
- Historical Snapshot

未保存のSceneがある状態で外部AIによる変更を検出しても、自動的にライブSceneへ上書き適用しない。

SceneSerializerの呼び出しはEditorの安全な実行フェーズで行う。

ワーカースレッドでは原則として取得済みJSONの解析・比較のみ実行する。

---

# 8. Scene View差分表示

## 8.1 表示仕様

| 変更種別 | 初期表示 |
|---|---|
| 追加 | 緑色アウトライン |
| 更新 | 青色アウトライン |
| 削除 | Beforeゴースト |
| 競合 | 赤色アウトライン |
| 未確認 | アイコン |

表示色はユーザー設定で変更可能とする。

## 8.2 Editorとの統合

既存のSceneViewWindow、Hierarchy、Selection、Inspectorを使用する。

変更一覧でEntityをクリックすると、対象を選択してHierarchyとInspectorを同期する。

削除済みEntityは、Beforeスナップショットを使った読み取り専用プレビューを提供する。

## 8.3 描画上の制約

変更ハイライトをWorldのComponentとして保持しない。

Editor専用の表示データとして管理する。

Scene Viewの表示切替によってシミュレーション状態が変化してはならない。

## 8.4 段階的実装

初期段階：

- Hierarchy変更バッジ
- Entityアウトライン
- Inspector内のField差分
- 変更一覧からの対象選択

拡張段階：

- 削除Entityのゴースト
- Scene Overlay
- Before / Afterの独立表示
- 3D Side by Side比較

---

# 9. Before / After比較

## 9.1 コード比較

比較基準：

- セッション開始時
- Operation開始直前
- 特定Revision
- 現在の作業ツリー

過去Revisionと現在状態を比較できるようにする。

## 9.2 シーン比較

以下の表示モードを提供する。

- Before
- After
- Overlay
- Side by Side

初期実装ではJSON差分とInspector比較を優先する。

## 9.3 3D比較

3D Side by Sideは独立したプレビュー用Sceneを使用する。

実行中のSceneを巻き戻して表示してはならない。

プレビュー用Sceneが参照するアセットについても、比較時点の状態を完全に再現できない場合はその旨を表示する。

---

# 10. Review・承認フロー

## 10.1 承認状態

各RevisionのReview状態を以下とする。

- Pending
- Approved
- Rejected
- NeedsReview

Reopenは過去のレビュー結果を消去する操作ではなく、レビュー状態を再開するイベントとして記録する。

## 10.2 承認単位

- Session
- Operation
- File
- Hunk
- Entity
- Component
- Field

承認は対象Revisionと対象範囲に紐付ける。

## 10.3 Revisionの独立性

Revisionが更新されても、過去の承認履歴を改変しない。

新しいRevisionの初期状態はPendingとする。

親単位の承認状態は子の集約結果から計算し、部分承認の場合はPartially Approvedと表示する。

## 10.4 承認の意味

Approvedは開発者が変更を確認した状態を表す。

以下の操作は含まない。

- Git Stage
- Git Commit
- Git Push
- 自動ビルド
- 自動テスト合格

Rejectedも変更の自動削除を意味しない。

変更を取り消す場合は明示的にRestoreを実行する。

## 10.5 復元状態との分離

Review状態とは別に、変更の現在状態を管理する。

例：

- Active
- Superseded
- Partially Restored
- Restored
- Conflicted

これにより、Approved済みRevisionを後から復元しても履歴上の矛盾が発生しない。

---

# 11. Restore Manager

## 11.1 復元対象

- ファイル全体
- コードHunk
- Entity
- Component
- Field
- Prefab Override

## 11.2 復元フロー

1. Restore要求
2. 対象Revision取得
3. 現在の作業状態を確認
4. GitTransaction相当の安全性ゲート評価
5. 対象の競合検証
6. 復元対象の依存関係確認
7. 復元前スナップショット保存
8. 復元プレビュー
9. ユーザー確認
10. 復元ジャーナル作成
11. 変更適用
12. 整合性検証
13. Commitまたは回復処理
14. 履歴更新

## 11.3 競合検証

コードの場合：

- 対象Hunkの文脈
- After側の変更内容
- 現在の対象範囲
- 対象ファイルの変更状態

これらを比較する。

一致しない場合は自動復元を行わない。

Sceneの場合：

- Document ID
- fileId
- Component
- Field
- 現在値
- After値

を照合する。

## 11.4 UndoStack連携

Scene変更は既存UndoStackの記録経路を使用する。

コードファイルや汎用アセットは専用の復元ジャーナルを利用する。

すべての復元をUndoStackだけで処理しない。

復元自体も履歴に記録し、可能な場合は再復元できるようにする。

## 11.5 トランザクション管理

復元操作の状態：

- Prepared
- Applying
- Committed
- RollingBack
- RecoveryRequired

複数ファイルの変更前内容を保存してから適用を開始する。

途中失敗時は保存した内容を用いてロールバックする。

異常終了後に未完了の復元操作を検出した場合は、起動時に回復処理を提示する。

複数ファイルとメモリ上のSceneに対する完全な原子性は前提とせず、障害後に整合状態へ復旧可能であることを保証目標とする。

## 11.6 Gitとの安全な共存

復元処理はGit Indexを直接変更しない。

既存のステージ済み変更と復元対象が重なる場合は、影響を明示し、必要に応じて復元を停止する。

既存GitTransactionのゲート判定・StageClassifier・ReloadHubとの共通化を検討する。

Playモード中やネットワークセッション中など、危険な状態では適用しない。

---

# 12. MCP拡張仕様

既存の5ツールを維持し、以下を追加する。

| MCP Tool | 内容 |
|---|---|
| `change_session_begin` | セッション登録 |
| `change_session_end` | セッション終了 |
| `change_list` | 変更履歴検索 |
| `change_describe` | 変更詳細取得 |
| `change_report` | 編集操作の報告 |

## 12.1 change_session_begin

主な引数：

- `provider`
- `workspace`
- `clientSessionId`（任意）

戻り値：

- `sessionId`
- `status`
- `trackingCapabilities`

## 12.2 change_session_end

主な引数：

- `sessionId`

戻り値：

- `status`
- `changeCount`

## 12.3 change_report

主な引数：

- `sessionId`
- `operationId`
- `paths`
- `beforeHashes`
- `afterHashes`

報告内容は観測結果と照合する。

AIが申告した内容だけで帰属を確定しない。

## 12.4 change_list

フィルター：

- Session
- File
- Date Range
- Review Status
- Attribution
- Change Type

履歴はページングして返し、大容量Blobを一覧応答へ含めない。

## 12.5 change_describe

特定のChange Revisionに対して詳細情報を返す。

- Before/After
- Hunk情報
- Scene変更情報
- Attribution
- Review状態
- Restore可能性
- 競合状態

## 12.6 権限制限

v1.1ではMCPに直接Restore・Approveのツールを公開しない。

承認と復元はEditor側の明示操作とする。

将来MCPからの実行を許可する場合は、別途権限設定、操作対象の制限、承認トークンを設計する。

---

# 13. データ永続化

## 13.1 保存場所

プロジェクトごとに以下を使用する。

- `.mye/ai_changes/tracking.db`
- `.mye/ai_changes/blobs/`
- `.mye/ai_changes/recovery/`
- `.mye/ai_changes/journal/`

`.mye/ai_changes/` はGit管理対象から除外する。

## 13.2 SQLiteテーブル

主要テーブル：

| テーブル | 用途 |
|---|---|
| `sessions` | AIセッション |
| `operations` | 編集操作 |
| `changes` | 変更対象 |
| `change_revisions` | 変更の各Revision |
| `change_hunks` | コード差分 |
| `scene_changes` | Scene構造差分 |
| `attribution_evidence` | 変更元の根拠 |
| `snapshots` | 保存スナップショット |
| `reviews` | 承認履歴 |
| `restore_operations` | 復元操作 |
| `events` | イベントジャーナル |
| `schema_migrations` | DBスキーマ移行履歴 |

## 13.3 データ整合性

- SQLiteはWALモードを基本とする。
- DBへの書き込みは単一Writerで直列化する。
- Blob書き込み後に内容を検証して参照を確定する。
- DBとファイルシステムの整合性を起動時に検査する。
- 未参照Blobは安全に回収する。
- スナップショット欠落時は復元不可として表示する。
- DB破損時もEditorの通常起動を妨げない。

## 13.4 保存容量

履歴容量と保存期間を設定可能にする。

容量上限到達時は既存履歴の整合性を壊さない。

必要なBlobを削除する前に、履歴の復元可能性が失われることを明示する。

容量不足によって完全な追跡ができなかった場合は、その期間を記録する。

---

# 14. 非機能要件

## 14.1 性能

- Diff計算はワーカースレッドで実行する。
- Editor描画スレッドでは大容量Diffを処理しない。
- FileWatcherイベントはデバウンス・重複排除する。
- UIは履歴を遅延読み込みする。
- Sceneハイライトは可視Entityを優先する。
- Snapshot取得の負荷を計測可能にする。
- 追跡サービス停止時もEditorは動作する。

性能目標値は既存Editorのベンチマークを取得したうえで確定する。

## 14.2 セキュリティ

- プロジェクト外への書き込みを拒否する。
- 相対パス・絶対パスを検証する。
- シンボリックリンク・ジャンクションを検査する。
- 復元直前にパスを再検証する。
- IPC接続元を検証する。
- 任意の外部コマンド実行をChange Tracking APIから提供しない。
- Blob内にソースコードや秘密情報が保存され得ることを考慮する。
- 履歴データをゲーム配布物へ含めない。

## 14.3 決定論

Change TrackingはEditor専用機能とする。

次の情報をWorldHash・Replay・決定論スナップショットに含めない。

- AI Session ID
- Change ID
- Attribution
- Review Status
- Highlight State
- Restore Journal

シーン変更の適用は既存Editor操作経路を使用する。

## 14.4 可用性

- MCP切断時もEditor継続
- Tracking Service停止時もEditor継続
- DB破損時は追跡機能のみ縮退
- 中断Sessionの復旧
- 不完全なOperationの検出
- 復元トランザクションの回復
- 履歴欠落の可視化

---

# 15. 実装ロードマップ

## Phase 0 — 設計・既存コード監査

**優先度：P0**

作業：

- 既存MCPの通信構造確認
- Source Control Diff APIの再利用範囲確認
- UndoStackの適用範囲確認
- SceneSerializerのスナップショット取得時の副作用確認
- GitTransactionの共通化方針策定
- IPCメッセージ仕様確定
- SQLiteスキーマ確定
- サービス異常終了時の復旧方針確定

成果物：

- アーキテクチャ設計
- IPC契約
- DBスキーマ
- 変更帰属ルール
- 復元安全性仕様

**完了条件：** 主要コンポーネントの責務とデータ所有者が確定し、実装前の設計上のP0問題が解消されている。

## Phase 1 — Change Tracking Core

**優先度：P0**

作業：

- Change Tracking Service実装
- SQLite初期化
- Blob Store実装
- Event Journal実装
- FileWatcher連携
- Snapshot Manager実装
- Diff Engine基盤
- 異常終了後の整合性確認

成果物：

- 永続的なファイル変更履歴
- Before/After Snapshot
- 履歴取得API

**完了条件：** Editor再起動後も記録したファイル変更を取得でき、データ欠落時には不完全であることを判定できる。

## Phase 2 — MCP・AIセッション連携

**優先度：P0**

作業：

- Session Registry
- Operation Registry
- MCP追加5ツール
- Attribution Engine
- IPC連携
- セッション中断・再接続処理
- 同時AIセッション検証

成果物：

- AIセッション別の変更一覧
- Operation単位の変更履歴
- 変更元の確実性表示

**完了条件：** 複数AIや人間の編集が混在しても、根拠のないAI帰属を確定しない。

## Phase 3 — Diff UI・基本シーン表示

**優先度：P1**

作業：

- AI Changes Window
- Session History
- Code Diff Viewer
- Scene JSON Diff
- Hierarchy変更バッジ
- Inspector Field Diff
- Scene View基本ハイライト
- Selection連携

成果物：

- AI変更一覧UI
- コード変更行の表示
- Scene変更対象の表示

**完了条件：** 対象ファイル、変更Hunk、Entity、FieldをUIから確認できる。

## Phase 4 — Review Manager

**優先度：P1**

作業：

- Revision別承認
- 部分承認
- 却下
- NeedsReview
- 再レビュー
- Review履歴保存
- 集約状態表示

成果物：

- 承認フロー
- 監査可能なレビュー履歴

**完了条件：** 承認情報が別Revisionへ誤って引き継がれない。

## Phase 5 — Restore Manager

**優先度：安全性要件P0**

作業：

- Restore Preview
- 競合判定
- Hunk復元
- Entity復元
- Field復元
- Prefab Override復元
- UndoStack連携
- Restore Journal
- 異常終了後のRecovery
- Git Index保護

成果物：

- 安全な部分復元
- 復元履歴
- 障害回復機能

**完了条件：** 競合時に危険な自動適用を行わず、途中失敗時にも復旧可能である。

## Phase 6 — 高度なScene比較

**優先度：P2**

作業：

- 削除Entityのゴースト
- Scene Overlay
- Before / Afterプレビュー
- Side by Side描画
- Prefab伝播差分表示
- 大規模Scene表示の最適化

成果物：

- 視覚的な3Dシーン比較
- 詳細なPrefab変更表示

**完了条件：** 本番Sceneを変更せず、過去と現在の視覚比較ができる。

---

# 16. 受け入れテスト

最低限、以下を実施する。

## 16.1 変更検出

- AIがC++ファイルを変更すると履歴が記録される。
- 同じファイルの複数Hunkを検出できる。
- 変更前から存在した人間の未コミット変更をAI変更として混入させない。
- 短時間に連続編集された変更の観測範囲を正しく記録する。
- 取得できなかった中間状態を完全な履歴として扱わない。

## 16.2 セッション・帰属

- 複数AIセッションを区別できる。
- PIDの再利用によってセッションを混同しない。
- Editor停止中のMCP操作を記録できる。
- 編集元が不明な変更はUnknownになる。
- 同一ファイルを複数AIが編集しても誤った確定帰属を行わない。

## 16.3 Scene・Prefab

- Transform変更対象をfileIdで特定できる。
- 別Sceneに存在する同じfileIdを混同しない。
- Entity削除時にBeforeを表示できる。
- Prefab Overrideを区別できる。
- Prefab伝播変更を直接編集と区別できる。
- 未保存Sceneを外部変更で上書きしない。

## 16.4 Review・Restore

- Hunk単位で復元できる。
- Field単位で復元できる。
- 再編集された対象は競合として検出する。
- 承認済みRevisionの次のRevisionはPendingになる。
- Rejectedによって変更が自動削除されない。
- 復元操作を履歴として記録できる。
- 複数ファイル復元の途中失敗から回復できる。
- 復元中の強制終了を検出できる。

## 16.5 既存機能との共存

- Git Indexを勝手に変更しない。
- Play中の危険な変更適用を停止する。
- MCP切断時もEditorが動作する。
- Tracking DB破損時もEditorが起動する。
- Scene Viewの追跡表示がWorldHashに影響しない。
- Replay Verifyの結果が追跡機能の有効・無効で変化しない。
- AI Change Trackingを無効化しても通常のEditor操作が変わらない。

---

# 17. 主な技術リスク

| リスク | 影響 | 対策 |
|---|---|---|
| 外部AIによる変更の取り逃し | 履歴不完全 | ベースラインと観測範囲を記録 |
| 変更元の誤帰属 | 誤ったレビュー判断 | 帰属確実性と根拠の分離 |
| スナップショットの容量増大 | ストレージ圧迫 | Blob重複排除・容量制限 |
| Sceneの保存前編集との衝突 | データ損失 | メモリ/ディスク状態の分離 |
| Prefab伝播の誤認識 | 変更内容の誤表示 | 原因変更との関連付け |
| 復元途中の異常終了 | ファイル不整合 | 永続ジャーナルとRecovery |
| Gitとの操作競合 | 作業状態の破壊 | GitTransaction相当のゲート |
| 追跡処理の高負荷 | Editor性能低下 | 非同期化と遅延処理 |
| サービス通信断 | 追跡停止 | 独立障害・再接続・縮退動作 |

---

# 18. 完成条件

本システムの完成条件を次のように定義する。

**機能面**

1. AIセッションごとに変更を記録できる。
2. 変更元の確実性を表示できる。
3. コード差分をHunk単位で確認できる。
4. Scene変更をEntity・Component・Field単位で確認できる。
5. Before/After比較を提供できる。
6. Revision単位で承認・却下できる。
7. 競合検証を通して変更を復元できる。
8. Editor再起動後も履歴を保持できる。

**安全性**

1. 既存の未コミット変更を破壊しない。
2. Git Indexを勝手に変更しない。
3. 未保存Sceneを上書きしない。
4. 変更元を根拠なく確定しない。
5. 異常終了後の復元処理を回復可能にする。
6. Playモード・ネットワークセッションの安全性を維持する。
7. WorldHashと決定論Replayに影響を与えない。

**検証面**

- 変更検出
- 変更帰属
- 差分生成
- 承認状態
- 復元処理
- 永続化
- 障害回復

これらをそれぞれ独立して自動テストできる構成とする。

---

# 19. 開発方針の最終決定

MyEngin AI Change Trackingは、単なるMCPツールの追加として実装しない。

**独立したChange Tracking Serviceを中心に、既存MCP・Source Control・UndoStack・SceneSerializer・SceneViewWindowを連携させるEditor開発支援機能として実装する。**

初期開発では次の順序を厳守する。

1. 変更を正しく記録する。
2. 変更元を誤判定しない。
3. 変更箇所を可視化する。
4. 承認状態を管理する。
5. 安全に復元する。
6. 高度な3D比較を追加する。

特に変更記録と復元の正確性を、UIの高度化より優先する。

**最終目標：AIがMyEnginに対して行った変更を、開発者が変更単位で追跡・検証・承認・復元でき、既存の開発環境と決定論シミュレーションを破壊しないシステムを完成させる。**
