# sub-16: ライブのシーンの復元 (M104q)

- 依存: sub-27 (順序は sub-15 → sub-27 → sub-16)
- 状態: OK (commit は台帳 harness.md を参照)
- 往復: 0
- 書き込みの深さ: **詳細** (2026-11-15、sub-27 の VERDICT で planner が書き直した)。Phase 5 の最後のサブで、Phase 5 の横断の検証 (A20) もここで回す。
- 質問の運用: 推奨で進め、Notion に後回し。着手前の確認は無い。

`C:\HAL\MyEngin\docs\ai-change-tracking\restore-safety.md` §3.3、§4.2 (restore)、要求 §11.1 / §11.4。**World を書き換えるので、Undo に載せることと、Play 中に触らないことが存在条件。**

## 範囲の取り決め (planner の裁定。spec §2 D40、D41)

1. **シーンの構造の単位 (Entity / Component / Field / Override) の復元は、今エディタで開いているシーンだけ。** 開いていないシーンの文書は、sub-15 / sub-27 のファイルの経路 (ファイル全体・Hunk) で戻す。restore §3.3 は「開いていなければディスク上の JSON に照合してファイルの経路で書く」としているが、構造の単位で JSON の一部だけを字句を保って書き換える処理は新しい大きな部品になり、Hunk の復元で代わりが利く。窓では、開いていないシーンの構造の変更の行に「このシーンを開くと、項目ごとに戻せます。開かずに戻すなら、ファイルまたは Hunk の復元を使います」と出す。
2. **「今の値が記録した After と同じか」の比べ方**: 記録の値は字句のまま (sub-11)、ライブの値はエンジンの型。字句どうしでは比べられない (`1.0` と `1`、AI が書いた書き方とシリアライザの書き方が違う)。**After の字句を、そのフィールドの型で読み (`FieldFromJson` を一時の領域へ)、`FieldToJson` で書き直した正規形**と、**ライブの値の `FieldToJson`** を比べる。読めない (型が合わない) After は競合。コンポーネント・エンティティは、フィールドごとの正規形の集合で比べる。C# コンポーネントは managed の `Serialize` の出力どうし (sub-01 の試作 4。値は 10 種類の型)。この規則を restore §3.3 に書く。
3. 開いているシーンのライブの状態が、ディスクの After とまだ同じになっていない場合 (外部の変更の確認が保留中 = sub-13、または読み込まずに「自分の編集を保つ」を選んだ) は、照合が合わず競合になる。それで正しい (After が当たっていないものは戻せない)。保留中は「先に外部の変更を読み込むか決めてください」と出す。

## 使えるもの

- `UndoStack::Record(label, scene, selection, fids, structural, edit)` (`C:\HAL\MyEngin\src\Editor\Undo\UndoStack.h` 113 行目)。`StructuralChanges::Apply` (108 行目)。`CaptureAfter` がプレハブの override の記録を取り直す (**この経路を通さずに値を書くと override の記録が取りこぼされ、静かにデータが失われる**)。
- `SceneSerializer::ApplyPartial(scene, entities, removeHiddenMissing)` (`SceneSerializer.h` 60 行目)、`EntityToJson` / `SubtreeToJson` (46、47 行目。**fileId の無いエンティティに採番し構造変更を適用する** = 編集モードのセーフポイントでだけ呼ぶ。arch §1)。
- `FieldFromJson` / `FieldToJson` (`C:\HAL\MyEngin\src\Engine\Core\Util\JsonUtil.h` 12、15 行目)。C# は `ManagedHost::DeserializeComponent(t, e, payload, json)` (`ManagedHost.h` 207 行目)。
- sub-27: `RestoreRunner` の Begin / End のフック、`opInFlight` への反映、回復の画面、履歴、`restore_record` / `restore_expect`、`RestoreRunWindow`。sub-15: `RestoreGate` (`EvaluateRestoreGate`)、`RestoreFormat`。sub-12: `SceneChangeOverlay` (fileId → 状態、`components[].fields[]` の before / after)。sub-11: `scene_diff` の値 (JSON の断片、字句のまま)。

## やること

### A. 照合と計画 (`src\Editor\ChangeTracking\SceneRestorePlan.h/.cpp`、新規。ImGui を呼ばない。World を書かない)

restore §3.3 の表を純粋に判定する (入力: 構造の変更の行、ライブのシーンから読んだ値。出力: 計画か、競合の理由)。

