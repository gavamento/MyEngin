# sub-11: シーン構造 diff (M104l)

- 依存: sub-23 (順序は sub-10 → sub-23 → sub-11)
- 状態: OK (commit は台帳 harness.md を参照)
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

SELF_EVAL: sub-11 (round 1)
実装:
  - tools/track/src/lexjson.rs (新規) — 字句を保つ JSON の読み手 (数値は字句、各値にバイト範囲、キー重複は失敗、深さ 200 まで)。(b) の自前を選択。serde_json の feature は変えていない
  - tools/track/src/scene_diff.rs (新規) — 純粋な構造比較 (11 種のうち PrefabPropagation 以外 + unparsed)。fileId の HashMap で対応付け、同じ字句は比較せず通す
  - tools/track/src/document.rs (新規) — .meta の guid / パスの仮 ID / 結び付け (引き継ぎ・断絶・リネーム) / resolve
  - tools/track/src/revision.rs — RevisionInput.doc_guid、DiffPlan.scene / doc_guid、plan_diff が構造比較、write_revision が documents / scene_changes を同じ SAVEPOINT で書く、changes.kind を拡張子で、rebuild_derived が documents / scene_changes も作り直す
  - tools/track/src/diff_worker.rs / collector.rs / observe.rs — ワーカーが作業ツリーの .meta を読み、payload に doc_guid を残す (作り直しが同じ document_id になる)
  - tools/track/src/ops.rs / protocol.rs / server.rs — op scene_diff (editor だけ、ページ送り、oversize)、describe の sceneSummary
  - tools/track/src/notify.rs — change_recorded に documentId (シーン等のときだけ)
  - tools/track/src/schema/v3.sql, mod.rs — 版 3: documents.successor_id と索引 2 本
  - tools/track/tests/scene_diff.rs (新規 13 + 計測 2 の ignore)、tests/pipe.rs (scene_diff の結合 1)、tests/storage.rs (索引数 12)、tests/track/25_scene_diff.ndjson (+expected)、tests/track/fixture/ (実シーン・プレハブ・アクターの切り出し)
  - docs/ai-change-tracking/{ipc-contract,db-schema,architecture,performance}.md、plans/m104-ai-change-tracking.md (進捗表)。ipc-contract.md の制御文字 (TAB と DC3) と、`\n` に化けて途中で改行されていた notify.rs のパスを直した
仕様との差分:
  - [追加] DB スキーマ版 3 (documents.successor_id、idx_documents_current_path、idx_scene_changes_revision) — 引き継ぎの表し方として「古い行に引き継ぎ先を書く」を選んだため (履歴の行を書き換えない)。v1/v2.sql は不変
  - [追加] describe / scene_diff の結果に resolvedDocumentId、describe に sceneSummary (構造の変更が有る Revision だけ。無い Revision の応答は従来のまま) — sub 本文の「要約」と「引き継ぎをたどれる」の具体化
  - [追加] 種別 unparsed は scene_changes.change_type の 1 行、理由は after_value に JSON 文字列 (推奨どおり、列は足さない)。ルート直下の変化は file_id = component = NULL の FieldModified (推奨どおり、DocumentModified は足さない)。nextFileId は出さない (注記済み)
  - [追加] 追加 (Before 無し)・削除 (After 無し) の文書は、シーンは構造の変更なし、プレハブ/アクターは PrefabAssetChanged 1 件 — 仕様に無い場合の最小解釈
  - [追加] HierarchyChanged は parent / childIndex ごとに 1 件 (親付け替えで childIndex も変われば 2 件) — 仕様は「parent か childIndex の変化」で件数の規定が無い
  - [追加] PrefabAssetChanged は中の変更が 1 件以上のときだけ (空白だけの変更では出さない。受け入れ条件 2 と整合)
  - [追加] 構造の上限は DiffLimits.max_bytes / 2 (行 diff の合計上限の半分 = 既定 8 MiB)
  - [追加] 版の扱い: シーンは version 1〜4 (無ければ 1、SceneSerializer の value("version", 1) に合わせた)、プレハブ/アクターは prefab / actor = 1 (どちらのキーでも可、前後でキーが違えば解析不可)
  - [追加] changes.kind を拡張子で scene / prefab / actor / meta にする (従来は text / binary)。actor は db の列コメントに無かった値
  - [追加] PrefabOverrideChanged の集合の増減は component / field を分割 (Comp.field → (Comp, field)、+Comp → (Comp, "+"))、値は要素の文字列リテラル
  - [未実装] Blob が読めない・バイナリ・除外パスの Revision の unparsed 行 (行だけ戻す、の仕様どおり構造の変更も unparsed も無し)
  - [逸脱] 受け入れ条件 12 は #[ignore] の計測試験 2 本 (純粋な関数 / Tracker 経由) で、mye_track bench には足していない (sub が「or」と許容)
