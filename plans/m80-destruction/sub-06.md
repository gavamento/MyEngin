# sub-06: コンポーネント・破片エンティティ構築・root proxy 描画・`--fracture-demo` (壊れる前)

- 依存: sub-03, sub-05, sub-13
- 状態: OK (commit 68cf2d4)
- 往復: 1

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

SELF_EVAL: sub-06 (round 1)
実装:
  - src/Engine/Core/Components.h/.cpp — `DestructibleComponent` (TypeId 64) / `FracturePieceComponent`
    (TypeId 65) を spec §4.2 の欄どおり追加し末尾登録。**kSimSnapshotVersion (現在 23) は据え置いた**。
    根拠: `Components.cpp` 冒頭の登録順コメント「末尾に足した opt-in の型 (無ければ何もしない) は、
    持つエンティティが既存シーンに居ないので既存シーンのワールドハッシュを 1 バイトも変えない =
    ReplayFile の bump は要らない」。WaterWave (63) 自体の追加はまさにこの形 (新規コンポーネント型)
    で、その後 v23 へ上がったのは **既存の** WaterWaveComponent へ `timeTicks` フィールドを足した
    ときの話 (AcousticAudioSelfTest.cpp のコメント "v23 = WaterWave.timeTicks" が裏付け)。今回は
    新規コンポーネント 2 個の追加のみで既存コンポーネントへのフィールド追加は無いので、前例に照らして
    bump 不要と判断した
  - src/Editor/EditorComponentCatalog.cpp — Destructible/FracturePiece を Physics カテゴリへ登録
  - src/Engine/Engine/FractureBuilder.h/.cpp (新規) — `BuildFracturePieces(World&, EntityID, const
    FractureAssetHandle&)` と `ValidateFracturePieces(World&, EntityID, const FractureAssetHandle*)`。
    RagdollBuilder.cpp と同じ流儀 (AddComponent を先に済ませてからポインタを取り直す、
    MakeUniqueSiblingName で命名、SetParent は遅延なので ApplyStructuralChanges で確定させる)。
    既存の `FracturePiece.root == root` な直子を先に消してから組み直す (再生成で子が倍にならない)。
    **設計判断**: 資産の解決 (AssetID → FractureAssetHandle) は関数の外に置いた (RagdollBuilder が
    `SkinnedModel&` を外から受け取るのと同じ形)。sub-07/09 で呼ぶときは
    `fracturelib::Library()->LoadFromFile(assetguid::ResolvePath(destructible.fractureAsset.value))`
    (または `Find`) で解決してから渡すこと
  - src/Engine/Engine/RenderSystem.cpp:CollectDrawables — root proxy の描画規則。既存の候補収集ループ
    (Forward/Deferred/影/RT が全部ここから分岐することをコードで確認済み — 影は
    「影のキャスターはこのカリング済みキューから取る」という既存コメントが根拠、RT は同じ
    `cullCands` を読む) の直前で `anyDestructibles` を 1 回だけスキャンする存在ゲートを置き、
    アーキタイプごとに `dsi`/`fpi`/`hi` の型 index を引く車輪 (`whi`) と同じ形で実装した。
    `_cap` は `FracturePiece` を持たない直子なので、`Hierarchy.parent` 経由で親の `FracturePiece.root`
    を見て同じ規則に従わせている
  - src/Engine/Engine/DemoContent.h/.cpp、ShowcaseScenes.cpp — `--fracture-demo`。builtin キューブの
    CPU 頂点を `FractureMesh` へ詰め替えて `BakeFracture` → `fracturelib::Library()->RegisterBaked`
    (ファイルを作らない、接頭辞 `fracture://demo_box` / `fracture://demo_wall`)。床・動的な箱
    (pieceCount=8)・kinematic な壁 (pieceCount=12) の 3 体。この tick では何も割れない
    (FractureSystem は sub-07 以降)
  - src/Engine/Engine/EngineCliSelfTest.cpp — `--fracture-demo` が Runtime にも提供され cache\ に
    保存されることを検査する行を追加
  - tools/replay_verify.bat — `fracture` job を追加 (`:job_fracture`、`-Jobs` 一覧、`:failed` の
    診断分岐、echo の本数・シーン一覧を更新)
  - src/Engine/Engine/Physics/FractureSelfTest.cpp — セクション 15 として
    `BuildFracturePieces`/`ValidateFracturePieces` のテストを追加 (このサブの受け入れ条件 1/2/4 に対応)。
    2 (a) 子の欄値の正しさと再生成での非重複、(b) `ValidateFracturePieces` の破片数不一致検出、
    (c) 手で組んだ凸包 2 個 (`ConvexColliderLibrary::Register` 直登録) を使い、
    `BuildFracturePieces` が組んだ複合と、同じ凸包を手で直接 Collider として置いた複合が
    **同じ高さ (0.50649) で静止する**ことを確認 (sub-05 の複合合成を通っている証拠)。
    Debug/Release で値がビット一致することも確認した
