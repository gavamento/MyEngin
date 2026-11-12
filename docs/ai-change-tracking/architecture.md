# AI Change Tracking: アーキテクチャ設計

- 状態: 設計のみ (2026-11-13)。実装は未着手。進め方は [plans\m104-ai-change-tracking.md](../../plans/m104-ai-change-tracking.md)
- 要求の正本: [spec-v1.1.md](spec-v1.1.md)。決定と却下案: [ADR-039](../adr/ADR-039-ai-change-tracking.md)
- 関連: [ipc-contract.md](ipc-contract.md) / [db-schema.md](db-schema.md) / [attribution-rules.md](attribution-rules.md) / [restore-safety.md](restore-safety.md)

この文書の記述は 3 種類に分けてある。**確認済み** = 2026-11-13 にコードを読んで確かめた事実。
**決定** = この設計で決めたこと。**未検証** = 実装前に試作で確かめる前提。

## 1. 仕様書の前提と既存実装の差 (確認済み)

仕様書 §1.3 は「既存機能の再利用」を原則にしているが、再利用できるものとできないものがある。

| 仕様書の前提 | 既存実装 | 設計への影響 |
|---|---|---|
| 既存 Source Control の Diff 表示を再利用 (§6.3) | Hunk 構造体・unified diff のパーサ・行 diff のどれも無い。`SourceControlWindow::DrawDiffWindow` は private で、`DiffView::text` (生テキスト) を `ImGui::TextUnformatted` 1 回で描くだけ。`PerformanceCompareWindow` は同じ描画をコピーしている | 行 diff・Hunk モデル・Diff ビュー部品は新規。既存の Diff 窓は触らない |
| 256 KiB の表示上限 (§6.3) | 上限は Rust 側 `tools\collab\src\ops.rs` の `MAX_DIFF_BYTES`。C++ 側に定数は無い | 追跡は git diff を通らないので、この上限は構造上かからない |
| FileWatcher を再利用 (§2.1、§5.1) | `src\Engine\Core\Util\FileWatcher` は `ReloadHub` 専用で `assets\` だけを見る。拾うのは変更・追加・リネーム後の名前だけで、**削除とリネーム前の名前は捨てる**。種別も時刻も返さない | 追跡には使えない。サービスが自前で監視する (§4) |
| MCP + ローカル IPC (§3.1) | Named Pipe はリポジトリに 0 件。`mye_mcp.exe` は Editor と通信しない独立 exe で、依存は `serde` / `serde_json` だけ。`tools\mcp\src` の子プロセス起動とファイル書き込みは `tools\check_rules.ps1` 規則 15-c / 15-d の許可表で閉じてある | pipe のサーバとクライアントは新規。規則 15 の許可表に 1 ファイル足す |
| SQLite (§13) | C++ にも Rust にも無い | 新規依存。サービスの crate だけに入れる |
| SceneSerializer でスナップショット (§7.5) | `SaveToJson` / `EntityToJson` / `SubtreeToJson` は読むだけの関数ではない。fileId の無いエンティティに採番し (`FileIdComponent` の追加 = アーキタイプ移動)、`World::ApplyStructuralChanges` を呼ぶ。Play 中に呼ぶと WorldHash は変わらないが SimSnapshot の中身が変わる | Editor Snapshot は編集モードのセーフポイントでだけ取る。Play 中は取らない |
| 未保存 Scene を外部変更で上書きしない (§7.5) | `ReloadHub::ReloadActiveScene` は確認なしで `SceneSerializer::ApplyDiff` を当てる。Undo に積まず dirty にもならない。`ApplyDiff` はファイルに無い fileId を破棄するので、保存前に作ったエンティティが消える。Play 中かどうかも見ていない | 既存挙動の変更が要る (§8) |
| Document ID はリネーム後も同一 (§7.1) | シーン JSON の中に GUID は無い。`<asset>.meta` の `guid` が使える。エディタの `RenameAsset` / `MoveAssetToFolder` は `.meta` を一緒に動かすので保たれるが、エディタ外でのリネームでは新しい guid が採番される | guid を Document ID にし、断絶は断絶として記録する (§6) |
| `fileId` で Entity を特定 (§7.1) | `fileId` はシーン内でだけ一意 (どのシーンも 1 から採番)。プレハブの localId は別のドメイン | 必ず Document ID と組にする |
| GitTransaction 相当のゲート (§11.2) | `ComputeBlockers(const GateInputs&)` は bool だけを受ける純関数で、そのまま使える。入力を組み立てる `EditorApp::BuildGateInputs` は private | 入力の組み立てを共有できる形にする ([restore-safety.md](restore-safety.md) §2) |
| UndoStack 連携 (§11.4) | 1 エントリは before / after のサブツリー全量 JSON。`CaptureAfter` の中でプレハブの override 記録を取り直す。任意のテキストファイルの内容を持つエントリは無い | シーンの復元は Undo に載る。コードの復元は載らないので専用ジャーナル |
| Hunk 単位の復元 (§11.1) | 既存の revert / discard はファイル単位だけ。`git apply` を使う経路は無い | Hunk の適用は自前で書く。git も Index も使わない |

## 2. 構成 (決定)

```
 AI クライアント (Claude Code / Codex CLI / Gemini CLI / その他)
      │ stdio (MCP)                    │ CLI フック
      ▼                                ▼
 mye_mcp.exe ──────────┐      mye_track.exe hook ...   ← 使い捨ての CLI モード
 (tools\mcp、既存)      │              │
                        ▼              ▼
              \\.\pipe\mye-track-<projectId>   (1 行 1 JSON)
                        ▲              │
                        │              ▼
 Editor.exe ────────────┘      mye_track.exe serve     ← 追跡サービス (tools\track、新規)
 src\Editor\ChangeTracking\      ├ Session / Operation Registry
   ├ TrackClient (pipe)          ├ Change Collector (notify による監視 + 報告の受付)
   ├ ChangeModel                 ├ Snapshot Manager / Blob Store
   ├ ReviewManager               ├ Diff Engine (行 diff / シーン構造 diff)
   ├ RestoreManager              ├ Attribution
   └ SceneChangeOverlay          └ History DB (SQLite) / Event Journal
                                        │
                                        ▼
                                <project>\.mye\ai_changes\
                                  tracking.db / blobs\ / journal\ / service.json
                                  recovery\   ← ここだけ Editor が所有