| 単位 | 照合 | 戻す内容 |
|---|---|---|
| Field | `documentId` が開いている文書と一致、`fileId` のエンティティがいる、そのコンポーネントがある、今の値の正規形 == After の正規形 | Before の字句を、そのフィールドの型で書く |
| Component (追加を取り消す) | コンポーネントがあり、全フィールドが After と一致 | コンポーネントを外す |
| Component (削除を取り消す) | コンポーネントが無い | Before の内容でコンポーネントを付ける |
| Entity (追加を取り消す) | エンティティがいて、サブツリーが After と一致 | サブツリーを消す |
| Entity (削除を取り消す) | その `fileId` が使われていない。親の `fileId` がいる | Before のエンティティ (と、同じ Revision で消えた子孫) を作る |
| Prefab Override | `overrides` の該当キーの有無と値が After と一致 | Before の override に戻す |

- `EntityRef` のフィールドは `fileId` の値で比べる。戻す値が指す `fileId` がシーンにいなければ「依存の欠落」で止める (restore §3.4)。
- `documentId` は必ず照合する。別のシーンの同じ `fileId` に当てない。
- Before が無い (追加の取り消し以外で Before の値を見ていない) / Blob が欠けている / `unparsed` の Revision は戻せない。
- `PrefabPropagation` は戻す対象にしない (原因の変更へ案内する。伝播の記録は sub-19 で入る。今は出てこない)。
- **復元済み**: 今の値が Before と同じなら何もしない。
- 複数の項目をまとめて戻す (エンティティ全体、Revision 全体): 1 つでも競合なら、まとめて戻すものは 1 つも当てない (sub-15 と同じ規則)。

### B. 適用 (`SceneRestoreRunner` か `RestoreRunner` の分岐。ImGui の外、編集モードのセーフポイントで)

1. ゲート: restore §2 の「ライブのシーンの復元」の列。`Playing` / `NetActive` で止める。`BuildRunning` / `OpInFlight` / `MergeInProgress` / `SceneDirty` では止めない。`ActorEdit` は、対象が編集中の文書でなければ止める (ミニシーンの編集中は `ctx.scene` が差し替わっている)。追跡サービス未接続で止める (Before の値を取れない)。sub-13 の外部の変更の保留中は止める (上の取り決め 3)。理由は全部並べる。実行の直前に判定し直す。
2. **必ず `UndoStack::Record` の中で書く**。ラベルは「AI の変更を復元」系 (英日)。対象の `fids` を正しく渡す。Component の追加・削除、Entity の追加・削除を戻すときは `StructuralChanges::Apply`。
   - Field: `FieldFromJson(comp, field, value)`。C# コンポーネントは `DeserializeComponent` に戻したいフィールド 1 つだけの JSON (sub-01 の試作 4)。
   - Entity / Component: `SceneSerializer::ApplyPartial`。
3. 書いた後に読み直して、Before の正規形と同じことを確かめる。違えば同じ Undo の中で元へ戻し (Record の中で失敗を検出したら、自分で書いた分を戻してから抜ける)、失敗として出す。
4. **シーンの復元はディスクを書かない。** シーンは dirty になる。保存は利用者がする (保存すると `editor_saved` で「Editor の利用者」の変更として記録される)。ジャーナルは作らない (restore §5: Undo が戻す手段)。
5. サービスへの記録: `restore_record` (対象 = 構造の変更の行、状態 `committed` / 失敗)。サービスは対象の `scene_changes.state` を `restored` にする (sub-27 は `changes` / `change_hunks` だけ。**`scene_changes` を足す**)。`scene_overlay` は `state = active` の行だけを返すので、戻した項目は強調から消える。Undo で戻した場合の記録はしない (restore §4.2)。`restore_expect` は送らない (ディスクを書かない)。
6. 編集モードのセーフポイントで行う: Play 中は呼ばれない (ゲート)。`EntityToJson` / `SubtreeToJson` を照合のために呼ぶなら、それがアーキタイプを動かしうることを踏まえ、編集モードでだけ呼ぶ。**照合のための読み取りでは、fileId の採番が起きない読み方 (`FileIdComponent` を読むだけ、フィールドを `FieldToJson` で読むだけ) を優先する。**

### C. C# コンポーネント (sub-01 の試作 4 の残り)

このサブで実走して確かめる。1 つでも成り立たなければ、C# コンポーネントは Entity 単位の復元だけにして、Field / Component の復元を選ぶと理由が出るようにする (restore §4.2 に結果を書く):
- (a) 1 キーの JSON で、他のフィールドが変わらない。
- (b) `UndoStack::Record` 経由の復元が Ctrl+Z (Undo) で戻り、Redo でもう一度当たる。
- (c) プレハブのインスタンスの override の記録が、C# コンポーネントのフィールドで保たれる。
C# のホストが動かない環境 (`dotnet` の無いランナーなど) で SKIP になるなら、SKIP を成功扱いにせず報告する。