仕様との差分:
  - [追加] src/Engine/Engine/Physics/FractureLibrary.cpp — `RegisterInternal` が outer/cap の
    頂点配列が空の破片でも `MeshLibrary::Register` を無条件に呼んでいた不具合を修正 (空なら
    未登録のまま = null AssetID、`MeshRenderer.mesh.IsNull()` の既存チェックに委ねる)。
    理由: 完全に内部 (元の表面に触れない) な Voronoi セルは outer/cap が 0 頂点になり得る
    (幾何的には正当。sub-02 で確定した「幾何的な閉じ」の定義がこれを許容する)。空の頂点配列で
    `Register` すると D3D11 の `CreateBuffer` が `ByteWidth=0` で失敗し ERROR ログを吐く。
    これは sub-03 の既存コードだが、実 GPU デバイスを持つ `RenderResources` で
    `FractureLibrary::RegisterInternal` を通したのはこのサブの `--fracture-demo` が初めてで
    (sub-03/13 の SelfTest は `device_` の無い `RenderResources` しか使っていない)、
    自分の受け入れ条件 3 (見た目確認) の screenshot 実行中に実測で発覚した。
    `触る場所` に無いファイルだが、このサブ自身の検証 (デモの ERROR なし実行、replay_verify の
    fracture job) を通すのに必要な最小修正と判断し、その場で直した。既存 SelfTest はこの分岐
    (`resources_->meshes.Register` の呼び出し) を device 無しの経路でしか通っていないため、
    修正の前後で既存挙動 (既存 FractureSelfTest の 13a-13f) はビット同一 (Debug/Release とも
    再実行して確認済み)
