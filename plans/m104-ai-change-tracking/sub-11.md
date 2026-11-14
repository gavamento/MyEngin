# sub-11: シーン構造 diff (M104l)

- 依存: sub-10
- 状態: 未着手
- 往復: 0
- 書き込みの深さ: **骨子** (spec §6.1)。

## やること

arch §6 (サービス側)。Editor なしで決まること。

1. `documents` 表: `documentId` は `<asset>.meta` の `guid`。サービスが `.meta` を自分で読む。`.meta` が無ければ正規化パスのハッシュを仮の ID にして `id_source = path`、後で `.meta` が付いたら引き継ぐ。エディタ外のリネームで guid が変わったら別の文書とし、旧文書に `broken_at` / `broken_note` を記録する (似ているからといってつながない)。
2. 対象の判定: シーン、`.prefab.json`、`.actor.json`。着手時に、実際の拡張子とルートの形をコード (`C:\HAL\MyEngin\src\Engine\Engine\Scene\SceneSerializer.cpp`、プレハブのローダ) とリポジトリの実ファイルで確かめ直す。
3. 構造比較 (arch §6.3): `fileId` で対応付ける、キーの並び順は無視、**数値は字句のまま**比べる (JSON を数値型へ読むと字句が失われる。字句を保つ読み方をする。`serde_json` の `arbitrary_precision` か自前の字句保持かは着手時に決め、`tools\track` の依存に閉じる)、`version` が違う / 未知なら「解析不可」でファイル単位の変更だけ残す。
4. 変更種別: 要求 §7.2 の 11 種のうち `PrefabPropagation` 以外。プレハブは arch §6.4 の表 (`PrefabAssetChanged` / `PrefabInstanceChanged` / `PrefabOverrideChanged`)。子エンティティの増減は `EntityAdded` / `EntityRemoved`。
   - sub-01 の試作 4 (2026-11-14 の見直し): C# コンポーネントのフィールドは、シーンの JSON の中で managed の `Serialize` が出す形 (10 種類の型。数値か数値の配列) で入っている。構造比較は JSON として一様に扱えばよく、C# かどうかをサービスが知る必要は無い。`FieldModified` の `before_value` / `after_value` はその字句のまま持つ (sub-16 が `DeserializeComponent` へ渡す)。
5. `scene_changes` への保存と、op `scene_diff`。`describe` の「シーン変更の要約」。
6. Editor Snapshot (`editor_snapshot`、`blob_put`) のサービス側の受け口。Editor 側の送信は sub-12 か sub-16 (見直しで確定)。

## やらないこと (このサブでは)

- `PrefabPropagation` の計算 (sub-19)。Editor の表示 (sub-12)。
- `SceneSerializer` を呼ぶこと (サービスは Engine の部品を使えない)。

## 触る場所 (planner の見立て)

- `C:\HAL\MyEngin\tools\track\src\` に `document.rs`、`scene_diff.rs`、`tests\scene_diff.rs`、`C:\HAL\MyEngin\tests\track\`
- テストの入力は、リポジトリの実シーンから切り出した小さい固定ファイルを `tests\track\fixture\` に置く
- `C:\HAL\MyEngin\docs\ai-change-tracking\architecture.md` §6

## 受け入れ条件 (このサブ)

1. Transform の 1 フィールドを変えたシーンで、`FieldModified` が (documentId, fileId, component, field) と字句の Before / After 付きで 1 件出る → `cargo test` (要求 §16.3 の 1 番目)。
2. キーの順序とエンティティの配列の順序だけを変えたファイルは、構造の変更が 0 件 (`childIndex` が変わっていない場合) → `cargo test`。
3. `0.1` と `0.10000000149011612` は別の値として出る。`1.0` と `1` も別 → `cargo test`。
4. 2 つのシーンの同じ `fileId` への変更が、別の `document_id` の行として保存され、`scene_diff` で混ざらない → `cargo test` (§16.3 の 2 番目)。
5. `version` が違う組は「解析不可」で、ファイル単位の Revision と行 diff は残る → `cargo test`。
6. エンティティの追加・削除・親の付け替え・コンポーネントの増減が、それぞれの種別で出る → `cargo test`。
7. `overrides` の集合の変化と、override 済みフィールドの値の変化が `PrefabOverrideChanged`、`prefabHash` の変化が `PrefabInstanceChanged`、プレハブのファイルの変化が `PrefabAssetChanged` → `cargo test` (§16.3 の 4 番目)。
8. `.meta` の guid が変わったリネームで、旧文書に断絶が記録され、新旧が同じ文書として扱われない。`.meta` ごと動かしたリネームは同じ文書 → `cargo test`。
9. 壊れた JSON、空のファイル、`entities` が無いファイルでサービスが落ちない → `cargo test`。

## 検証コマンド

- `cargo test --manifest-path tools\track\Cargo.toml`、`tools\track_verify.bat`、`tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴
