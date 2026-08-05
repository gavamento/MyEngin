# M50: 部位・アクター系の拾い残し一括回収

## 再開手順 (セッション跨ぎ用)
1. `git log --oneline -5` で最後に完了した M50x を確認 (1 サブ = 1 コミット = 1 セッション + /clear)
2. 本ファイルの該当サブの節を読み、直前サブの「検証」が済んでいる前提で着手
3. 全サブ共通検証: Debug/Release ビルド 0 警告 → `Editor.exe --selftest` 全 PASS →
   `tools\replay_verify.bat` PASS (既定デモ + --parts-demo の 2 ペア) → `tools\check_rules.ps1` 0 error。
   ソース追加時は `pwsh tools\gen_project_files.ps1`。M50d は `tools\build_managed.bat` も両構成
4. コミット形式: `M50a: <要約>` + 決定/実装メモ/検証/制限 の定型
5. 新規 UI 文字列は `LocalizationTable.inl` に `MYE_STR(id, en, ja)` (規則 10)
6. --parts-demo は cache\parts_showcase.scene.json をロード優先 — demo 変更時は cache 削除 (M49 の罠)

## 進捗表 (完了時に更新。計画外の事実・ハマった所・申し送りだけ書く)

| サブ | 状態 | コミット | メモ |
|---|---|---|---|
| M50a 修理と磨き | **完了** | (このコミット) | 計画外: モデル登録だけでは足りず、**ショーケース材質 (parts_*/rt_*) の登録がフラグゲートで Runtime には parts 登録が皆無**だった → Editor/Runtime とも無条件登録化 (名前キー少数・冪等・非ハッシュ)。これで cache シーン直接ロードで Floor/Target キューブ群も初めて描画された。RegisterSkinnedModels は selftest 専用で温存。サンプルの AssetID は FNV を手計算 (builtin://cube = 1506918697593860217, mat_blue, タグ WeakPoint/Head) — 描画実測で正しさを確認済み |
| M50b プレハブ UX | 未着手 | - | - |
| M50c 構造上書き | 未着手 | - | - |
| M50d codegen + ABI v11 | 未着手 | - | - |

---

## Context

M48 (プレハブ 2.0、11 サブ) + M49 (範囲部位) 完遂後の棚卸しで、「やると書いて未実施」の
項目を確定した (計画・ADR-011・全コミット申し送りの突き合わせ)。ユーザー指示で
**1 計画 (M50) に束ねる**。対象:

1. スキーマの型付きコード生成 (M48j/ADR-011 が「M49+」と明記した本命)
2. 保存シーン経由ロードでモデルが描画されない (M48i 申し送り、実害あり)
3. Create Prefab の命名/出力先 (M48d が M48i へ回して未実施 + 上書き穴は想定より重い)
4. M13 宿題: コンポーネント追加/削除の Revert + Unpack Prefab (ADR-011 の表で「未回収」)
5. ドキュメント整合 (ADR-011 の「当たり判定は将来対応」が M49 で回収済みになった件 等)
6. M49 小物 (PartBounds 単独警告、--edit-actor サンプル)

調査 3 本 + 構造上書き設計の敵対的検証 1 本を完了済み。以下は file:line まで裏取り済み。
※ユーザー仕様書 v1 への反映はリポジトリ外文書のため計画外 (ユーザー側作業)。

## サブ分割 (1 サブ = 1 コミット = 1 セッション + /clear、4 分割)

---

### M50a: 修理と磨き (bump なし)

**①保存シーンのモデル描画欠落の修理** — 新しいロードコードは書かない。
`ReloadMeshes` が既にヘッドレス登録の実体 (ModelLoader.cpp:399-439 / FbxLoader.cpp:806-835、
`lc.scene=nullptr` でエンティティ非生成・Load とバイト同一キー):
- 起動走査 (DemoContent.cpp:482-494) の `.fbx` を `FbxLoader::ReloadMeshes` に差し替え
  (RegisterSkinnedModels の完全上位互換)
- `ModelLoader::ReloadMeshes` に skins ループ (:462-464 の 3 行) を足して FBX と同格にし、
  `.glb/.gltf` も 1 呼びに統一 (RegisterSkinnedModels は selftest 用の薄いラッパで残す)
- 非モデルファイルへのログ抑制: 内部 `RegisterAssets(..., bool logErrors)` に括り出し
- DemoContent.cpp:61-63 の BoxTextured.glb ハードコード (単発の同修理) は、
  RegisterDemoContent 単独呼びの selftest が無いことを確認して削除
- device は起動走査時点で初期化済み (EngineLoop :266→:296→:427)。MeshLibrary::Register は
  device 省略時 CPU のみ登録 = selftest のヘッドレス文脈も安全 (GpuResources.cpp:148-178)
- selftest: PartSelfTest :353-413 のヘッドレス照合ハーネスに `headless.meshes.Get(mr->mesh)` /
  `materials.Get(...)` 検証を追加 (glTF/FBX 両方 — 今回の穴を将来検知する検査)

**②PartBounds 単独 (PartComponent 無し) への Inspector 警告** — RaycastParts の対象外である旨。
InspectorWindow の Part.joint 警告 (M48i) と同じ流儀。

**③`--edit-actor` サンプルアセット同梱** — モデル非依存 (キューブ + 部位 + PartBounds) の
.actor.json を assets\prefabs\ に追加。パスハッシュはリポジトリ相対で解決される構成のみ
(サブアセット ID を含まない = コミット可能)。ミニシーン編集 + 範囲部位表示の目視手段になる。

検証: 8 ビルド 0 警告 / --selftest / replay_verify (**無風 PASS が合格条件** — 起動走査の
変更は登録の追加のみでシーン内容不変だが、demo の BoxTextured 整理があるので実測で確認) /
check_rules / 目視 = cache\parts_showcase.scene.json を直接開いて CesiumMan が**描画される**こと
(修理の故障点そのもの)、--edit-actor サンプルでワイヤ + クリック選択。

---

### M50b: プレハブ UX — Create Prefab 命名 + Unpack (bump なし)

**①Create Prefab の命名** (実装は HierarchyWindow.cpp:41-68。現状: バイト単位 isalnum で
非 ASCII 全滅 / assets\prefabs\ 固定 / 一意化なし / 無警告上書き。**パスハッシュキーの置換で
既存インスタンスが新ベースへ黙って張り替わる**のが最重症):
- AssetBrowserWindow.cpp:720-783 の命名モーダル前例 (requestModal_ → 次フレーム OpenPopup) を
  Hierarchy 側にも適用。既定名 = エンティティ名
- サニタイズは禁止文字 (`\/:*?"<>|` + 制御文字) だけに緩める → 非 ASCII 名が通る
- `MakeUniquePath` (AssetOps.cpp:170-187、"x (1)" 連番) を通す。
  **AssetOps::CreateActorAsset (:356-385) の同じ穴も直す**
- 拡張子選択 (.actor.json / .prefab.json — kActorSuffix/kPrefabSuffix を渡すだけ)

**②Unpack Prefab** (Hierarchy 右クリック。最小実装が成立することを確認済み):
1. `CollectInstanceMembers(root, members, &innerRoots)`
2. members の PrefabLink / root の PrefabInstance を RemoveComponentRaw
3. 内側ルートはタグを残し `outerLocalId = 0` へ (内側インスタンスは無傷)
4. override 記録の消去は CaptureAfter → RecordOverrides の既存分岐で自動。Undo もタダ
   (全量スナップショット + ApplyPartial removeHiddenMissing)
- **ガード**: 祖先にインスタンスルートがある場合は禁止 (外側 Apply でその枝が新ベースから
  落ちるため)。部位の構造ロックが外れる旨をログ + 確認モーダルに明示
- selftest: Unpack 後の非ロック化 / 内側無傷 / Undo 往復 / 入れ子内 Unpack の拒否

検証: ビルド / --selftest / replay_verify 無風 / check_rules / 目視 = 日本語名エンティティから
Create Prefab → 日本語ファイル名で生成、同名 2 回で "(1)" 連番、Unpack → 青文字が消え
自由編集可、入れ子内で Unpack がブロックされる。

---

### M50c: 構造上書き = コンポーネント追加/削除の Revert (bump なし、シーン文書 version 2→3)

M13 宿題の本丸。**コンポーネント単位のみ** (エンティティ = 子の増減の追跡は v2 と明記)。
敵対的検証済みの設計:

**現状の穴 (直すもの)**: インスタンスで削除したコンポーネントを PropagateBaseChange
(Prefab.cpp:1009-1014) が無条件 AddComponentRaw で復活させ、:1045 の記録撮り直しで固定する。
Revert でも戻せない。UI は「ベースに無い comp」に青 * を出すが RevertField は no-op (:842-888)。

**設計**:
- override キーに `"+Component"` / `"-Component"` を追加 (WriteEntity/ReadEntityOverrides は
  任意文字列素通し = フォーマット変更なし)。**ライブ diff から純導出** (RecordOverrides の
  全置換と両立): `OverridesAgainstBase` (:286-318) 拡張 — ベース走査で実体に無い → "-C"、
  実体アーキタイプ走査 (TypeId 昇順 = 決定論) で追跡対象かつベースに無い → "+C"
- **追跡述語 = ReadEntityComponents の removeMissing 除外集合 (SceneSerializer.cpp:224-229) の
  ミラー**: NoSerialize / Hidden / Name / LocalTransform を除外。kComponentScriptState (C#) は
  含める (ベース追加の C# comp が既定値で生える既存ギャップはスコープ外とコメント明記)
- `PropagateBaseChange`: メンバ毎に**レコードをコピー取得** (ループ内 SetOverrides の
  リハッシュでポインタ失効 — :1209 に同前例) → comp 欠落 && "-C" → 復活スキップ /
  **comp 既存 && "+C" → 値伝播を丸ごとスキップ** (ベースが同名 comp を獲得したときの
  値クロバー防止 — "+C" が必須である理由)。レコード無し (レガシー) は現状どおり復活
- `RevertInstance`: 構造復元を追加 ("-C" をベース値で再生成 / ベース外の追跡 comp を除去)。
  Add/Remove 後のポインタ再取得に注意 (アーキタイプ移動)
- **`RefreshNonOverridden` は現状維持できない** (最大の穴: 閉シーン中のベース成長が編集 1 回で
  「ユーザーの削除」に誤ラベル・恒久化される)。**シーン文書 version 2→3** (SaveToJson :316、
  LoadFromJson で version を Scene に保持) + 分岐:
  - v3: ベースにあり実体に無く "-C" 無し → **ロードで AddComponentRaw + ベース値充填**
    (localId 昇順 = 決定論)。"+C" 付き comp はフィールド refresh をスキップ (クロバー対策)
  - v2 (M48e 期): 構造キーをライブ diff からレコードへマージのみ (実体不変 = M48e の削除が
    "-C" を得て sticky 化 — 現行より改善)。v2 + 閉間ベース成長は "-C" 扱い (一度きり、明記)
  - レコード無し: 現行移行のまま (拡張導出が構造キーも自動で拾う)
- 新 API: `Prefab::RevertComponent(scene, lib, e, compName)` (双方向) +
  `Prefab::ComponentOverrideState` (record 一次・ライブ fallback)
- UI (InspectorWindow): comp ヘッダに「+」バッジ / comp 右クリックに「Revert Added Component」/
  リスト末尾に「Removed prefab components: C [Restore]」節 (batchOp で Undo 包む) /
  追加 comp のフィールド単位 Revert は disabled (no-op バグの解消)
- 互換: 旧エンジンは "+/-" キーを素通し・不活性 (確認済み)。ReplayFile/WorldHash/ABI bump 不要
  (レコードはハッシュ対象外)

selftest (SceneSelfTest に新ブロック、検証済みリスト 13 本): 削除→"-C"→復活抑止→再導出維持 /
v3 往復で削除維持 / v3 + キー無し欠落はロードで追加 (新契約) / v2 移行で "-C" 付与 /
追加→"+C"→ベース獲得で値保持 + 値キー転換 / ベース削除で "+C" 転換 / 双方削除でキー消滅 /
削除→同値再追加でレコード空 / Undo/Redo 往復 / RevertInstance 双方向 / Apply 伝播 /
入れ子 (外側ベース経由の再播種) / レガシーは復活 = フォールバック現状維持ピン。
既存ピン (:542-599, :965-1030, :1198-1345, :1366-1384) は全て生存を確認済み。

検証: ビルド / --selftest / replay_verify 無風 / check_rules / 変異テスト (ゲートを外す →
「復活抑止」selftest が落ちることを確認して戻す) / 目視 = インスタンスで Collider 削除 →
アセット保存で復活しない → Revert All で戻る、追加 comp に「+」バッジ。

---

### M50d: スキーマ codegen + 汎用フィールド ABI ★v10→v11 束ね + ドキュメント

**前提 (確定)**: スクリプト→任意コンポーネントの汎用フィールドアクセスは C++/C# 両レーンとも
現状ゼロ (ABI は AddComponentByName / SetMeshRenderer のみ)。

**ABI v11 (末尾 append、スロット 2 本だけ — 家風「足す本数を減らすのが安全側」)**:
```c
int32_t (*GetComponentField)(void* engine, MyeEntityId e, uint64_t compNameHash,
                             uint64_t fieldNameHash, void* buf, int32_t bufSize,
                             int32_t* outType);  // 戻り値 = 実サイズ (0 = 無し/不一致)
int32_t (*SetComponentField)(void* engine, MyeEntityId e, uint64_t compNameHash,
                             uint64_t fieldNameHash, const void* buf, int32_t size);
```
- 実装はエンジン内前例の写し: `FindByName(nameHash) + GetComponentRaw + FieldDesc`
  (Prefab.cpp:854,889 / SearchWindow.cpp:145 と同じ解決)。ポインタは越境させない (値コピー)
- `MyeFieldType` (ScriptTypes.h:10-22) に AssetRef/String64/String256/Float4x4 を**末尾追加**
- 触る箇所は M49 で実証済みの一式: EngineAPI.h (:27 v11 + 履歴 + 末尾スロット) /
  EngineApiTable.cpp / Interop.cs 位置ミラー (72→73 スロット、機械照合) / ScriptAPI.h 糖衣 /
  MyeScript.cs / PartSelfTest の version 検査 / kEngineVersion 0.64→0.65

**codegen (生成は起動系でなくビルド系に差し込む)**:
- **nameHash は焼いてよい / TypeId は絶対に焼かない** (登録順依存)。生成物は Shared の流儀で
  FNV 再掲 (MyePartTag 前例)、エンジンヘッダ include 禁止
- C++: `<project>\cache\Generated\SchemaComponents.gen.h` — スキーマごとに
  `struct XxxSchema { ... }` (レイアウトミラー) + nameHash/フィールド定数 +
  Get/Set 糖衣 (汎用 ABI 経由)。差し込み = `AssetOps::BuildProjectScripts` の
  WriteGeneratedMain 直前 (:1026)。**WriteGeneratedVcxproj に AdditionalIncludeDirectories
  1 行追加が必要** (Common.props はエンジン側 include のみ)
- C#: `assets\scripts\Generated\Schema.gen.cs` — 静的クラス + 定数 + 糖衣。
  ScriptRuntime.Compile が再帰収集するので**追加設定ゼロ** (MyeScript 非派生は誤登録されない)。
  差し込み = CompileCSharpScripts (:503-515) と起動時 EngineLoop.cpp:366 の直前
- レガシー起動 (replay_verify 経路) は「生成のみ・誰も include しなければ挙動不変」で安全
- 生成側で nameHash 重複検出 (Register は黙って併合するため)

**リプレイ被覆**: demo シーンに Health スキーマ (TypeId 32) が既に載っている。恒久 probe
(C++ GameLogic) が汎用 ABI 経由で Health を毎 tick 読み書きし状態に反映 — M49 の
PartRaycastDemo と同じ「書き戻さないと被覆にならない」流儀。変異テスト (Set の値を汚す →
replay MISMATCH) も同型で実施。

**selftest**: SchemaSelfTest に「生成ヘッダの offset/size/nameHash が実行時登録と一致」検査。
C ABI 経路の Get/Set (型不一致・サイズ不足・未知 comp/field の 0 返し含む)。
C# は temp probe で実走 (abi-bump-verification の流儀)。

**ドキュメント**: ADR-011 更新 (「当たり判定のひも付け」= M49 PartBounds で回収済みを追記、
M13 宿題表の 2 件を M50 回収に更新、v1 制限の該当行を整理) + engine_spec 追記 +
必要なら ADR-012 (構造上書きとシーン文書 v3)。

検証: 8 ビルド + managed Debug/Release 0 警告 / --selftest / replay_verify (probe 追加で
録り直し PASS) / check_rules / ミラー機械照合 / C# temp probe / 変異テスト。

---

## リスク / 罠 (実装時に必ず意識)

1. Components.cpp / EngineAPI.h / Interop.cs / MyeFieldType / FieldDesc は**末尾 append 以外禁止**
2. M50c の "+C" は値クロバー防止の**必須**要素 (「"-C" だけ」への単純化は不可 — 検証済み)
3. PropagateBaseChange 内はレコードを**コピーして参照** (SetOverrides のリハッシュで失効)
4. シーン文書 v3 は「キー不在 = ベース追随」を構造へ拡張する契約変更 — v2/レガシーの
   フォールバック挙動を selftest でピン留めする
5. codegen の生成物はレガシー検証経路に include させない (生成のみは挙動不変)
6. C# comp のベース伝播は既定値で生える既存ギャップ (スコープ外、コメント明記 + 実装時に実測)
7. --parts-demo は cache のシーンをロード優先 — demo 変更時は cache 削除 (M49 の罠)

## 検証コマンド (全サブ共通、M48/M49 と同じ)

8 ビルド (+M50d は managed) 0 警告 / `--selftest` Debug/Release ALL PASS /
`tools\replay_verify.bat` / `tools\check_rules.ps1` / サブ固有の変異テストと目視項目は各節。