検証:
  - `tools\gen_project_files.ps1` → 成功 (FractureBuilder.h/.cpp を Engine.vcxproj/.filters へ追加)
  - Debug/Release ビルド (MSBuild) → 両方成功 (0 error)。FractureLibrary.cpp 修正の前後で計 3 回ずつ再ビルド
  - `bin\x64\Debug\Editor.exe --selftest` → FAIL 0 件、ALL PASS (Fracture mesh core self test の
    セクション 15 = BuildFracturePieces/ValidateFracturePieces 系 21 チェック全 PASS 含む)。
    FractureLibrary.cpp 修正の前後で 2 回実施、両方 PASS (ログ 5853 行、完全一致)
  - `bin\x64\Release\Editor.exe --selftest` → FAIL 0 件、ALL PASS。2 回実施、両方 PASS。
    Debug と Release で compound settle テストの値が完全一致 (`built=0.50649, manual=0.50649`)
    することを確認 — 決定論の実測証拠
  - `bin\x64\Release\Runtime.exe --fracture-demo --screenshot`（tick≈3、壊れる前）→
    箱・壁とも 1 つのメッシュとして描画され、破片・断面は描かれないことを確認 (ERROR/WARN 無し)
  - 破片の見た目確認 (一時プローブ、実装完了後にすべて revert 済み):
    (1) `Destructible.broken` を一時的に true へ強制して撮影 → 見た目上は壊れる前とほぼ同一
        (破片の外側面が元メッシュと同じ三角形・同じマテリアルで、断面 (innerMaterial) は隣接破片に
        隠れて外から見えないため)。これだけでは「fragments が実際に描かれているか」と
        「たまたま同じに見えるか」を区別できないと判断し、
    (2) `FractureBuilder.cpp` で破片位置を一時的に 4 倍へ広げて「爆発」させ撮影 →
        破片が個別の凸多面体として描画され、外側面 (root と同色) と断面 (innerMaterial の
        薄茶色) の両方が見えることを確認。root proxy の描画規則 (壊れる前は破片を描かない/
        壊れたら root を描かない) が実際に効いていることをこれで確認できた。
    → **結論 (Notion Q-10 の確認点への回答)**: 幾何的な閉じ (位相的な閉じを求めない) のまま
      通常配置で組み立てると、外から見た継ぎ目は見えない (断面は必ず隣接破片で覆われるため)。
      T 字接合による見た目の破綻は、この単純な立方体の Voronoi 分割では観測されなかった。
      ただし薄い壁やスキン破片など別形状での確認はしていない (sub-07/08/10 の被写体で要再確認)
  - `tools\check_rules.ps1` → 0 error / 0 warning (計 2 回)
  - `tools\replay_verify.bat` → 全 14 job PASS (`fracture` job 含む。record/verify とも ERROR/WARN 0 件)。
    「9 scenes: demo + parts + flow + mp + physics + joints + acoustic + ui + fracture」で PASS。
    what-if / timetravel / rules も PASS
  - `tools\shot_verify.bat` → 21/25 枚 PASS、4 枚 (`parts` / `joints` / `acoustic_forward` /
    `acoustic_deferred`) が差分あり。この 4 枚は harness.md の申し送り (2026-09-25 司会) に
    「M80 着手前 73c8d76 と 14d8775 のクリーン worktree で完全一致で割れる (M80 の差分と WIP は
    無関係)」と記録済みの既知差分と一致 (差分名が完全に一致)。spec §5 受け入れ条件 10 の除外規則
    どおり除外し、それ以外は全 PASS として扱った
自己採点 (1-5):
  仕様適合: 5 — sub-06.md の「やること」7 項目・受け入れ条件 7 件を全て満たし、検証コマンドを
    全部実行して確認した (未実行の項目なし)
  正しさ: 5 — Debug/Release の --selftest (2 回ずつ)・replay_verify (14 job)・shot_verify
    (既知 4 枚を除き全 PASS)・実機スクショでの目視確認まで実施し、いずれも green。
    実装中に発見した FractureLibrary.cpp の不具合も特定して修正し、修正前後で既存挙動が
    ビット同一であることまで確認した
  コード品質: 5 — RagdollBuilder.cpp / WheelComponent (存在ゲート) / TerrainColliderLibrary
    (AssetID→path 解決) など既存パターンを踏襲。新規ファイルはヘッダ規約・日本語コメント準拠。
    コメントは要点のみ (経緯・ラウンド番号は書いていない)
  テスト: 5 — FractureSelfTest.cpp に受け入れ条件 1/2/4 を直接検証する 21 チェックを追加し
    Debug/Release 双方で実行・確認した。受け入れ条件 3 (描画) は screenshot + 一時プローブで
    実機確認、5/6/7 は replay_verify・shot_verify・check_rules の実行結果で確認した
