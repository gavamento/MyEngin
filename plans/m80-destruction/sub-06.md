# sub-06: コンポーネント・破片エンティティ構築・root proxy 描画・`--fracture-demo` (壊れる前)

- 依存: sub-03, sub-05
- 状態: 未着手
- 往復: 0

## やること

壊れる前までの縦切りを 1 本通す: 焼き結果 → 破片エンティティ → 1 剛体として落ちて転がる → 元メッシュ 1 つで描かれる → replay 一致。

1. **コンポーネント** (spec §4.2 の欄どおり): `DestructibleComponent` (TypeId 64)、`FracturePieceComponent` (TypeId 65)。`Components.h` に構造体、`Components.cpp` の**末尾**に登録 (`MYE_JP` の日本語名、Int32 の enum 相当はツールチップで値の意味)。`EditorComponentCatalog.cpp` の表に物理カテゴリで追加。`kSimSnapshotVersion` を上げるかは WaterWave (63) 追加時の前例に従う (SELF_EVAL に根拠)
2. **`BuildFracturePieces(world, root, 資産)`** (Engine 層、`RagdollBuilder.cpp` と同じ置き場の流儀): spec §4.1「破片エンティティ」どおりに子を組む。既存の `FracturePiece.root == root` の子を先に消してから組み直す (再生成)。ルートに Rigidbody が無ければ付け `compoundColliders = true`、ルート自身の Collider は外して警告。エンティティ名は `Frag<i>` / `_cap`
3. **root proxy の描画規則** (`RenderSystem.cpp:1098-1115` 付近): `Destructible.broken == false` のとき、`FracturePiece` を持つエンティティとその `_cap` 子を描かない。`broken == true` のとき、Destructible を持つエンティティ自身の MeshRenderer を描かない。**アーキタイプごとに型 index を 1 回引く**車輪と同じ書き方で、破壊物の無いシーンは余計な処理を走らせない。影・Deferred・RT の収集が同じ item 列を使うか coder が確認し、どの経路でも同じ規則になること
4. **実行時の整合確認**: ルートの Destructible が参照する資産の破片数と、子の `FracturePiece.index` の集合が一致しなければ、その Destructible を「無効」として扱う (sub-07 の破断をしない)。ERROR 1 回
5. **デモ `--fracture-demo`**: `DemoContent.cpp` に builder、`ShowcaseScenes.cpp` の `kShowcases` に行を追加 (`cache\fracture_showcase.scene.json`)。**ビルド時にメモリ上で焼いて登録**する (sub-03 の GUID なし登録口、接頭辞 `fracture://demo_...`)。内容 (壊れる前の範囲): 床、落下して転がる破壊物の箱 (動的ルート)、固定の壁 (kinematic ルート、sub-07 で撃つ的) を置く。この時点では割れない (FractureSystem 未実装)
6. **replay_verify に `fracture` job を追加** (`tools\replay_verify.bat`: `:job_fracture`、`-Jobs` 一覧、`:failed` の診断、echo 行の数)。`tools\shot_verify.bat` に golden を足すかは coder 判断 (足すなら sub-07 で撮り直すことを申し送り)
7. **EngineCliSelfTest** の showcase 表の検査 (`EngineCliSelfTest.cpp:304-320`) を更新

## やらないこと (このサブでは)

- 破断・塊の分離 (sub-07)、割れた後 (sub-08)
- Inspector の生成ボタン (sub-09)。このサブでは生成は demo builder と SelfTest から呼ぶだけ

## 触る場所 (planner の見立て)

- `src/Engine/Core/Components.h/.cpp`、`src/Editor/EditorComponentCatalog.cpp`
- 新規 `src/Engine/Engine/FractureBuilder.h/.cpp` (名前は coder 判断)
- `src/Engine/Engine/RenderSystem.cpp` (描画の規則)
- `src/Engine/Engine/DemoContent.cpp/.h`、`src/Engine/Engine/ShowcaseScenes.cpp`、`src/Engine/Engine/EngineCliSelfTest.cpp`
- `tools/replay_verify.bat` (+ `tools/shot_verify.bat` 任意)
- `FractureSelfTest.cpp` にケース追加
- `src/Engine/Core/LocalizationTable.inl` (コンポーネント名・欄名の表示にローカライズが要るなら)
- **触らない**: `src/Engine/Renderer/WaterPass.cpp` (WIP)。RenderSystem 側の変更で WaterPass に手を入れる必要が出たら止めて報告

## 受け入れ条件 (このサブ)

1. `BuildFracturePieces` で子が破片数 × 2 (破片 + `_cap`) できる。欄の値 (index / root / mesh / hull / material) が正しい。2 回呼んでも子が倍にならない — `--selftest`
2. 動的ルートの破壊物が 1 剛体として落ちて床で止まり、重心・慣性が「同じ形の凸包を複合にした物」と一致 (sub-05 の合成を通っている) — `--selftest`
3. 描画: 壊れる前は元メッシュだけが描かれる。`broken` を手で true にすると破片だけが描かれる (デモのスクショ 2 枚 / または描画 item 数の SelfTest) — `--screenshot` + 画像パス
4. 資産の破片数と子が合わない Destructible が無効になり ERROR 1 回、落ちない — `--selftest`
5. `--fracture-demo` の replay (Debug 録り → Debug `--snapshot-stress 37` → Release) が一致 — `replay_verify.bat` の `fracture` job
6. **既存は不変**: `tools\replay_verify.bat` の既存 job 全 PASS、`tools\shot_verify.bat` 全 PASS、`PhysicsSelfTest` PASS (TypeId が 2 つ増えて実行時のスクリプト TypeId がずれても挙動が同じこと) — 実行ログ
7. `check_rules.ps1` PASS。WIP ファイル不変 — `check_rules.ps1`、`git status`

## 検証コマンド

```
tools\gen_project_files.ps1
（Debug|x64 と Release|x64 をビルド）
bin\x64\Debug\Editor.exe --selftest
bin\x64\Release\Editor.exe --selftest
bin\x64\Release\Runtime.exe --fracture-demo --screenshot ...   (スクショの撮り方は既存の shot_verify に倣う)
tools\replay_verify.bat
tools\shot_verify.bat
tools\check_rules.ps1
```

## 実装メモ (coder が追記)

## フィードバック履歴