### D. AI Changes 窓

- 「シーンの変更」の一覧 (sub-12) の行と、詳細の構造の変更に「復元…」。範囲 (フィールド / コンポーネント / エンティティ / Revision の全部) を選べる。プレビュー (sub-15 の形に合わせる): 止める理由、照合の結果、戻すと行う操作 (どのエンティティのどの値を何から何へ)、「Undo (Ctrl+Z) で取り消せます」「シーンは未保存になります。保存は手で」の注記。
- 実行後の結果の表示。復元の履歴 (sub-27 の画面) にシーンの復元も出る。
- 開いていないシーンの行には、上の取り決め 1 の文面。
- 画面は `AiChangesWindow.cpp` に大きく足さず、sub-27 の `RestoreRunWindow` 側か新しいファイルに置く。文字列は英日。

### E. sub-27 から持ち越した点

1. **削除を含む復元の後のアセットの登録**: `ReloadHub` は `BatchChange::Deleted` を扱わない (既存のコメント) ので、追加の取り消し (ファイルの削除) をした後、アセットの登録が残る。Editor 層に既存のアセットの再走査の経路 (Asset Browser の更新など) があれば、削除を含む復元の後に呼ぶ。**Engine 層には手を入れない。** 経路が無ければ、結果の画面に「アセットの一覧を更新してください」と出し、`restore-safety.md` に限界として書く。
2. 段階 B / C (スクリプトの再コンパイル・再起動) は自動にしない (プレビューの注記のまま)。変えない。

### F. 文書