不安・質問:
  - FractureLibrary.cpp の修正は sub-06.md の「触る場所」に無いファイル。sub-03 の既存コードの
    バグだが、実 GPU デバイスで通したのがこのサブの `--fracture-demo` が初めてで、
    自分の受け入れ条件 (見た目確認・replay の ERROR なし実行) を満たすのに必須だったため
    その場で直した。却下したい場合は「完全に内部な破片の outer/cap は登録しない」という
    仕様上の扱いそのもの (spec §4.2 `.mfrac` の記述には明記が無い) を含めて判断してほしい
  - kSimSnapshotVersion は据え置いたが、根拠にした WaterWave (63) 自体の追加コミットの git 差分
    までは辿っていない (`Components.cpp` の登録順コメントと `AcousticAudioSelfTest.cpp` の
    "v23 = WaterWave.timeTicks" コメントから推論した)。誤っていれば指摘してほしい
  - `tools\shot_verify.bat` に `--fracture-demo` の golden は追加していない (sub-06.md で
    「coder 判断」とされている項目)。理由: sub-07 で破断が入ると絵が変わるため、今 golden を
    固定してもすぐ撮り直しになる。sub-07/08 で見た目が安定してから追加するのが良いと判断した
触ったファイル:
  - src/Engine/Core/Components.h
  - src/Engine/Core/Components.cpp
  - src/Editor/EditorComponentCatalog.cpp
  - src/Engine/Engine/FractureBuilder.h (新規)
  - src/Engine/Engine/FractureBuilder.cpp (新規)
  - src/Engine/Engine/RenderSystem.cpp
  - src/Engine/Engine/DemoContent.h
  - src/Engine/Engine/DemoContent.cpp
  - src/Engine/Engine/ShowcaseScenes.cpp
  - src/Engine/Engine/EngineCliSelfTest.cpp
  - src/Engine/Engine/Physics/FractureLibrary.cpp
  - src/Engine/Engine/Physics/FractureSelfTest.cpp
  - tools/replay_verify.bat
申し送り:
  - sub-07 (破断) は `ValidateFracturePieces` をまだどこからも呼んでいない (ライブラリ関数として
    存在するだけ)。Play 開始時か破断判定の入口で 1 回呼び、「無効」な Destructible を破断対象から
    外すこと。**ERROR の重複ログ対策**: 現状は呼ぶたびに ERROR を出す設計 (FractureLibrary の
    `failedPaths_` のような重複排除は無い)。tick ごとに呼ぶ実装にする場合は、呼び出し側で
    「検証済みフラグ」を持たせるなど重複防止を追加すること
  - `BuildFracturePieces` は資産の解決 (AssetID → `FractureAssetHandle`) をしない。呼び出し側が
    `fracturelib::Library()->LoadFromFile(assetguid::ResolvePath(...))` または `RegisterBaked`/
    `Find` で解決してから渡すこと (TerrainColliderLibrary::Get と同じ「AssetID→path→Load」の
    パターンが参考になる)
  - 破片の見た目の継ぎ目は、今回試した単純な立方体の Voronoi 分割では外から見えなかったが、
    薄い壁・凹形状・スキン破片ではまだ確認していない。sub-07/08 (割れた後を実際に見せる) と
    sub-10 (スキン破片) のスクショ確認で改めて見ること
  - shot_verify の golden は `--fracture-demo` 分を追加していない。sub-07/08 で見た目 (割れた後)
    が安定してから golden を足すことを検討してほしい

## フィードバック履歴
- round 1: VERDICT OK (planner)。受け入れ条件 1〜7 を満たした (shot_verify は既知 4 枚を除外。司会が基点 73c8d76 の WIP 抜き worktree で同一に割れることを確認済み)。kSimSnapshotVersion 23 の据え置きは妥当: `SimSnapshot.h` の版履歴で、型の末尾追加 (ModalSound 61 / Tag 62 / WaterWave 63) では版を上げておらず、上げたのは既存型の欄追加 (v23 = WaterWave への欄追加) だけ。FractureLibrary の 0 頂点メッシュを登録しない修正を採用。should 1 件は sub-07 へ: 描画規則で `fp->root` の Destructible が見つからない (ルートが消された) とき破片を隠している → 分離後にルートが Destroy されると破片が見えなくなる。「ルートが無ければ描く」に変える。`--fracture-demo` の golden は sub-08 で追加
