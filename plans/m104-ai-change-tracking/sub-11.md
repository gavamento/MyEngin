# sub-11: シーン構造 diff (M104l)

- 依存: sub-23 (順序は sub-10 → sub-23 → sub-11)
- 状態: 未着手
- 往復: 0
- 書き込みの深さ: **詳細** (2026-11-15、sub-23 の VERDICT で planner が書き直した)
- 質問の運用: 推奨で進め、Notion に後回し。着手前の確認は無い。

`C:\HAL\MyEngin\docs\ai-change-tracking\architecture.md` §6 (arch)、`db-schema.md` §3 の `documents` / `scene_changes` (db)、要求 §7。**このサブはサービス側 (`tools\track`) だけ。** Editor の表示は sub-12。`src\` は触らない (ただし D の文書の直しを除く)。

## 確認済みの事実 (2026-11-15 に planner が実ファイルで確かめた。着手時に coder がコードでも確かめる)

- シーンは `*.scene.json` (例 `assets\scenes\scene_b.scene.json`)。ルートは arch §6.3 の `{engine, version, sceneName, nextFileId, entities}`。
- プレハブは `*.prefab.json`。ルートは `{"prefab": 1, "name": ..., "entities": [...]}` (シーンとキーが違う。`version` ではなく `prefab` が版)。アクター (`*.actor.json`) の形は着手時に確かめる。
- `.meta` は `<アセットのファイル名>.meta` (例 `scene_b.scene.json.meta`) で、`{"guid": "7d4d8953b427201b", "type": "scene", "version": 1}`。guid は 16 桁の 16 進。
- エンティティは `{fileId, name, parent?, childIndex, tagMask?, components, overrides?}`。`components` はコンポーネント名をキーにしたオブジェクト。
- スキーマは凍結済み (sub-05)。`documents` と `scene_changes` は v1 にある。今の版は 2 (sub-23 が索引を足した)。列が足りなければ移行 v3 を足す。

## やること

### A. 文書の同一性 (`document.rs`、arch §6.1)

1. 対象の判定: 拡張子で `scene` / `prefab` / `actor` を決める (`changes.kind` と `documents.kind`)。それ以外の JSON はただのテキスト。
2. `documentId` = そのファイルの `.meta` の `guid`。サービスが `.meta` を自分で読む (Editor を経由しない)。`.meta` が無い・読めない・`guid` が 16 桁の 16 進でないときは、正規化した相対パスの SHA-256 の先頭 16 桁を仮の ID にして `id_source = path`。
3. 後で `.meta` が付いたら、仮の ID の文書を、guid の文書として**引き継ぐ** (同じパスで `id_source = path` → `meta`)。`documents` の主キーが変わるので、引き継ぎの表し方 (古い行に「引き継ぎ先」を書く / `scene_changes` の `document_id` を書き換える) を決めて db に書く。履歴の行を消さない。
4. `.meta` ごと動いたリネーム・移動 (guid が同じ) は同じ文書で、`current_path` を更新する。
5. エディタの外でリネームされて guid が変わった (新しいパスに新しい guid の `.meta` ができた、または `.meta` が無くなった) ら、**別の文書として扱い、古い文書に `broken_at` / `broken_note` を記録する**。内容が似ていても同じ文書と決めない。
6. `.meta` の変更そのものも、今までどおり 1 つのテキストの変更として記録される (`kind = meta` にする)。guid が変わった `.meta` の変更は、5 の判定の引き金になる。
7. 文書の同一性も、events と Blob から作り直せる派生の情報にする (spec §2 D26。`rebuild_derived` を延ばす。作り直しで同じ `document_id` になること)。観測のイベントに、その時点の guid (または「無い」) を入れる必要があれば、ペイロードに足す (ペイロードの項目の追加はスキーマの変更ではない。db §5.1 に書く)。

### B. 構造比較 (`scene_diff.rs`、arch §6.3、§6.4)

純粋な関数にする: (Before のバイト列, After のバイト列, 種類) → 構造の変更の列、または「解析不可」と理由。

1. **数値は字句のまま比べる。** JSON を数値型に読むと字句が失われる (`0.1` と `0.10000000149011612`、`1.0` と `1`)。字句を保つ読み方をする。手段は 2 つで、coder が選ぶ: (a) `serde_json` の `arbitrary_precision` と `preserve_order` の feature (依存は増えないが、`Cargo.toml` の feature が変わり、crate 全体の `Value` の振る舞いが変わる。既存の試験が全部通ることを確かめる) (b) 構造比較のための小さい字句保持の読み手を自前で書く。どちらでも、**比べるのは「値の正規形」**: オブジェクトはキーの集合として (順序を無視)、配列は順序つき、文字列は復号した内容、数値は字句、`true` / `false` / `null`。空白と改行は無視。
2. エンティティは `fileId` で対応付ける。配列の中の位置では対応付けない。`fileId` が無い・重複している・数値でないエンティティがある文書は「解析不可」。
3. 版: シーンは `version`、プレハブは `prefab`。Before と After で違う、または知らない値 (知っている値は定数の表。着手時に `Scene::kDocVersion` (現行 4) とプレハブの版をコードで確かめる) なら「解析不可」。
4. 変更の種別 (要求 §7.2 の 11 種のうち `PrefabPropagation` 以外):
   - `EntityAdded` / `EntityRemoved`: `fileId` の増減。プレハブのインスタンスの子の増減もこれ。
   - `HierarchyChanged`: `parent` か `childIndex` の変化。
   - `ComponentAdded` / `ComponentRemoved`: `components` のキーの増減。
   - `FieldModified`: コンポーネントの中の値の変化。`field` は最上位のキー (例 `position`)。入れ子の中だけが変わっても、最上位のキー 1 つの変更として出し、`before_value` / `after_value` はそのキーの値全体の字句 (元のバイト列から切り出した JSON の断片。sub-16 がそのまま書き戻せるように)。
   - `EntityModified`: `name` / `tagMask` など、コンポーネントでもヒエラルキーでもないエンティティ直下の変化 (`field` に名前を入れる)。
   - `PrefabAssetChanged`: プレハブ / アクターのファイル自体の変更 (文書全体として 1 件。中の構造の変更も上の種別で出す)。
   - `PrefabInstanceChanged`: シーン側で `PrefabInstanceComponent` の `prefabHash` が変わった。
   - `PrefabOverrideChanged`: `overrides` のキーの集合が変わった、または override 済みのフィールド (`"Comp.field"` / `"name"` / `"tagMask"`) の値が変わった。構造の上書き (`"+Comp"` / `"-Comp"`) の増減もこれ。**override 済みのフィールドの値の変化は `FieldModified` ではなく `PrefabOverrideChanged` として出す** (二重に出さない)。
5. ルート直下の変化 (`sceneName`、`nextFileId` など) は、`file_id = NULL` の文書全体の変更として 1 件ずつ (種別は `EntityModified` を使い回さず、文書の変更と分かる形にする。11 種に無いので、`scene_changes.change_type` に `DocumentModified` を足すか、`component = NULL` / `file_id = NULL` の `FieldModified` にするかを決めて db に書く。推奨は後者)。`nextFileId` だけの変化は、エンティティの追加に伴う付随の変化として、単独では出さなくてよい (出すなら注記)。
6. C# コンポーネントのフィールドは、JSON の中で managed の `Serialize` の形で入っている。サービスは C# かどうかを知らなくてよい (一様に扱う)。
7. 大きさの上限: 構造比較の対象は、行 diff と同じ上限 (8 MiB) まで。超えたら「解析不可 (大きすぎる)」。エンティティ数万のシーンで Diff ワーカーが止まらないこと (計算量は入力の大きさにほぼ比例)。
8. 壊れた JSON、空のファイル、`entities` が無い・配列でない、は「解析不可」で、サービスは落ちない。**解析不可でも、ファイル単位の Revision と行 diff は今までどおり残る。**

### C. 記録と op

1. Diff ワーカー (`diff_worker.rs`) に、種類が scene / prefab / actor のときの分岐を足す。行 diff は今までどおり作り、構造の変更を追加で作る。Revision の確定 (`write_revision`) と同じ SAVEPOINT で `scene_changes` を書く (構造の変更の無い Revision が外から見える瞬間を作らない)。
2. 「解析不可」は Revision に残す。置き場所は、`scene_changes` に `change_type = 'unparsed'` の 1 行 (理由を `after_value` か専用の表し方で) か、移行で列を足すか。推奨は前者 (列を足さない)。決めて db に書く。
3. op `scene_diff` (`revisionId`、`cursor?`、`role = editor` だけ): 構造の変更の配列 (`sceneChangeId`、`documentId`、`fileId`、`changeType`、`component`、`field`、`beforeValue`、`afterValue`、`state`)。1 行の上限に収まるようページ送り (値が大きいものは切らずに、ページの区切りで調整。単独で超える値は `oversize` と大きさ)。
4. `describe` に「シーン変更の要約」を足す (種別ごとの件数、`documentId`、解析不可ならその理由)。`mcp` にも返す (MCP の `change_describe` がそのまま通す。`tools\mcp\src` は変えない。MCP 側の 64 KiB の打ち切りの中に収まる)。
5. `list` の `changeType` フィルタは、ファイル単位の種別 (added / modified / …) のまま。構造の種別では絞らない (必要になったら別の引数)。
6. 作り直し (`rebuild_derived`): `documents` と `scene_changes` も events と Blob から同じ結果に戻る。Blob が欠けた Revision は構造の変更なし (行だけ戻る)。
7. 通知: 既存の `change_recorded` に、`documentId` (シーン / プレハブのとき) を足す (Editor が、開いているシーンの変更かどうかをすぐ判定できるように。項目の追加で、プロトコル版は上げない)。

### D. 文書と持ち越し

- arch §6、db (`documents` の引き継ぎ、「解析不可」の表し方、ルート直下の変化)、`C:\HAL\MyEngin\docs\ai-change-tracking\ipc-contract.md` (`scene_diff`、`describe`、通知)。
- **持ち越し**: `ipc-contract.md` に、前のサブから制御文字 (タブと `\x13`) が残っている箇所がある (例: `tools<TAB>rack\src\notify.rs`。planner が 1 行以上あることを確認)。このサブで直す。直した後、`docs\ai-change-tracking\*.md` と `plans\m104-ai-change-tracking.md` に、タブ以外の制御文字と、パスの中の不自然なタブが残っていないことを機械で確かめる。
- `C:\HAL\MyEngin\plans\m104-ai-change-tracking.md` の進捗表。

## やらないこと (このサブでは)

- `PrefabPropagation` の計算 (sub-19)。Editor の表示 (sub-12)。Editor Snapshot (`editor_snapshot` / `blob_put`) の受け口 (使うサブが決まってから。sub-16 か sub-18 の見直しで決める)。
- `SceneSerializer` を呼ぶこと (サービスは Engine の部品を使えない)。
- `src\`、`tools\mcp\src` の変更。`schema\v1.sql` / `v2.sql` の変更 (足すなら v3)。
- 作業ツリーへの書き込み。

## 試験の入力

- リポジトリの実シーン・実プレハブから切り出した小さい固定ファイルを `C:\HAL\MyEngin\tests\track\fixture\` に置く (元のファイルを書き換えない。コピーして縮める)。改行は `.gitattributes` に従う (LF)。字句の比較の試験のために、数値の書き方を手で変えた版も置く。
- 純粋な関数 (`scene_diff`) の単体試験と、サービスを通した結合の試験 (`tests\scene_diff.rs`) と、pipe のシナリオ (`tests\track\`) の 3 層。

## 受け入れ条件 (このサブ)

1. Transform の 1 フィールドを変えたシーンで、`FieldModified` が (documentId, fileId, component, field) と、字句のままの Before / After 付きで 1 件だけ出る → `cargo test` (要求 §16.3 の 1 番目)。
2. キーの順序、空白、改行、エンティティの配列の並び (`childIndex` はそのまま) だけを変えたファイルは、構造の変更が 0 件 (行 diff は出る) → `cargo test` (要求 §7.3)。
3. `0.1` と `0.10000000149011612`、`1.0` と `1`、`1e0` と `1` は別の値として出る。同じ字句は変更にならない。文字列の `"\u0041"` と `"A"` は同じ → `cargo test`。
4. 2 つのシーンの同じ `fileId` への変更が、別の `document_id` の行として保存され、`scene_diff` で混ざらない → `cargo test` (§16.3 の 2 番目)。
5. 版が Before と After で違う、知らない版、壊れた JSON、空のファイル、`entities` が無い、`fileId` の重複、上限を超える大きさ、のそれぞれが「解析不可」で、理由が分かり、ファイル単位の Revision と行 diff は残り、サービスは落ちない → `cargo test`。
6. エンティティの追加・削除、親の付け替え、`childIndex` だけの変化、コンポーネントの増減、`name` の変化が、それぞれの種別で 1 件ずつ出る。1 つのフィールドの入れ子の中だけを変えると、最上位のキー 1 つの `FieldModified` → `cargo test`。
7. プレハブ: `overrides` の集合の変化と override 済みフィールドの値の変化が `PrefabOverrideChanged` (同じ変化が `FieldModified` として二重に出ない)、構造の上書き `"+Comp"` / `"-Comp"` の増減も `PrefabOverrideChanged`、`prefabHash` の変化が `PrefabInstanceChanged`、プレハブのファイルの変化が `PrefabAssetChanged` + 中の構造の変更 → `cargo test` (§16.3 の 4 番目)。
8. 文書の同一性: (a) `.meta` ごと動かしたリネームは同じ `document_id` で `current_path` が変わる (b) `.meta` の guid が変わったリネームは別の文書で、古い文書に断絶が記録される (c) `.meta` が無いファイルは仮の ID、後で `.meta` が付くと引き継がれ、それ以前の `scene_changes` をたどれる → `cargo test`。
9. `before_value` / `after_value` が、元のファイルのバイト列から切り出した JSON の断片で、そのまま JSON として読め、元の字句と一致する → `cargo test`。
10. `scene_diff` がページ送りでき、`mcp` / `hook` では `forbidden`。`describe` に要約が出る。`change_recorded` に `documentId` が付く → `cargo test` + `tools\track_verify.bat`。
11. 作り直し: 記録の後に `tracking.db` を壊して起動し直すと、`documents` と `scene_changes` が壊す前と同じ → `cargo test`。
12. エンティティ 2 万個のシーン (生成) の構造比較が、release で数秒以内に終わり、Diff ワーカーが他の変更の記録を止め続けない。所要時間を `C:\HAL\MyEngin\docs\ai-change-tracking\performance.md` に書く → `mye_track bench` に足すか、`#[ignore]` の計測試験。
13. Phase 1〜2 の試験とシナリオが全部通る (全体の `cargo test` を 3 回、`tools\track_verify.bat` を 3 回。後に `mye_track.exe` が残らない)。`serde_json` の feature を変えた場合は、変える前後で既存のシナリオの期待値が変わらないこと (キーの順序が変わって期待値が動くなら、その差を説明する)。作業ツリーを書かない検査がこのサブの試験でも成り立つ。
14. `tools\check_rules.ps1` が 0 error。`git diff --stat HEAD -- src tools\mcp` が空 (空なら Editor のビルドと selftest は回さない。回していないことを書く)。D の制御文字の確認の結果。

## 検証コマンド

- `cargo test --manifest-path tools\track\Cargo.toml` (3 回)
- `tools\build_track.bat Release`、`tools\track_verify.bat` (3 回)
- 計測のコマンド (文書に書いたもの)
- `pwsh -NoProfile -File tools\check_rules.ps1`
- `git diff --stat HEAD -- src tools\mcp`

## 実装メモ (coder が追記)

## フィードバック履歴
