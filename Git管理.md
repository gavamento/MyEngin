# Rustを使ったゲームエンジン内Git・コラボレーション機能まとめ

## 1. 全体構成

```
MyEngine
│
├─ C++ Engine / Editor
│   ├─ Content Browser
│   ├─ Scene Editor
│   ├─ Material Editor
│   ├─ Source Control Panel
│   ├─ Pull Request Panel
│   └─ Notification UI
│
└─ Rust Collaboration Service
    ├─ GitService
    ├─ RemoteMonitor
    ├─ ContentManager
    └─ CodeHosting
```

---

## 2. C++側の役割

- エンジン本体
- Editor UI
- Content Browser
- Scene Editor
- Material Editor
- Git状態の表示
- PR・Reviewの表示
- 通知表示

Rust側とは高レベルAPIで通信する。

---

## 3. Rust側の役割

### GitService

- Repository管理
- Status取得
- Stage / Unstage
- Commit
- Branch作成・切り替え
- Fetch
- Pull
- Push
- Diff
- History
- Sparse Checkout
- Credential管理

Git操作の基本バックエンドはGit CLIを使用する。

```
Rust
 ↓
Git CLI
 ↓
Local Repository / Remote
```

---

## 4. Git状態管理

管理する状態：

```
Modified
Added
Deleted
Renamed
Untracked
Conflict
```

Content Browser上にも状態を表示する。

```
Assets/
├─ Player.scene       M
├─ Enemy.prefab       A
├─ Level.scene        M
├─ OldTexture.png     D
└─ Test.png           ?
```

操作：

- Stage
- Unstage
- Revert
- Diff
- History
- Commit

---

## 5. Git処理の非同期化

Git処理はEditorスレッドで直接実行しない。

```
C++ Editor
    │
    ▼
Rust Worker
    │
    ▼
Git CLI
    │
    ▼
Result
```

対象：

- Clone
- Fetch
- Pull
- Push
- Remote確認

Statusも毎フレーム取得せず、ファイル変更を検知して更新する。

```
FileSystemWatcher
 ↓
変更検知
 ↓
Debounce
 ↓
Git Status取得
 ↓
Cache更新
```

---

## 6. 起動時Remote確認

エンジン起動時にRemoteとの差分を確認する。

```
MyEngine起動
 ↓
git fetch
 ↓
HEAD と origin/main を比較
 ↓
Remoteに変更がある場合
 ↓
Editorへ通知
```

通知内容：

- Remote側の追加Commit数
- Commit一覧
- Author
- Commit Message

自動Pullは行わず、Fetchと通知のみ行う。

---

## 7. Remoteから一部だけ取得

### Branch単位

必要なBranchだけFetchする。

```
main
develop
feature/player
feature/stage02
```

---

### Sparse Checkout

Repository内の必要なDirectoryだけWorking Treeに展開する。

```
Repository
├─ Engine/
├─ Source/
├─ Assets/
│   ├─ Characters/
│   ├─ Environment/
│   ├─ Audio/
│   └─ Movies/
└─ Tools/
```

必要なものだけ：

```
Source/
Assets/Characters/
```

---

## 8. Content Browserとの連携

AssetのLocal / Remote状態を表示する。

```
Assets/

▼ Characters
   Player.fbx        Local
   Enemy.fbx         Local

▷ Environment        Remote
▷ Movies             Remote

▼ Audio
   Battle.wav        Remote
   Footstep.wav      Local
```

操作：

- Download
- Download Folder
- Keep Offline
- Remove Local Copy

内部ではSparse CheckoutなどのGit機能を利用する。

---

# 9. GitとGitHubを分離

Git操作とGitHub機能は別レイヤーとして扱う。

```
ISourceControlProvider
└─ Git
```

```
ICodeHostingProvider
├─ GitHub
├─ GitLab
├─ Bitbucket
└─ Azure DevOps
```

Git操作：

```
Git CLI
```

PR・Review：

```
GitHub API
```

---

# 10. Pull Request連携

Editor内で以下を確認する。

- PR一覧
- Author
- Base Branch
- Head Branch
- CI状態
- Review状態
- Changed Files
- Unresolved Review数

操作：

- PR Checkout
- Diff表示
- Review確認

---

# 11. Reviewコメント表示

取得する情報：

```
File Path
Line
Start Line
Commit
Diff
Comment
Reviewer
Resolved
Outdated
```

コードEditor上：

```
PlayerController.cpp

120 │ void Jump()
121 │ {
122 │     velocity.y = 10.0f;  💬
123 │ }
```

Review操作：

- Reply
- Resolve
- Review Thread表示
- PR表示

---

# 12. Scene / Asset Review

Gitのテキスト差分をEngine内部データに変換する。

```
PR Review
 ↓
File
 ↓
Semantic Diff
 ↓
Object ID
 ↓
Scene Object
```

Scene Editor上で対象ObjectにReviewを表示する。

対象：

- Scene
- Prefab
- Material
- Animation
- Shader

---

# 13. Scene Objectの永続ID

Scene内Objectには永続IDを持たせる。

```json
{
    "id": "43c7e8a1",
    "name": "Enemy_13",
    "transform": {
        "position": [18, 2, 5]
    }
}
```

これにより、

```
Git Diff
 ↓
Object ID
 ↓
Asset Database
 ↓
Scene Object
```

としてReview対象を特定する。

---

# 14. Rust側の最終構成

```
CollaborationService
│
├─ GitService
│   ├─ Repository
│   ├─ Status
│   ├─ Stage
│   ├─ Commit
│   ├─ Branch
│   ├─ Diff
│   ├─ Fetch
│   ├─ Pull
│   ├─ Push
│   └─ SparseCheckout
│
├─ RemoteMonitor
│   ├─ Fetch
│   ├─ AheadBehind
│   ├─ RemoteChange
│   └─ Notification
│
├─ ContentManager
│   ├─ DownloadAsset
│   ├─ DownloadFolder
│   ├─ Availability
│   └─ Cache
│
└─ CodeHosting
    └─ GitHubProvider
        ├─ Authentication
        ├─ PullRequest
        ├─ Review
        ├─ ReviewThread
        ├─ Comment
        ├─ Resolve
        └─ Checks
```

---

# 15. C++側に公開するAPI

```cpp
SourceControl::GetStatus();

SourceControl::Stage(files);
SourceControl::Commit(message);

SourceControl::CheckRemote();
SourceControl::Fetch();
SourceControl::Pull();
SourceControl::Push();

SourceControl::DownloadAsset(asset);
SourceControl::DownloadFolder(folder);

SourceControl::GetPullRequests();
SourceControl::GetReviews(asset);

SourceControl::ReplyReview(thread, message);
SourceControl::ResolveReview(thread);
```

---

# 16. 最終構成

```
C++ Engine / Editor
        │
        ▼
Rust Collaboration Service
        │
        ├─ Git
        ├─ Remote Sync
        ├─ Selective Content
        ├─ Pull Request
        ├─ Review
        └─ Notifications
        │
        ├──────────────┐
        ▼              ▼
     Git CLI       GitHub API
```

---

# 17. 今後の検討事項

## 巨大ファイルへの対応

ゲーム開発では以下のような巨大ファイルを扱う可能性がある。

- Texture
- FBX / 3D Model
- Audio
- Video
- PSD
- Blenderファイル
- その他のBinary Asset

通常のGit管理だけではRepositoryサイズ、Clone時間、Fetch時間、ストレージ使用量などが問題になる可能性がある。

そのため、実際のAsset規模や運用方法が決まった段階で、**巨大ファイルをどの方式で管理・配布するか別途検討する必要がある。**