検証:
  - cargo test (tools\track、--no-fail-fast) 3 回 → 3 回とも 236 passed / 0 failed / 3 ignored (前は 206 件。うち ignored は計測 2 本 + 既存 1)
  - tools\build_track.bat Release → 成功。tools\track_verify.bat 3 回 → 3 回とも 25 シナリオ全 PASS (新規 25_scene_diff を --update で作成し、期待値の中身を読んで確認)、終了後に mye_track.exe 無し、C:\HAL\MyEngin\.mye 無し
  - cargo test --release --test scene_diff -- --ignored --nocapture --test-threads=1 → 2 万エンティティ (4.87 MB) の構造比較 162.9 ms、Tracker 経由で Revision 確定 540 ms (同時に書いた小ファイルも同じ 539 ms)。1 回の計測、performance.md §9
  - pwsh -NoProfile -File tools\check_rules.ps1 → 0 error / 61 warning (私が足した警告は無し。performance.md:111 は既存)。途中、早い段階の編集で CRLF 化した 6 ファイルを規則 14-c が検出したので LF に戻した
  - git diff --stat HEAD -- src tools\mcp → 空。したがって Editor のビルドと selftest は回していない (planner の指示どおり)
  - D の制御文字: docs\ai-change-tracking\*.md と plans\m104-ai-change-tracking.md を機械で走査 → 修正前は ipc-contract.md の 2 行 (TAB 4 個・DC3 1 個、うち `\notify.rs` が改行に化けて 2 行に割れていた) → 修正後はタブ以外の制御文字 0、タブ 0、U+FFFD 0
  - 未実行: Editor 実機での表示 (sub-12)、Rust 1.89 でのビルド、CI
自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 1〜14 に対応する試験・計測・検査は全部回した。差分欄の解釈の追加が多い (planner の判断待ち)
  正しさ: 4 — 実ファイル切り出し・生成入力・pipe・DB 作り直しの比較で確認。.meta をディスクから読む時点の競合 (読んだ後に動く) は次の .meta の観測で見直す設計だが、全順序は試験していない
  コード品質: 4 — 純粋関数と書く 1 箇所を分け既存の経路 (plan_diff / write_revision) に乗せた。bind がやや長い
  テスト: 4 — 単体 + 結合 + pipe + シナリオ + 作り直し。リネームの `.meta` / 本体の観測順の全組み合わせと、実 Editor は未試験
不安・質問:
  - 仮の ID から guid への「引き継ぎ」は、同じパスのときだけ (リネームを伴う新規 guid は断絶扱い)。ユーザーの意図 (「後で .meta が付いたら引き継ぐ」) の範囲と読んだが、planner に確認したい
  - 空白だけの変更の PrefabAssetChanged を出さない解釈 (差分欄)。sub-12 の表示で「プレハブが変わった」を行 diff だけで出すなら問題ない
  - build_track.bat の出力に、文字化けしたエラー行 (「...は、内部コマンドまたは外部コマンド...」相当の cmd のメッセージ) が 1 行出る。ビルドと copy は成功しており、このサブの変更とは無関係に見える (未調査)