```

### 2.1 責務と所有者

| もの | 所有者 (唯一の書き手) | 読み手 |
|---|---|---|
| `tracking.db`、`blobs\`、`journal\`、`service.json` | 追跡サービス | サービスだけ。他は pipe 越しに問い合わせる |
| `recovery\` (復元ジャーナル) | Editor の `RestoreManager` | Editor |
| 作業ツリーのファイル | 追跡は**書かない**。復元のときだけ Editor が書く | サービスは読むだけ |
| Git Index | 誰も触らない (追跡も復元も) | — |
| ライブの `Scene` | Editor (既存どおり)。復元は `UndoStack` 経由 | — |
| ハイライト・バッジの表示状態 | Editor の `SceneChangeOverlay` (World の外) | Scene View / Hierarchy / Inspector |

サービスは作業ツリーを 1 バイトも書かない。これで「追跡が動いているせいでファイルが変わった」という故障の形を構造上なくす。
復元を Editor 側に置くのは、ゲート (Play 中、未保存、ビルド中)、`ReloadHub` の一括適用、`UndoStack` がすべて Editor にあるため。

### 2.2 配置

| 置き場所 | 中身 | 既存の流儀との対応 |
|---|---|---|
| `tools\track\` | 単独の Rust crate (`mye_track`)。workspace にしない | `tools\mcp\`、`tools\collab\` と同じ。workspace 化は `build_collab.bat` に影響するので見送った前例がある |
| `tools\mcp\src\track_client.rs` | pipe クライアントと 5 ツールの実装 | 道具の追加手順は `tools.rs` の `list()` と `call()` |
| `src\Editor\ChangeTracking\` | pipe クライアント、モデル、Review、Restore、オーバーレイ | `src\Editor\SourceControl\` と同じ階層。規則 12 と同じ形の封じ込め規則を足す |
| `src\Editor\Windows\Project\AiChangesWindow.*` | AI Changes 窓 | `SourceControlWindow` の隣 |
| `src\Editor\Widgets\DiffView.*` | 行 diff の表示部品 | 既存 Diff 窓からは使わない (既存の描画を変えない) |

依存の追加は `tools\track` だけに閉じる: `rusqlite` (bundled)、`sha2`、`notify`、`serde`、`serde_json`。
`tools\mcp` の依存は増やさない (ADR-032 決定 1 を保つ)。C++ 側のハッシュは既存の `src\Engine\Platform\Sha256.h` を使う。

## 3. プロセスと寿命 (決定)

- **1 プロジェクトに 1 サービス。** サービスは起動時に名前付き mutex (`Local\mye-track-<projectId>`) を取る。取れなければ既に居るので即終了し、起動を頼んだ側は既存の pipe へ接続する。
- **起動するのは MCP か Editor。** どちらも「pipe へ接続 → 無ければ `mye_track.exe serve --project <root>` を起動 → 接続をやり直す」の順。Editor は既存の `StartChildProcess` (`src\Editor\App\ChildProcess.h`) を使う。MCP は `track_client.rs` から起動するので、規則 15-c の許可表にこのファイルを足す。
- **親と運命を共にしない。** Job Object に入れない。Editor を閉じても MCP セッションの記録が続き、MCP が終わっても Editor の表示が続く (仕様書 §16.2「Editor 停止中の MCP 操作を記録できる」)。
- **自分で終わる。** 接続中のクライアントが 0 になってから一定時間 (既定 10 分、設定可) で終了する。終了時刻は `events` に残し、次回起動時の欠落区間の計算に使う。
- **サービスが居ないときの扱い。** Editor も MCP も通常動作を続け、追跡だけが止まる。Editor は窓に「追跡停止中」と出す。MCP の 5 ツールは `isError` の結果を返す。
- **projectId。** 正規化したプロジェクトルートのパス (`NormalizePathKey` と同じ規則) の SHA-256 の先頭 16 桁。pipe 名・mutex 名・DB 内の値で同じものを使う。

### 3.1 スレッド (決定)

| プロセス | スレッド | 役目 |
|---|---|---|
| サービス | 接続ごとに 1 本 | 行を読んで要求キューへ積む。応答を書く |
| サービス | Writer 1 本 | DB と Blob への書き込みを直列に行う唯一のスレッド |
| サービス | 監視 1 本 | `notify` のイベントをデバウンスして Writer へ渡す |
| サービス | Diff ワーカー 1 本 | 行 diff とシーン構造 diff。結果を Writer へ渡す |
| Editor | 追加なし | pipe は overlapped I/O で毎フレーム `Poll()` する。`CollabClient` と同じく C++ 側にスレッドを作らない |
| MCP | 追加なし | 既存の同期ループの中で、要求ごとに pipe を往復する |

状態へ反映する順番は Writer のキュー順で決まる。監視イベントと報告が同時に来ても、DB に入る順は 1 本に定まる。

## 4. 変更検出 (決定)

変更源は 4 つ。どれも「観測」として Writer へ届き、帰属は別の段で付ける (仕様書 §1.3「検出と帰属を分離」)。

| 変更源 | 届く経路 | 何が分かるか |
|---|---|---|
| ファイル監視 | サービス内の `notify` | パス、種別 (作成・変更・削除・リネーム)、観測時刻。**誰が書いたかは分からない** |
| MCP の `change_report` | pipe | AI の申告 (パス、Before / After のハッシュ) |
| CLI フック | `mye_track.exe hook` → pipe | 編集の直前と直後。道具の入力 (編集内容) |
| Editor の保存 | pipe (`editor_saved`) | Editor がそのファイルを保存したこと (内容ハッシュ付き) |

- **監視範囲はプロジェクトルート全体。** 無視するのは `.git`、`.mye`、`cache`、`dist`、`target`、`bin`、`obj` と、`.gitignore` に当たるパス。collab の `tools\collab\src\watch.rs` が `.mye` / `cache` / `dist` / `target` を無視しているのに合わせる。
- **ベースライン。** サービス起動時に追跡対象の全ファイルの内容ハッシュを取り、`snapshots` に残す。以後の Before は「最後に確認できた内容」。
- **停止していた間の変更。** 起動時にベースラインを取り直して前回と比べ、違っていれば 1 件の変更として記録する。この変更は観測の完全性が `gap` (欠落あり) で、帰属は Unknown。中間の編集は復元しない。
- **取り逃し。** デバウンス中に同じファイルが複数回書かれたら、最初の Before と最後の After だけが残る。この Revision は `coalesced` (中間状態を見ていない) と記録する。監視バッファがあふれたら、その区間を欠落として記録し、ベースラインを取り直す。
- **追跡する種類。** テキスト (C / C++ / C# / HLSL / JSON / YAML / Markdown / その他 UTF-8) は行 diff まで。UTF-8 として読めないものと既定サイズ (8 MiB、設定可) を超えるものはバイナリ扱いで、ファイル単位の記録だけ。
- **秘密情報。** Blob には原本がそのまま入る。既定の除外パターン (`.env`、`*.pem`、`*.key` など) を持ち、除外したファイルは「変わったこと」だけを内容なしで記録する。

**未検証**: `notify` が短い間隔の連続書き込み・削除・リネームをどこまで区別して返すか。エディタが使う「一時ファイルへ書いて rename」(`WriteFileReplacing`) がどう見えるか。

## 5. コード差分 (決定)

- 行 diff は Myers 法をサービス内に実装する。入力は Before / After の Blob、出力は Hunk の列 (前後 3 行の文脈付き)。
- 比較の基準は保存済みスナップショット。`git diff` は使わない。理由は 3 つ: HEAD との差には AI 以前の未コミット変更が混ざる、Index の状態で結果が変わる、Editor が居ないと collab が動かない。
- 行の区切りは LF / CRLF をそのまま保持して比べる。改行だけが変わった行は「改行の変更」として別に数え、本文の変更に紛れさせない (`.gitattributes` が改行の正本で、規則 14 がある)。
- リネームは、監視がリネームとして返したものに加えて、同じデバウンス窓の中の「削除 + 同じ内容ハッシュの作成」を 1 件にまとめる。内容が変わっていたら別々の削除と作成のままにする (推測でつながない)。
- Hunk ID は (Revision ID, Hunk の通し番号)。Revision が変われば Hunk ID も変わる。承認と復元が古い Hunk を指したままにならない。

## 6. シーン差分 (決定)

### 6.1 識別

- 履歴上のキーは **(projectId, documentId, fileId)**。
- `documentId` は `<asset>.meta` の `guid` (16 桁 hex)。サービスは `.meta` のファイルを自分で読む。Editor の `AssetDatabase` を経由しない (Editor が居なくても決まるようにする)。
- `.meta` が無いファイルは、正規化パスのハッシュを仮の documentId にし、`id_source = path` と記録する。あとで `.meta` が付いたら同じ文書として引き継ぐ。
- エディタ外でリネームされて guid が変わった場合は、**別の文書として扱い、旧文書の側に「ここで追跡が切れた」と記録する。** 内容が似ているからといって同じ文書とは決めない。

### 6.2 スナップショットの 3 種

| 種類 | 取る場所 | 取り方 | 制約 |
|---|---|---|---|
| Disk Snapshot | サービス | ファイルのバイト列をそのまま Blob へ | Editor 不要。いつでも取れる |
| Editor Snapshot | Editor | `SceneSerializer::SaveToJson` の結果を pipe で送る | **編集モードのセーフポイントでだけ。** Play 中は取らない。理由は §1 の表 (fileId の採番と構造変更の適用) |
| Historical Snapshot | サービス | 上の 2 つを Revision に結び付けて保持したもの | — |

Editor Snapshot を取るのは「保存していない編集と、ディスク上の AI の変更を見比べる」ときだけで、常時は取らない。
`SaveToJson` が走るのは既存の保存・Undo と同じ場面に限るので、追跡のために採番のタイミングが変わることはない。

### 6.3 構造比較

- サービスの Diff ワーカーが JSON を読んで比べる。シーンのルートは `{engine, version, sceneName, nextFileId, entities}`、エンティティは `{fileId, name, parent?, childIndex, tagMask?, components, overrides?}`。
- エンティティは `fileId` で対応付ける。配列の中の位置では対応付けない。
- キーの並び順の違いは変更にしない。数値は**字句のまま**比べる (`0.1` と `0.10000000149011612` は別の値として出す)。丸めて同じに見せない。
- `version` (`Scene::kDocVersion`、現行 4) が Before と After で違う、または未知の値なら、構造比較をせず「解析不可」とし、ファイル単位の変更だけを残す。
- 変更種別は仕様書 §7.2 の 11 種。`parent` と `childIndex` の変化は `HierarchyChanged`、`components` のキーの増減は `ComponentAdded` / `ComponentRemoved`、値の変化は `FieldModified`。

### 6.4 プレハブ

確認済みの保存表現: インスタンスのルートに `PrefabInstanceComponent{prefabHash, outerLocalId}`、全メンバに `PrefabLinkComponent{localId}`。
override はエンティティの `"overrides"` キーで、`"Comp.field"` / `"name"` / `"tagMask"` と、構造上書きの `"+Comp"` / `"-Comp"`。子エンティティの追加・削除は override として追跡されていない。

| 観測 | 変更種別 |
|---|---|
| `.prefab.json` / `.actor.json` の内容が変わった | `PrefabAssetChanged` |
| シーン側で `PrefabInstanceComponent` の `prefabHash` が変わった | `PrefabInstanceChanged` |
| シーン側で `overrides` の集合が変わった、または override 済みフィールドの値が変わった | `PrefabOverrideChanged` |
| プレハブのベースが変わり、override していないフィールドが変わった | `PrefabPropagation` (原因の Change ID を付ける) |

- `PrefabPropagation` は**ディスク上のシーンファイルには現れない**。伝播はロード時 (`RefreshNonOverridden`) とホットリロード時 (`PropagateBaseChange`) にメモリ上で起きるため。サービスは「このプレハブを参照しているインスタンスと、override していないフィールド」から影響範囲を計算して記録する。実際の値は Editor Snapshot があるときだけ分かる。
- 子エンティティの追加・削除は override ではないので、`EntityAdded` / `EntityRemoved` として出す。

## 7. Editor 側の表示 (決定)

確認済みの拡張点に、World を変えずに足す。

| 表示 | 足す場所 | 前例 |
|---|---|---|
| AI Changes 窓 | `EditorApp` にメンバ、`layouts_.Init` の表、`OnImGui`、Window メニュー、`SetupDockLayout`、`build\Editor.vcxproj` | 窓の追加は全部 `EditorApp` に手で書く方式 |
| Hierarchy のバッジ | `HierarchyWindow.cpp` の `DrawEntityNode` と `DrawFiltered` の**両方** (通常表示と検索表示は別経路) | `ScmBadgeColor` / `DrawScmTileBadge` |
| Inspector の Field 差分 | `InspectorWindow.cpp` の `DrawComponentFields` のループ、`DrawField` の後 | プレハブの `*` マーカーとフィールドの右クリックメニュー |
| Scene View のアウトライン | `SceneViewWindow.cpp` の `BuildOverlays` から `EditorLinePass::AddWireBox` | `DrawSelectionOutline` (メッシュ AABB のワイヤ箱) |
| 削除 Entity のゴースト | `GhostMeshPass::Add(mesh, world, rgba)` | `SceneViewGhost.cpp` (分岐ゴースト) |
| Before / After の 3D 比較 | 一時 `Scene` + 専用 `RenderSystem` + `RenderTexture` | `AnimationPreviewWindow`、`AssetPreviewCache` |

- 表示状態は `SceneChangeOverlay` が (documentId, fileId) → 変更種別の表として持つ。各窓は毎フレーム `Scene::FindByFileId` で引く。`Selection` と同じやり方で、イベントも World のコンポーネントも使わない。
- fileId を**読むだけ**の関数を使う。`Scene::EnsureFileId` は未採番なら採番してしまうので、表示のために呼ばない。
- 「アウトライン」は輪郭ではなくワイヤ箱になる (輪郭を出すポストプロセスは無い)。仕様書 §8.1 の「緑色アウトライン」はこの意味で読む。
- 色は仕様書 §8.1 が「ユーザー設定で変更可能」としているが、色の設定を保存する前例は無い (テーマ色はコード内の定数)。`EditorSettings` に項目を足す。
- C# コンポーネントのフィールドは別経路 (`DrawManagedComponentFields`) で描かれ、フィールド単位のフックが無い。初期はコンポーネント単位のバッジにとどめる。
- 文字列はすべて `LocalizationTable.inl` と `Tr()`。窓名は `"表示名###固定ID"`。

## 8. 外部シーン変更の挙動変更 (決定)

仕様書 §7.5 と §16.3 を満たすには、既存の自動適用を変える必要がある。ユーザー判断 (2026-11-13) は「未保存のときだけ止めて確認する」。

- `ReloadActiveScene` は Engine 層、dirty の判定 (`EditorApp::IsSceneDirty` = `undo_.StateSerial() != savedStateSerial_`) は Editor 層にある。Engine から Editor を呼ぶ依存は作れない。
- `ReloadHub` に「開いているシーンの外部変更を今適用してよいか」を問う関数の口を足す (`SetKernelHooks` と同じ、呼び手が渡す形)。未設定なら今までどおり適用する = Runtime と既存テストの挙動は変わらない。
- 口が「保留」を返したら、`ReloadHub` はそのパスを保留として覚えるだけで適用しない。Editor は「外部で変更された。読み込む / 自分の編集を保つ」を出す。
  - 「読み込む」: 保留を適用する (今までと同じ `ApplyDiff`)。
  - 「自分の編集を保つ」: 保留を捨てる。次に保存するとディスク側を上書きすることを確認文に書く。
- dirty でなければ今までどおり自動適用する。
- 自分の保存で起きる監視イベントは、今も `ApplyDiff` をもう一度走らせている (同じ内容なので結果は変わらない)。保存直後は dirty でないので、この変更の後も同じ。

**未決 (ユーザー判断が要る)**: Play 中に外部変更が来たときの扱い。今は Play 中の World へそのまま適用され、Stop で Play 前の状態に戻るので、変更は次の外部変更まで反映されないままになる。案は [plans\m104-ai-change-tracking.md](../../plans/m104-ai-change-tracking.md) の未決事項にある。

## 9. 決定論への非干渉 (決定)

- ECS にコンポーネントを足さない。SimSnapshot の版 (現行 82)、TypeId、ABI (v35 = 202 スロット) は変えない。
- 追跡の状態は World の外に置く。前例は `Selection`、`CameraPilot`、Scene View のギズモ状態、`GhostTrack`。
- `src\Editor\ChangeTracking\` を Engine / Runtime / GameLogic / Shared / Server から include させない。規則 12 と同じ形の規則を `check_rules.ps1` に足す (規則 19)。
- §8 の `ReloadHub` の口は Engine 層の変更だが、未設定時の挙動は今と同じ。口が呼ばれるのはフェーズ 2 のセーフポイントで、sim の Tick の中ではない。
- 受け入れ条件: 追跡を有効にした場合と無効にした場合で `tools\replay_verify.bat` の結果が一致する。Scene View のハイライトを出した状態と出していない状態で `HashWorld` が一致する。
- 追跡は Editor 専用。`Runtime.exe` と `Server.exe` はサービスへ接続しない。`.mye\` は配布物に入らない (既に `.gitignore` とプロジェクトのテンプレートが除外している)。

## 10. 障害の局所化 (決定)

| 起きたこと | 影響の範囲 | ふるまい |
|---|---|---|
| サービスが落ちた / 居ない | 追跡だけ | Editor と MCP は続行。次に接続できたとき欠落区間を記録する |
| MCP が切れた | そのセッションだけ | セッションを Interrupted にする。監視による記録は続く |
| Editor が落ちた | 表示と復元だけ | サービスは記録を続ける。復元の途中だったら次回起動時に回復を出す |
| DB が壊れた | 履歴の閲覧 | 壊れた DB を退避し、`journal\` と `blobs\` から作り直せる分だけ作り直す。Editor の起動は止めない |
| ディスクが足りない | 新しい記録 | 既存の履歴は壊さない。記録できなかった区間を欠落として残す |
| Blob が欠けた | その Revision の復元 | 「復元不可」と表示する。一覧と差分の要約は残す |

## 11. 性能の考え方 (決定)

- 目標値はまだ置かない。仕様書 §14.1 のとおり、既存 Editor の計測を取ってから決める (AGENTS.md §3.5)。
- 重い処理 (ハッシュ、diff、JSON の比較、DB) はすべてサービス側のプロセスで行う。Editor のフレームに入るのは pipe の `Poll()` と表の参照だけ。
- 一覧はページ単位で取り、Hunk と Blob は開いたときに取る。
- 計測できるようにするもの: ベースラインの所要時間とファイル数、1 件の変更の記録にかかった時間、Blob の総量、pipe の往復時間。サービスの `status` で返す。
