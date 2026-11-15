# Editor の保存の経路と `editor_saved` (M104k2)

Editor が自分で書いたファイルを、追跡サービスへ「この内容で保存した」と知らせる仕組みと、どの経路から知らせるか (しないか) の一覧。
サービス側の取り決めは [ipc-contract.md](ipc-contract.md) §4.4、判定の表は [attribution-rules.md](attribution-rules.md) §3。

## 1. 仕組み

```
各保存の経路 ──(保存直後)──▶ scmhint::Changed(絶対パス)  ──┐
シーン・アクター・.gitignore の保存 ─────────────────────────┤
                                                            ▼
                                        EditorApp::NoteFileSaved(絶対パス)   ← 送る場所は 1 か所
                                          ├─ SourceControlSession::HintSaved   (既存)
                                          └─ ChangeTrackingSession::NoteSaved  (順番待ちに積むだけ)
                                                            │ 次の Poll から
                                                            ▼
                              ファイルのハッシュ (SHA-256) → editor_saved { path, hash } を pipe へ
```

- **送る場所は 1 か所**: 保存直後の通知の口は、もともと Source Control のヒント用に集まっていた `scmhint::Changed` (`src\Editor\SourceControl\ScmHint.h`) と、`EditorApp` が直接持つ保存 (シーン・アクター) だけ。どちらも `EditorApp::NoteFileSaved` に入り、そこから `ChangeTrackingSession::NoteSaved` を 1 回呼ぶ。各保存の経路が `TrackClient` を直接呼ぶことはない。
- **Engine 層の `WriteFileReplacing` には手を入れない。** Engine 層の関数 (`SceneSerializer::SaveToFile`、`Prefab::SaveEdited`、各ライブラリの `SaveToFile` など) は書いたバイト列を呼び元へ返さないので、Editor は**書き終わったファイルを 1 回読み直して**ハッシュを取る (`Sha256FileHex`。ストリームで読み、全体をメモリへ載せない)。書いたバイト列から計算する形は、Engine 層の書き込みを変えない限り取れない。
- **保存を失敗させない・遅らせない**: `NoteSaved` は順番待ちに積むだけ (ハッシュも I/O もしない)。ハッシュと送信は次の `Poll()` 以降で、1 回の `Poll()` につき `kTrackSavedPerPoll` (2) 件まで。上限 `kTrackSavedHashMaxBytes` (8 MiB) を超えるファイルは知らせない。順番待ちの上限は `kTrackSavedMaxPending` (256)、同じファイルは 1 件にまとめる。
- **無効・非対話起動・切断中はハッシュも計算しない**: `NoteSaved` は `TrackState::Ready` のときだけ積む (設定で無効 / 非対話起動の `--selftest` / `--screenshot` / リプレイ検証は `Disabled`、プロジェクト無しは `NoProject`、接続前は `Connecting`、切れていれば `Disconnected`、サービスが DB へ書けない間は `DbDegraded`)。積んだあとに切れたら、順番待ちは捨てる (あとから知らせても照合できない)。
- **積まないもの**: プロジェクトの外、`.mye\` の下 (サービス自身のデータ。追跡の対象外)、ルート自身、空のパス。
- **送れなかった場合**: 何もしない。その保存は、観測だけから `unknown` / `inferred` になる。応答の失敗はログに 1 行残すだけ。

サービスは受けるとすぐ対象パスを読み、知らせを `editor_saved` のイベントとして記録する。観測した Revision の After のハッシュが一致すれば、知らせと観測のどちらが先でも「Editor の利用者」(`editor_user` / `verified`) になる。

## 2. 洗い出した経路

2026-11-15 に `src\Editor\` の書き込み (`WriteFileReplacing`、`std::ofstream`、`fs::rename`、`fs::copy_file`、`AssetDatabase::WriteMeta`、各ライブラリの `SaveToFile`) の呼び手を全部たどった結果。「通知」の列が **あり** の経路は、この M104k2 で `NoteFileSaved` へ繋がるようにした (または元から繋がっていた)。

### 2.1 知らせる経路

| 経路 | 書き手 | 通知の入口 | 備考 |
|---|---|---|---|
| シーンの保存 (Ctrl+S、File メニュー、名前を付けて保存) | `EditorApp::SaveCurrentScene` → `SceneSerializer::SaveToFile` | `EditorApp::NoteFileSaved` を直接 | 保存に成功したときだけ |
| アクター / プレハブ アセットの保存 (ミニシーン編集) | `EditorApp::SaveActorEdit` → `Prefab::SaveEdited` | `EditorApp::NoteFileSaved` を直接 | 成功したときだけ |
| プレハブの Apply (Inspector の「Apply All」「Apply to Prefab」) | `Prefab::ApplyInstance` → `.prefab.json` | `scmhint::Changed` (`InspectorWindow.cpp` の `ApplyPrefabInstanceAndNotify`) | Apply はアセットファイルを書く。書き換わったプレハブのパスを渡す |
| アセットの `.meta` の書き込み (テクスチャの取り込み設定、モデルの LOD 設定、取り込みポップアップ) | `AssetDatabase::WriteMeta` | `scmhint::Changed(.meta のパス)` (`InspectorWindowAssets.cpp`、`AssetBrowserWindow.cpp`) | スプライトエディタの `.meta` (`SpriteEditorWindow.cpp`) は元から通知していた |
| アセットの移動・リネーム | `AssetOps::PerformAssetRelocate` (`fs::rename`) | `scmhint::Changed(dst)`、`scmhint::Changed(dst + ".meta")` | 移動後のパス (リネームのまとめは観測側が行う)。移動元は消えているので、読めず送られない。**フォルダの移動は知らせない** (個々のファイルのハッシュを取らない。その中のファイルは `unknown` / `inferred` になる) |
| アセットの作成・複製 | `AssetOps` の `Create*` / 複製、`AssetBrowserWindow` の作成 | `scmhint::Changed` (`AssetOps.cpp`、`AssetBrowserWindow.cpp`) | 元から通知していた |
| 設定ファイル `project_settings.json`、`input\actions.json`、フォントメトリクスのクック出力 | `ProjectSettingsWindow`、`EditorApp`、`InspectorWindow` | `scmhint::Changed` | 元から通知していた |
| `.gitignore` の追記 (Source Control の推奨行) | `EditorApp` の `scmHost.applyGitignore` | `EditorApp::NoteFileSaved` を直接 | |
| アセットの Inspector の保存ボタン (マテリアル `.mat.json`、`.sound.json`、`.physmat.json`、`.navfilter.json`、Feel) | `InspectorWindowAssets.cpp`、`FeelAssetInspector.cpp` | `scmhint::Changed` (この M104k2 で足した) | |
| ビヘイビアツリー、アニメーション、アニメーターコントローラ、オーディオミキサー、タイルパレット / タイルマップ、2D 図形、スプライトアニメ、ナビ / 破砕のベイクの保存 | 各窓、`NavBakeCommit`、`FractureBakeCommit` | `scmhint::Changed` | 元から通知していた |

### 2.2 知らせない経路 (と理由)

| 経路 | 理由 |
|---|---|
| `.mye\` の下の書き込み (`editor_settings.json`、`layouts`、`ai_changes`) | 追跡の対象外 (サービス自身のデータ)。`NoteSaved` が捨てる |
| `editor_global.json`、プロジェクトの一覧 (`ProjectRegistry`)、パネルの配置 | プロジェクトの外 (機械ローカル)。`NoteSaved` が捨てる |
| ビルド・パッケージの出力 (`BuildSettingsWindow` の `bin\` / 出力先)、`cache\`、`obj\` | 追跡しない既定の除外 (`ignore.rs`) の中、またはプロジェクトの外 |
| ごみ箱への削除 (`AssetOps` の削除) | `scmhint::Changed` は呼ばれるが、ファイルが無いので読めず送られない。削除は「ファイルが無い」ことで観測される。Editor の削除を Editor の利用者として確定する根拠 (内容のハッシュ) は無い (`unknown` / `inferred` のまま。今後の課題) |
| Undo / Redo によるアセットの書き戻し (`AssetOps` の `UndoFileOp`) | 既存の `scmhint::Changed` が無い経路。Source Control のヒントにも出していない。この M104k2 の範囲外 (保存の経路そのものは変えない) |
| スクリプトのテンプレート生成・IDE のプロジェクトファイル生成 (`WriteTextIfChanged`、`.vcxproj` など) | 生成物。利用者の保存ではなく、内容が同じなら書かない。通知の口が元から無い |
| プロジェクトの新規作成のテンプレート (`ProjectTemplates`) | プロジェクトを開く前。追跡は開いたプロジェクトの中でだけ動く |
| デバイス消失の救出 (`DeviceLostRescue`)、`ModalTools`、`EngineIndexDump` | ツールの出力 (診断・生成物)。通知の口が元から無い |
| Engine 層が自動で書くもの (`AssetDatabase::EnsureMeta` が不在の `.meta` を作る、`SaveGame`、`DisplaySettings` など) | Editor の利用者の保存操作ではない。通知の口が無く、Engine 層には手を入れない |
| Runtime / ゲーム側の書き込み | Editor ではない (追跡の Editor クライアントが知らせる範囲の外) |
| 外部エディタ (AI Changes 窓の「ファイルを開く」で開いたもの) での保存 | Editor の保存ではない。AI や別のツールの変更と同じく、観測だけから帰属が決まる |

知らせない経路で書かれたファイルは、AI のセッションが動いていなければ `unknown`、動いていれば時刻の近さで `inferred` になりうる。AI Changes 窓は両方を「不明 / 推定」の節に分けるので、誤って AI のものとは見えない。

## 3. 通る確認

- Editor 側 (`MYE_SELFTEST_ONLY=changetracking` の `Editor.exe --selftest`): 経路の形ごとに、保存すると `editor_saved` が 1 回だけ送られ、ハッシュが書いた内容の SHA-256 と一致すること。無効・非対話起動・プロジェクト無し・接続前・切断中・切断された後は、送られず、ハッシュの計算も呼ばれないこと。プロジェクトの外と `.mye` の下は積まれないこと。同じファイルの重複が 1 件になること。上限を超えるファイル・消えたファイル・フォルダは送られず、接続を乱さないこと。1 回の `Poll()` が扱う件数に上限があること。
- サービス側 (`cargo test`): 知らせが観測より先でも後でも同じ結果、ハッシュ違い・パス違い・窓より古い知らせは付かない、AI の宣言と同じ内容なら Editor が勝つ、DB を壊して作り直しても同じ根拠が戻る、`mcp` / `hook` は `forbidden`。
- 実際の Editor と実サービスの結合 (手動): 一時プロジェクトで追跡を有効にし、シーンを保存すると、AI Changes 窓の「エディタ / 復元による変更」の節にその変更が「エディタの操作」として出る。