触ったファイル:
  tools/track/src/{lexjson,scene_diff,document}.rs (新規)、tools/track/src/schema/v3.sql (新規)、tools/track/src/{revision,diff_worker,collector,observe,ops,protocol,server,notify,lib}.rs、tools/track/src/schema/mod.rs、tools/track/tests/scene_diff.rs (新規)、tools/track/tests/{pipe,storage}.rs、tests/track/25_scene_diff.ndjson / 25_scene_diff.expected.ndjson (新規)、tests/track/fixture/ (6 ファイル、新規)、docs/ai-change-tracking/{ipc-contract,db-schema,architecture,performance}.md、plans/m104-ai-change-tracking.md、plans/m104-ai-change-tracking/sub-11.md (実装メモ)
  消した・止めたもの: %TEMP%\mye_track_sd_unparsed_22204 / _41088 (私の試験が失敗したとき残した一時ディレクトリ 2 個。既存の mye_track_* と mye_m104b は触っていない)。止めたプロセスは無い (試験・シナリオが起動した mye_track.exe は自分で終了し、最後に残っていないことを確認)。
申し送り:
  - sub-12 (Editor 表示) は scene_diff / describe.sceneSummary / change_recorded.documentId を使える。構造の値は JSON の断片。unparsed は afterValue が理由の JSON 文字列
  - sub-16 (復元) は before_value / after_value をそのまま書き戻せる。引き継ぎは resolvedDocumentId
  - sub-19 (PrefabPropagation) は scene_changes.caused_by を使える (列は既存)。プレハブの変更の Revision は PrefabAssetChanged の行を持つ
  - v1.sql の先頭コメントに docs\ai の `\a` (BEL) が混じっている疑い (`docsi-change-tracking` と見える)。凍結ファイルなので触っていない
  - 作業ツリーが CRLF の Windows で Python から text モードで書くと CRLF 化する (規則 14-c が検出)。直した

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-15)。確かめたこと: `mye_track` / `cargo` の残存プロセスなし、リポジトリに `.mye\ai_changes\` なし、`src` と `tools\mcp` に差分なし、`schema\v1.sql` / `v2.sql` / `Cargo.toml` に差分なし (字句保持の読み手は自前で、`serde_json` の feature を変えていない)、`docs\ai-change-tracking\*.md` と計画に制御文字 0。planner の実走: `cargo test` (track) 236 件通過、`tools\track_verify.bat` 全シナリオ PASS (25_scene_diff を含む)。計測 (2 万エンティティで 162.9 ms) は coder の 1 回の結果による。
  - 追加の判定: スキーマ版 3 (`documents.successor_id` と索引 2 本)、`resolvedDocumentId`、`sceneSummary`、`unparsed` の表し方、ルート直下の変化の表し方、追加・削除の文書の最小解釈、`HierarchyChanged` を parent / childIndex ごとに 1 件、中の変更が無ければ `PrefabAssetChanged` を出さない、上限、版の扱い、`changes.kind` の値、override の component / field の分け方、は全部採る。
  - 質問への回答: (1) 仮の ID から guid への引き継ぎは同じパスのときだけ、でよい (リネームを伴って別の guid が付いたら断絶)。(2) 空白だけの変更でプレハブの `PrefabAssetChanged` を出さない、でよい。(3) `build_track.bat` の化けたエラー行は、ビルドとコピーが成功している限り追わない (別件)。
  - nit: `tools\track\src\schema\v1.sql` の先頭コメントに BEL (0x07) が混じっている。凍結したファイルで、SQL のコメントの中なので動作に関係しない。直さない (直すと「凍結」の意味が曖昧になる)。
  - 未確認: `.meta` と本体の観測の順の全組み合わせ、実 Editor での表示 (sub-12)。