- restore §3.3 (正規形での比べ方、開いているシーンだけ、C# の結果)、§4.2、arch §7、ipc (`restore_record` の対象に構造の変更)、`C:\HAL\MyEngin\engine_spec.md` §9.2、`C:\HAL\MyEngin\docs\test_checklists.md`、`C:\HAL\MyEngin\plans\m104-ai-change-tracking.md` (進捗表)。

## やらないこと (このサブでは)

- 開いていないシーンの構造の単位の復元 (取り決め 1)。ディスクの JSON の部分的な書き換え。
- `PrefabPropagation` の扱い (sub-19)。プレハブのアセットそのものの構造の単位の復元 (プレハブのファイルはファイル / Hunk の経路)。
- Hunk の複数選択の UI、複数ファイルの計画 (M104 では行わない。spec §3 の後回し)。
- 下位の層の変更 (変わってよいのは `LocalizationTable.inl` だけ)。ECS・ABI・SimSnapshot・TypeId。

## 受け入れ条件 (このサブ)

1. Field を 1 つ戻すと値が Before になり、Undo で After に戻り、Redo で Before になる → `changetracking` スイート (要求 §16.4 の 2 番目)。
2. 正規形の比べ方: After の字句が `1.0`、ライブが `1` の `float` → 一致。型が合わない After → 競合。`EntityRef` は `fileId` で比べる → スイート (純粋な関数)。
3. 今の値が After と違う Field は競合になり、World が変わらない (`HashWorld`、エンティティ数、アーキタイプ数が同じ) → スイート。
4. 別のシーンを開いているとき、同じ `fileId` のエンティティに当たらない → スイート (§16.3 の 2 番目)。
5. Component の追加の取り消し・削除の取り消し、Entity の追加の取り消し・削除の取り消し (子孫つき)。親がいない、`EntityRef` の先がいない場合は止まる。Undo でそれぞれ戻る (**何も戻らない Undo のエントリができない**: Undo の前後で値が変わることを確かめる) → スイート。
6. プレハブのインスタンスの override 済みフィールドを戻した後、`overrides` の記録が正しい (取りこぼしが無い)。保存して読み直しても同じ → スイート。
7. C の (a) (b) (c) をそれぞれ確かめる試験がある。成り立たなかった場合は Entity 単位だけに落とし、Field の復元を選ぶと理由が出る → スイート (C# のホストが要る。SKIP を成功扱いにしない)。
8. Play 中・ネットセッション中は止まる。ビルド中・シーンが未保存でも止まらない。ミニシーンの編集中は止まる。外部の変更の確認が保留中は止まる。理由が全部並ぶ。確認の後に Play を始めたら実行の直前で止まる → スイート。
9. シーンの復元はファイルを書かず (一時プロジェクトの全ファイルのパスとハッシュが前後で同じ)、シーンが dirty になる。ジャーナルが作られない → スイート。
10. サービス: `restore_record` の対象に構造の変更を渡すと、`scene_changes.state` が `restored` になり、`scene_overlay` から消える。レビューの記録は変わらない。DB を作り直しても同じ → `cargo test` + `tools\track_verify.bat`。
11. E の 1 の結果 (再走査を呼ぶか、限界として出すか) → スイートか文書。
12. 実サービスとの結合 (`MYE_TRACK_PROBE=1`、一時プロジェクト): シーンのファイルを外から書き換えて読み込む → AI の変更として記録 → Field を 1 つ復元 → 値が戻る → `scene_changes` が `restored` → Undo → 値が After に戻る → プロセスが残らない。
13. スクショ (一時のプローブ): シーンの復元のプレビュー、結果、開いていないシーンの文面、英語。**見た目の合否はユーザーの目視。**
14. **A20 (Phase 5 の横断)**:
    - `bin\x64\Release\Editor.exe --selftest-full` が全部通る。Debug は `--selftest-list` の `fast` / `full` の行のスイートだけ (別名を混ぜない) から `physics` を除いた全部が通る (spec §2 D38)。実行中と後に `mye_track.exe` が無い。
    - **`tools\replay_verify.bat` が通る** (シーンを書き換える経路が入ったので必ず回す)。
    - `tools\check_rules.ps1` が 0 error、`tools\check_rules_selftest.ps1` が PASS。`cargo test` (track、mcp)、`tools\track_verify.bat` が通る。
    - ABI・SimSnapshot・TypeId が変わっていない: `git diff ca6a3991 -- src\Shared src\Scripting\Interop.cs` が空。`git diff --stat ca6a3991 -- src\Engine` に出るのは `LocalizationTable.inl` と `HotReload\ReloadHub.*` だけ。
    - このリポジトリの作業ツリーに、復元による変更が無い (`git status`)。

## 検証コマンド

- `tools\build_track.bat Release` (サービスを変えたら。`mye_track.exe` が動いていないことを確かめてから)
- `MyEngine.sln` を `Debug|x64` と `Release|x64` でビルド (`/p:MyeWarnAsError=true`)
- `MYE_SELFTEST_ONLY=changetracking` (Debug)、`MYE_TRACK_PROBE=1`、`--selftest-full` (Release)、Debug の physics を除いた全スイート
- `tools\replay_verify.bat`
- `cargo test --manifest-path tools\track\Cargo.toml`、`tools\track_verify.bat`、`cargo test --manifest-path tools\mcp\Cargo.toml --target-dir cache\track_verify\mcp_target`
- `pwsh -NoProfile -File tools\check_rules.ps1`、`pwsh -File tools\check_rules_selftest.ps1`
- `git diff ca6a3991 -- src\Shared src\Scripting\Interop.cs`、`git diff --stat ca6a3991 -- src\Engine`、`git status --short`
- exe の実行は PowerShell から。試験は一時プロジェクトで行う。`tools\mcp\target\release\` の exe に触らない。ユーザーのプロセスを止めない。

## 実装メモ (coder が追記)

```
SELF_EVAL: sub-16 (round 1)
実装:
  - `src\Editor\ChangeTracking\SceneRestorePlan.h/.cpp` (新規。ImGui なし・World を書かない): `BuildSceneRestorePlan` (照合と計画)、`SceneReadView` (ライブの読み取り口。試験は偽の表を渡せる)、`CanonicalFieldJson` / `DefaultFieldJson` (D41 の正規形: 記録の値を**そのフィールドの型で読み**書き直した値と、ライブの値の正規形を比べる。EntityRef は fileId の整数、C# は managed の出力どうし)、`ClassifySceneRow` / `SelectSceneRows` (範囲: フィールド / コンポーネント / エンティティ / 変更全体。追加・削除されたエンティティの範囲には、同じ変更の子孫の行も入れる)。表 §3.3 の 6 単位 + name / tagMask / parent / childIndex を判定。1 行でも競合・戻せないなら ops は空。
  - `SceneRestoreRunner.h/.cpp` (新規): `LiveSceneReadView` (`EntityToJson` で読むだけ。fileId の無いエンティティに採番しない)、`ApplySceneRestore` (ゲートを直前に判定し直す → 計画を作り直してプレビューの ops と比べる → **`UndoStack::Record` の中で書く** (StructuralChanges::Apply、fids = 触るエンティティ。Field は `FieldFromJson` / EntityRef は fileId 解決 / C# は `DeserializeComponent` へ 1 キー、Entity・Component・name・親子は JSON を直して `SceneSerializer::ApplyPartial`、作る・消す) → 同じ行から計画を作り直し全部「復元済み」を確認 → 合わなければ Record の中で自分の書いた分を戻して Undo に積まない)。`BuildSceneRestoreRecord`。ディスクは書かない・ジャーナルは作らない。
  - `SceneRestoreFormat.h/.cpp` (新規): 文面 (英日) と画面の内容 (`SceneRestoreScreen` / `SceneRestoreResultView`)。
  - `src\Editor\Windows\Project\SceneRestoreWindow.h/.cpp` (新規): `SceneRestoreDialog` (プレビュー: 止める理由の全部・照合の結果・戻すと行う操作 (どのエンティティのどの値を何から何へ)・Undo と未保存の注記。実行: ゲート → `ApplySceneRestore` → `restore_record` をキューへ → 結果の画面)。EditorApp が持ち、`AiChangesWindow` は「シーンの変更を復元...」ボタンだけ (`DrawSceneRestoreButton`、開いていないシーンの行は灰色 + 「開くと項目ごとに戻せます」)。
  - `RestoreGate.h/.cpp`: `RestoreGateTarget::LiveScene` (Play・ネット・ミニシーン編集中だけ止める。ビルド・git・マージ・未保存・各窓の未保存は止めない) と `RestoreBlockKind::ExternalChangePending` / `NoScene`。`RestoreFormat.cpp`: 2 つの文面。
  - `RestoreRunner.h/.cpp`: `QueueRecord` (ジャーナルを作らない復元の記録を送り直しつきの順番待ちへ)、`SetOnRecorded`。`ChangeTrackingSession.cpp`: 記録が受け取られたら `ChangeModel::Refresh` と `SceneChangeOverlay::Refresh` (新設。記録は通知を出さない)。`ChangeModel.h/.cpp`: `TrackSceneChange::state`。`RestoreRunFormat.cpp`: 履歴の行にシーンの復元の印と件数。
  - `AssetRegistrySync.h/.cpp` (新規) と `EditorApp.cpp` の復元のフック: sub-27 から持ち越した「削除の後のアセットの登録」。**Editor 層に既存の経路があった** (`AssetDatabase::MoveAsset(path, path)` = Asset Browser の削除と同じ、`GuidForPath(path, true)` = git の `RegisterAdded` と同じ)。`EndBatch` のあとに削除・追加・名前の変更を反映する。Engine 層は触っていない。
  - サービス (`tools\track`): `restore_record` の `targets[]` に `scope = scene` と `sceneChangeIds` (4096 件まで) を足した (`ops_record.rs`)。`committed` で `scene_changes.state = 'restored'` (`restore.rs`。変更 (ファイル) の状態・レビューは変えない)。`restore_list` が `sceneChangeIds` を返す。**DB スキーマは変えていない** (`state` 列は版 1 から)。
  - `EditorApp.h/.cpp`: `sceneRestore_` と、`AiChangesHost::sceneRestoreEnv` / `openSceneRestore` (World・Undo・選択・C# のホストを 1 フレームだけ借りる)。`AiChangesWindow.h/.cpp`、`LocalizationTable.inl` (57 行)。
  - 試験: `SceneRestoreSelfTest.cpp` (changetracking スイートの (q)。`RunSceneRestoreProbe` は `MYE_TRACK_PROBE` から呼ぶ)、`tests\track\29_scene_restore.ndjson` と `.expected.ndjson`、`tools\track\tests\session.rs` (新規 1 本: 行の状態・scene_overlay・レビュー不変・DB 作り直し)。
  - 文書: restore-safety.md (§3.3 を開いているシーンだけ + 正規形に、§4.2 の C# の結果、§8.3 を新設)、ipc-contract.md、db-schema.md、architecture.md §7、engine_spec.md §9.2、test_checklists.md (M104q の節)、plans\m104-ai-change-tracking.md (進捗表)。
仕様との差分:
  - [逸脱] ミニシーン (ActorEdit) の編集中は、**対象がそのアセットでも止める** (sub は「対象が編集中の文書でなければ止める」)。ミニシーンの fileId と今のシーンの対応を保証できず、受け入れ条件 8 が「ミニシーンの編集中は止まる」とあるので、安全側に倒した。ライブのシーンの復元はシーンの文書だけが対象。
  - [逸脱] プレハブの上書きの印だけの行 (`PrefabOverrideChanged` で overrides の要素そのものが 1 側だけにある行) は**単独では戻さない**: 値の行を戻すと `UndoStack::Record` の `CaptureAfter` が記録を取り直すので連動して消える (derived)。印だけが残る行は「戻せない (OverrideKeyOnly)」。値の変更が無い印だけの切り替えは、記録を取り直すと戻る/戻らないが定まらないため。
  - [追加] `RestoreBlockKind::ExternalChangePending` / `NoScene` (取り決め 3 の「保留中は止める」を表すため。`GateBlocker` は GitTransaction の型なので触らない)。
  - [追加] `SceneRestoreDialog` を EditorApp の持ち物にし、`AiChangesHost` に `sceneRestoreEnv` / `openSceneRestore` を足した (sub は「AiChangesWindow.cpp に大きく足さない」)。結果の画面もこのダイアログの中 (RestoreRunWindow は触らず、履歴は `BuildHistoryRows` に印を足しただけ)。
  - [追加] 追加・削除されたエンティティを範囲に選ぶと、同じ変更の子孫の行も範囲に入る (入れないと「After に無い子孫」で止まる / 親だけ作って子を残す)。`SceneChangeOverlay::Refresh` と `RestoreRunner::QueueRecord` / `SetOnRecorded`、`TrackSceneChange::state`。
  - [追加] 参照の先 (`EntityRef` / 親) が「同じ復元で作り直すエンティティ」なら、いるものとして扱う。
  - [未実装] 開いていないシーンの項目ごとの復元 (取り決め 1 のとおり。窓は文面を出してボタンを灰色にする)。
  - [未実装] 条件 12 の「外部の変更を読み込む」操作そのもの (実 Editor の ReloadHub 経由)。結合の試験は、書き換え後のファイルの状態のライブのシーン (`Scene` を手で組む) を使い、サービスとのやり取り (記録 → 復元 → `restored` → 強調から消える → Undo → 履歴) は実サービスで確かめた。
検証:
  - ビルド: `MyEngine.sln` を Debug|x64 / Release|x64 で `/p:MyeWarnAsError=true` → 成功・警告 0 (プローブを外してから最後のビルド)。
  - Debug の `MYE_SELFTEST_ONLY=changetracking` → 全部 PASS (1039 PASS、新規 約 170。失敗 0)。`MYE_TRACK_PROBE=1` を付けて同じ → ALL PASS、実サービスで「外から書き換えたシーンの Field を戻す → 値が戻る → ファイルは 1 つも変わらない → `scene_changes` が `restored` → `scene_overlay` から消える → Undo で After → `restore_list` に scope scene」、終了後に `mye_track.exe` なし。
  - C# コンポーネント (実走。.NET 8.0.31 / MyeScripting.dll あり。SKIP なし): (a) `DeserializeComponent` に 1 キーの JSON で他のフィールドが変わらない ✓、(b) `Record` 経由の復元は Undo で戻り Redo でもう一度当たる ✓、(c) プレハブのインスタンスで復元しても上書きの記録は壊れず、保存して読み直しても同じ ✓ (C# のフィールドは `FieldDesc` が無いので、エンジンのライブ diff は C# の上書きをもともと記録しない = 既存の仕様。復元はそれを変えない)。→ C# も Field 単位で戻せる。
  - プレハブの上書き (実 Scene + PrefabLibrary): 上書き済みフィールドを戻すと overrides の記録が「空 (記録は残る)」になり、`SaveToJson` → `LoadFromJson` でも同じ。Undo で値と記録が一緒に戻る。
  - Release の `--selftest-full` → 125 スイート 0 失敗 (307 秒)。Debug は `--selftest-list` の fast / full の行から physics を除いた 124 スイート → 0 失敗 (902 秒)。どちらも実行中と後に `mye_track.exe` / `Editor.exe` の残りなし。
  - `tools\replay_verify.bat` → PASS (24 ジョブ 541 秒。17 シーンの Debug / Release 一貫性・スナップショットの往復・jobs A/B・compute A/B・タイムトラベル・規則検査。両構成をビルドし直した最後の状態で。実行後の `chcp` は 65001)。
  - `cargo test --manifest-path tools\track\Cargo.toml` → 全通過 (session 41 件、新規 1)。`tools\build_track.bat Release` → `tools\track_verify.bat` 全シナリオ PASS (`29_scene_restore` を含む)。`cargo test --manifest-path tools\mcp\Cargo.toml --target-dir cache\track_verify\mcp_target` → 58 件通過。
  - `pwsh -NoProfile -File tools\check_rules.ps1` → 0 error (警告 66 は既存)。`check_rules_selftest.ps1` → PASS。
  - `git diff ca6a3991 -- src\Shared src\Scripting\Interop.cs` → 空。`git diff --stat ca6a3991 -- src\Engine` → `LocalizationTable.inl`、`HotReload\ReloadHub.cpp/.h`、`ReloadHubSelfTest.cpp` (後 3 つは sub-13 のもの) だけ。ABI・SimSnapshot・TypeId・DB スキーマは変更なし。
  - スクショ (一時のプローブ + Release の `Editor.exe --frames 40 --shot-frame 36 --screenshot --lang ja|en`。プローブは外してビルドし直した): `C:\HAL\MyEngin\cache\m104q_shots\` の preview / conflict / blocked / notopen / result × ja / en の 10 枚。撮れた・文字が化けていない・ボタンが常に下に見える、は確認。見た目の合否はユーザーの目視 (未確認)。
  - 未実行: 実 Editor での GUI 操作 (押す・Ctrl+Z・保存・ヒエラルキーの更新)、`IsSceneDirty` が復元の後に真になること (Undo の `StateSerial` が変わることまでは試験)、外部の変更の保留中の実操作。
自己採点 (1-5):
  仕様適合: 4 — 14 条件に試験・実走・文書で対応し、C# の (a)(b)(c)・A20 の横断も回した。差分 (ミニシーン常時停止・印だけの行) は planner の裁定待ち。実 Editor の操作と目視は未確認
  正しさ: 4 — 競合で World が変わらない・Undo で戻る・読み直しの失敗で戻す・実サービスとの往復を試験した。実 Editor の ReloadHub / 保存との結合は未確認
  コード品質: 4 — 判定 (純関数) / 書き込み / 文面 / 画面を分け、既存の部品 (ゲート・キュー・Record) を共有。SceneRestorePlan.cpp が約 980 行と大きい
  テスト: 4 — 純関数・実 Scene・C#・プレハブ・ゲート・ダイアログ (偽サービス)・実サービス。実 Editor の GUI は無い
不安・質問:
  - (planner へ) ミニシーン編集中は対象がそのアセットでも止めた。アクターの文書の復元を許すなら、ミニシーンの fileId の対応を保証する別の試験が要る。このままでよいか。
  - (planner へ) 印だけの `PrefabOverrideChanged` 行を戻せない扱いにした (値の行と連動)。値の変更の無い上書きの切り替えを戻したい要望が出たら、`overrides` を直接書く経路 (CaptureAfter が上書きする) の再設計が要る。
  - エンティティの照合は `childIndex` と `overrides` を見ない (兄弟の並びと印は付随するため)。AI が兄弟の順序だけを変えた変更は `HierarchyChanged(childIndex)` として戻せるが、位置が兄弟数を超えると丸められて読み直しで失敗する (その場合は自分で戻して Undo に積まない)。
触ったファイル:
  - 新規: src\Editor\ChangeTracking\SceneRestorePlan.h / .cpp、SceneRestoreRunner.h / .cpp、SceneRestoreFormat.h / .cpp、AssetRegistrySync.h / .cpp、SceneRestoreSelfTest.cpp、src\Editor\Windows\Project\SceneRestoreWindow.h / .cpp、tests\track\29_scene_restore.ndjson、tests\track\29_scene_restore.expected.ndjson
  - 変更: src\Editor\App\EditorApp.h / .cpp、src\Editor\ChangeTracking\ChangeModel.h / .cpp、ChangeTrackingSelfTest.h / .cpp、ChangeTrackingSession.cpp、RestoreFormat.cpp、RestoreGate.h / .cpp、RestoreRunFormat.cpp、RestoreRunner.h / .cpp、SceneChangeOverlay.h / .cpp、src\Editor\Windows\Project\AiChangesWindow.h / .cpp、src\Engine\Core\Localization\LocalizationTable.inl (Engine 層で触ったのはここだけ)、tools\track\src\ops_record.rs / restore.rs、tools\track\tests\session.rs
  - 文書: docs\ai-change-tracking\restore-safety.md / ipc-contract.md / db-schema.md / architecture.md、docs\test_checklists.md、engine_spec.md、plans\m104-ai-change-tracking.md、plans\m104-ai-change-tracking\sub-16.md (実装メモ)
  - 生成物 (gen_project_files.ps1。ステージは要る): build\Editor.vcxproj、build\Editor.vcxproj.filters
消した・止めたもの:
  - 消した: src\Editor\ChangeTracking\SceneRestoreShotProbe.h (スクショ用の自分の一時プローブ。EditorApp の 2 行も外し、SHOTPROBE / SceneRestoreShotProbe の残りがないことを src と build で確認)。自分が作った試験用の一時ディレクトリ (%TEMP%\mye_scene_restore_*、mye_track_probe_* は試験が自分で消す。残りなし)。
  - 止めた: 自分が起動した grep 1 本 (誤って tools 全体を走査した。pid を確認してから止めた)。試験が起動した mye_track.exe は試験が自分で止めた (実行後に残りなし)。ユーザーの mye_mcp.exe には触れていない。
  - 触れていない: %TEMP%\mye_track_*、%TEMP%\mye_m104b\、cache\m104*_shots\ の既存分、tools\mcp\target\release\、.claude\settings.json、.mcp.json、C:\HAL\MyEngin\.mye (作っていない)。ファイルは 1 つも削除していない (自分の一時プローブ以外)。
申し送り:
  - sub-17〜19: ゴースト・Overlay (sub-17) は、削除されたエンティティの Before の JSON を `scene_changes.before_value` から取れる (`TrackSceneChange::beforeValue`)。復元の `state = restored` の行は強調から消える (`SceneChangeOverlay::Refresh` で取り直す)。
  - sub-19 (プレハブ伝播): `PrefabPropagation` は `ClassifySceneRow` で `Unsupported` に落ちる (RowUnsupported の文面)。原因の変更へ案内する文面は未実装。
  - `RestoreRunner::QueueRecord` は、ジャーナルを持たない復元の記録の口 (切断中は溜めて送り直す)。
```

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-15)。確かめたこと: `mye_track` / `Editor` の残存プロセスなし、リポジトリに `.mye\ai_changes\` なし、基点からの `src\Engine` の差分は `LocalizationTable.inl` と `HotReload\ReloadHub.*` (sub-13) だけ、`src\Shared` / `Interop.cs` / Runtime / Server / GameLogic・スキーマ・`tools\mcp`・`.claude`・`.mcp.json` に差分なし、プローブの残りなし。**`src\Editor\ChangeTracking\` と `SceneRestoreWindow.cpp` に、ファイルを書く API・`SaveToFile` の呼び出しは `RestoreWriteIo.cpp` の外に 0 件** (シーンの復元はディスクを書かない)。`SceneRestoreRunner.cpp` は 288 行目で `env.undo->Record(…, StructuralChanges…)` の中で書き、Field は `FieldFromJson` (123 行目)、C# は `DeserializeComponent` に 1 キーの JSON (103 行目)、エンティティは `ApplyPartial`。`AssetRegistrySync.cpp` は Editor 層の既存の `AssetDatabase::MoveAsset` / `GuidForPath` を呼ぶだけ。planner の実走: Debug の `MYE_SELFTEST_ONLY=changetracking` が終了コード 0・FAIL 0・C# の SKIP なし、Release の `--selftest` 全体が 112 スイート 0 失敗 (実行中も後も `mye_track.exe` なし)、`cargo test` (track) 286 件通過、`tools\track_verify.bat` 全シナリオ PASS (29_scene_restore を含む)、`tools\check_rules.ps1` 0 error。**`tools\replay_verify.bat`、Release の `--selftest-full`、Debug の physics を除いた 124 スイート、`MYE_TRACK_PROBE=1`、スクショ 10 枚は coder の結果による** (planner は再実行していない)。
  - 逸脱の判定: ミニシーンの編集中は対象がそのアセットでも止める (安全側。受け入れ条件 8 と一致) は採る。印だけのプレハブ上書きの行を単独では戻さない (値の行を戻すと `CaptureAfter` で連動して消える) は採る。
  - 追加の判定: `ExternalChangePending` / `NoScene` の止める理由、`SceneRestoreDialog` を EditorApp の持ち物にした、子孫の行を範囲に含める、同じ復元で作り直す先を「いる」と扱う、`AssetRegistrySync`、`scope = scene` の記録、は全部採る。
  - C# コンポーネントの確認 (a)(b)(c) が実走で成り立った (SKIP なし) ので、C# も Field 単位で戻せる。sub-01 の試作 4 の最後の「未検証」が解消した。
  - 質問への回答: (1) ミニシーンの編集中は止めるままでよい。アクターの文書は、ファイルか Hunk の復元で戻せる。(2) 印だけの上書きの行を戻せない扱いでよい。要望が出たら別件。(3) `childIndex` の丸めで読み直しに失敗したら自分で戻して Undo に積まない、の動きでよい (試験済み)。
  - 未確認: 実 Editor の操作 (押す・Ctrl+Z・保存・Hierarchy の更新)、復元の後に `IsSceneDirty` が真になること (Undo の `StateSerial` が変わることまでは試験)、外部の変更の保留中の実操作、スクショの見た目。