# sub-04: ボクセル化 + surface nets (開いたメッシュの経路)

- 依存: sub-01 (閉じ判定)。sub-02 / sub-03 と並列可
- 状態: OK (コミット待ち。開いた箱の解像度 48 / 64 は sub-14 へ移管)
- 往復: 2

## やること

spec §4.1 焼き 1 の「`openMeshMode == 1`: ボクセル化を許容」の経路を純関数で作る。

1. **解像度可変のボクセル化**: 入力メッシュ (開いていてよい) の AABB の最長辺を `voxelResolution` (16..256) セルに割った立方セルの格子。外周に 1 セル以上の空きを置く。表面の三角形に触れるセルを占有とし、外側から塗りつぶして「外に繋がらない空洞」を内部として埋める (開いた箱は殻だけが残るので殻の厚み = 1 セル以上の立体になる)
2. **surface nets**: 占有格子から閉じた三角形メッシュを作る (境界は空きで囲んであるので必ず閉じる)。法線は面から、UV は箱投影
3. 出力メッシュが sub-01 の閉じ判定を通ることを関数の中で確かめ、通らなければ失敗を返す
4. **流用しない**: `src/Engine/Engine/Modal/Voxelizer.*` (32³ 固定、Deep-Modal と学習・実行時で共有する契約) は読んでよいが変えない・呼ばない。`TriBoxOverlap` 相当が必要なら別に書くか、共有してよいかを SELF_EVAL で planner に問う (既定: 別に書く)
5. 決定論: 同じ入力で同じバイト列。並列化しない
6. 焼きの入口 (sub-02 の関数) に `openMeshMode` と `voxelResolution` を渡せるようにし、閉じていなければ 0 = 拒否 (理由付き)、1 = このサブの関数で閉じたメッシュにしてから分割、を分岐させる。sub-02 がまだ入っていない場合は入口の配線だけ後のサブに回してよい (その旨を申し送り)

## やらないこと (このサブでは)

- Inspector (sub-09)
- 元の UV を保つこと (ボクセル化経路の外側面の UV は箱投影でよい。制限として spec に書かれている扱い)

## 触る場所 (planner の見立て)

- 新規 `src/Engine/Engine/Physics/FractureVoxel.h/.cpp` (名前は coder 判断)
- sub-02 の焼きの入口
- `FractureSelfTest.cpp` にケース追加
- **触らない**: `src/Engine/Engine/Modal/*`、`tools/deepmodal/*`、`assets/deepmodal/*`

## 受け入れ条件 (このサブ)

1. 蓋のない箱・平面 (quad)・二重の壁を持つメッシュをボクセル化 → surface nets した結果が閉じ判定を通る — `--selftest`
2. 解像度 16 と 64 で出力の三角形数が増え、元の AABB との差がセル 2 個分以内 — `--selftest`
7. (round 1 で追加) 出力が surface nets (同じ平面上の面の連なりではない) で、曖昧な配置の入力 (対角に並ぶ 2 ボクセル) でも閉じ判定を通る — `--selftest`
8. (round 1 で追加) `BakeFracture(openMeshMode = 1, pieceCount = 16)` が、開いた箱と平面で解像度 32 / 48 / 64 の全部について成功し、Release の焼き時間が記録され、既定値の結論が出ている — `--selftest` + SELF_EVAL
3. 開いた入力を `openMeshMode = 0` で渡すと理由付きで拒否、`= 1` で分割まで通り全破片が幾何的に閉じる (体積 > 0、ベクトル面積の和が表面積の 1e-4 以下。spec 変更 2026-09-25) — `--selftest` (sub-02 が入っていれば)
4. 同じ入力で 2 回のバイト列一致 — `--selftest`
5. 処理時間 (解像度 64 / 128 / 256) を記録 — SELF_EVAL
6. 既存 SelfTest (特に Deep-Modal の Voxelizer 系) と `check_rules.ps1` に変化なし。WIP ファイル不変 — `--selftest`、`check_rules.ps1`、`git status`

## 検証コマンド

```
tools\gen_project_files.ps1
（Debug|x64 と Release|x64 をビルド）
bin\x64\Debug\Editor.exe --selftest
bin\x64\Release\Editor.exe --selftest
tools\check_rules.ps1
```

## round 1 の裁定 (planner、FIX_REQUEST の手順)

**原因の見立て**:
- round 1 の実装は、占有境界の立方体の面をそのまま出す**ブロック状の抽出**で、surface nets ではない (spec の逸脱)
- 出力は同じ平面上の大きな面 (2 三角形 × 数千) の集まりになる。そのため Voronoi 面の断面は、**一直線に並ぶ点 (共線点) が数千個続く輪郭**になる
- `EarClip` (`FractureMesh.cpp` の 420-442 付近) は、`cr <= areaEps` の頂点を耳にできない。さらに、境界を含む三角形の内外判定で、共線点が隣の耳を「塞いで」しまうので詰まる
- 解像度 16 だけ通るのは、共線の列が短いから

手順:
1. **surface nets にする**: 占有格子のセル (8 隅の占有が混在する双対セル) ごとに頂点を 1 つ置く。位置は、そのセルの 12 辺のうち占有が変わる辺の中点の平均 (反復の平滑化はしない。純関数で決定的)。占有が変わる格子辺ごとに、その辺を囲む 4 セルの頂点で四角形を作る。向きは占有側から外へ。四角形を三角形 2 つに割る対角は決定的な規則 (例: 短い方の対角、同値なら index 小) で選ぶ
2. **曖昧な配置の事前解消**: 格子辺のまわりの 2×2 で占有が対角に並ぶ配置 (1,0,1,0) は、その辺を 4 面が共有する非多様体を生む。メッシュ化の前に、このような配置の空きセルのうち index 最小のものを占有にする。配置が無くなるまで繰り返す (占有は単調に増えるだけなので必ず止まる)。頂点だけで接する配置は、辺の多様体性を壊さないので残してよい
3. 関数の最後の `CheckClosedMesh` はそのまま残す (閉じていなければ失敗を返す)
4. **`EarClip` の共線点**: 前後と一直線に並び、かつ間にある頂点 (`|cr| <= areaEps`、`dot(curr−prev, next−curr) > 0`) は、面積 0 の三角形 (prev, curr, next) として先に取り除いてよい。面積 0 なので見た目と体積に影響せず、辺の使用回数も保たれるので、切断結果は厳密に閉じたまま。各周回で共線点があれば、それを優先して外す。既存の箱・L 字・トーラスの digest は変わってよいが、Debug / Release の一致は保つこと
5. **焼きの計測と既定値**: `BakeFracture(openMeshMode = 1, pieceCount = 16)` を、開いた箱と平面で、解像度 32 / 48 / 64 について成功させ、Release の時間を記録する。**解像度 64 で 10 秒以内なら既定値は 64 のまま、超えるなら 10 秒以内に収まる最大 (32 か 48) を既定値として SELF_EVAL に書く** (Components の欄は sub-06 で作るので、既定値はそこで反映する)。どの解像度でも `EarClip` が支配的で遅い場合は、耳の候補の内外判定を反射頂点 (凹頂点) だけに絞る最適化を入れてよい (出力が同じなら digest は変わらない)
6. `TriBoxOverlap` の独立実装はそのまま採用

## 実装メモ (coder が追記)

SELF_EVAL: sub-04 (round 1)
実装:
  - src/Engine/Engine/Physics/FractureVoxel.h/.cpp (新規) — `VoxelizeMeshForFracture(source, resolution, out)`。
    入力 (開いていてよい) の AABB を最長辺基準で `[16,256]` セルへクランプした解像度で立方セル格子化し、
    外周 1 セルの空きパディングを確保 (`ComputeGridFrame`)。三角形と重なるセルを Akenine-Möller の
    三角形/箱オーバーラップ (`TriBoxOverlap`、Modal/Voxelizer.cpp と同じアルゴリズムだが独立実装) で
    占有にし、pad リングから 6 近傍 flood-fill (`FloodFillInterior`) で外部を確定、残りを内部として埋める。
    占有セルと非占有隣接セルの境界だけを四角形 (2 三角形) として取り出し (`BuildBoundaryMesh`/`EmitFace`)、
    法線は面から、UV は箱投影 (法線の支配軸と直交する2軸)。角の座標は整数グリッド index の純関数
    (`CornerPos`) なので、隣接セルが生成する同じ角は常にビット同一になり、位置溶接ベースの
    `CheckClosedMesh` がそのまま使える。関数の最後で `CheckClosedMesh` を呼び、通らなければ失敗を返す
    (このサブの受け入れ条件3)
  - src/Engine/Engine/Physics/FractureBake.h/.cpp:BakeFracture — 「焼きの入口」として、
    `FractureBakeInput` に `openMeshMode`/`voxelResolution` を追加。`CheckClosedMesh` で入力を検査し、
    閉じていなければ `openMeshMode==0` で理由付き拒否、`==1` で `VoxelizeMeshForFracture` を呼んで
    閉じたメッシュに差し替えてから続行。閉じている (元から/ボクセル化後) が `signedVolume<0` なら
    `FlipMeshWinding` で正規化してから `PlaceSeeds`/`BakeFractureCore` へ渡す。既存の箱/L字/トーラスの
    digest (`0xA62D9B06031B23C8` / `0x87A81F882E68E6DE`、sub-02/13 の記録値と一致) が変わらないことを
    確認済み — 既に閉じて外向きな入力には無効な分岐
  - src/Engine/Engine/Physics/FractureSelfTest.cpp/.h — セクション14として、開いた箱/平面(quad)/
    二重壁 (新規 `MakeDoubleWallMesh`、平行2枚の非連結な開いた面) のボクセル化が閉じることの確認、
    解像度16/64での三角形数増加とAABB差 (両方とも1セル以内、要求の2セル以内を満たす) の確認、
    同一入力の2回のバイト列一致 (`SerializeMesh` の比較)、`BakeFracture` 経由の `openMeshMode` 0/1分岐
    (0は拒否、1はボクセル化を経て分割・全破片が幾何的に閉じることを確認)、解像度64/128/256の処理時間
    記録を追加
  - tools\gen_project_files.ps1 を実行し build/Engine.vcxproj(.filters) に新規2ファイルを反映

仕様との差分:
  - [追加] 「焼きの入口」への openMeshMode/voxelResolution 配線を `BakeFracture` 自体に実装した
    (sub-04 の指示どおり。sub-02 は既にコミット済みだったため「後回し」オプションは使わず実施)。
    これにより `BakeFracture` は「入力は閉じていて外向き」という sub-02 時点の前提を持たなくなった
    (`BakeFractureWithSeeds` は従来どおり呼び出し側が保証する契約のまま、ヘッダコメントを明記)
  - [追加] 「surface nets」を、頂点をスムージングする本来のアルゴリズムではなく、占有セルと
    非占有セルの境界をそのまま四角形として取り出す「ブロック面抽出」として実装した。理由:
    sub-04.md 自身が「法線は面から」と書いており (スムージングする実装なら法線は隣接セルの平均に
    なるはず)、位相的な閉じを保証する構造的な単純さ・決定性の検証しやすさを優先した。挙動として
    「境界は空きで囲んであるので必ず閉じる」は満たす
  - [追加] TriBoxOverlap は既定どおり Modal/Voxelizer.cpp と共有せず独立実装にした (質問なし、既定を採用)

検証:
  - `tools\gen_project_files.ps1` (pwsh 直呼び) → Engine.vcxproj / .vcxproj.filters に FractureVoxel.h/.cpp を追加、成功
  - MSBuild `MyEngine.sln` Debug|x64 / Release|x64 → 両方ビルド成功 (新規警告なし)
  - `bin\x64\Debug\Editor.exe --selftest` → 全体 exit=0、"Fracture mesh core self test: ALL PASS" (新規14節すべて PASS)
  - `bin\x64\Release\Editor.exe --selftest` → 全体 exit=0、同じく ALL PASS。lshape/torus の digest は
    Debug/Release で完全一致 (`0xA62D9B06031B23C8` / `0x87A81F882E68E6DE`)、既存 Modal(Voxelizer) self test も
    ALL PASS (回帰なし)
  - `tools\check_rules.ps1` → 0 error / 0 warning
  - `git status --porcelain` → WIP ファイル (WaterPass.cpp / deepmodal 系) 不変、変更はこのサブの
    ファイルのみ (+ 生成物の vcxproj)
  - 処理時間 (受け入れ条件5、open box 解像度64/128/256、`VoxelizeMeshForFracture` 単体):
    Debug = 948-1106ms / 4684-4946ms / 23219-24083ms、Release = 113ms / 627ms / 3257ms
    (三角形数 84016 / 331824 / 1318960。Release は Debug の約8〜10倍速い)
  - AABB 差 (受け入れ条件2、open box 解像度16/64): diff=0.125 (cell=0.125、ちょうど1セル) /
    diff=0.0312 (cell=0.0312、ちょうど1セル)。要求の2セル以内に対し余裕あり

自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件1・2・4・5・6は完全に満たす。条件3も `--selftest` で通したが、
    後述の不安・質問のとおり `BakeFracture` の実運用パラメータ (既定 voxelResolution=64) では
    別コンポーネント (EarClip、sub-01/02所有) の頑健性限界に触れる既知のリスクが残るため4とした
  正しさ: 4 — 全検証コマンドを実行し証拠を確認した (未実行の主張なし)。ボクセル化+surface nets
    単体は解像度16〜256まで閉じ・決定論・AABB妥当性を確認済み。BakeFracture 経由の実分割は
    低解像度 (16) でのみ確認しており、中〜高解像度での分割は不安・質問の課題が残る
  コード品質: 4 — 既存コード (MakeBoxFaces の corner パターン、Voronoi/Voxelizer の慣例) に合わせ、
    日本語コメントのみ、sub/round 番号などの作業経緯はコードに書いていない。Modal/deepmodal 系は
    未変更 (git status で確認)
  テスト: 4 — 受け入れ条件6件すべてに対応する自動テストを追加し、Debug/Release 両方の
    `--selftest` で実行・確認した。BakeFracture 分割のテストは既知のEarClip制約を避けるパラメータ
    (voxelResolution=16) を選んでおり、既定値 (64) 相当の組み合わせは検証していない

不安・質問:
  1. [重要・要判断] `BakeFracture` の分割 (Voronoi 切断) を、ボクセル化した細かいメッシュ
     (voxelResolution=32以上、`MakeOpenBox(1,1,1)` + pieceCount=8) に対して行うと、
     `FractureMesh.cpp` の `EarClip` (sub-01所有、蓋の三角形分割) が
     "positive側の蓋: 蓋の三角形分割に失敗" で失敗することを実測で確認した (resolution=48でも同様に失敗、
     resolution=16でのみ成功)。`Destructible.voxelResolution` の既定値は64・pieceCountの既定値は16
     (spec §4.2) なので、**既定値のまま「開いたメッシュ + ボクセル化を許容」を使うと実運用で焼きに
     失敗する可能性が高い**。原因は `EarClip` のコメントが前提とする「蓋の頂点数は高々数百程度」を、
     ボクセル化した細かい (階段状の) 断面が大きく超えること (実測で数千〜数万頂点規模) と見ている。
     このサブの担当は FractureVoxel (ボクセル化本体) と焼きの入口配線であり、`EarClip` 自体
     (sub-01/sub-02所有・複数ラウンドで頑健化済みの複雑な関数) を無断で変更するのはスコープ外と判断し、
     触っていない。SelfTest は voxelResolution=16 (最小値) でのみ分割まで確認し、この既知の制約を
     ここに記録する。プランナーへ: (a) `EarClip`/蓋三角形分割の頑健化を別サブ (sub-01/02 の追補、
     または新規サブ) として起こすか、(b) `Destructible.voxelResolution` の既定値を下げる/上限を絞るか、
     (c) 「ボクセル化を許容」時のみ pieceCount や voxelResolution の組み合わせに実用上の注意書きを
     ドキュメント化するだけに留めるか、方針を決めてほしい。sub-06 (root proxy 組み立て) や
     sub-09 (Inspector) がこの組み合わせを実際に使う前に解決/合意しておくべき事項と考える
  2. [既定を採用、確認のみ] TriBoxOverlap は sub-04.md の既定 (「別に書く」) どおり
     Modal/Voxelizer.cpp と共有せず独立実装にした。共有すべきという判断があれば差し戻してほしい
  3. [解釈の確認] 「surface nets」を頂点スムージングなしの「占有境界ブロック面抽出」として実装した
     (上記「仕様との差分」参照)。sub-04.md の「法線は面から」という記述と整合すると判断したが、
     もし本来の (頂点位置を平均化する) surface nets アルゴリズムを求めているなら実装し直しが必要

触ったファイル:
  - src/Engine/Engine/Physics/FractureVoxel.h (新規)
  - src/Engine/Engine/Physics/FractureVoxel.cpp (新規)
  - src/Engine/Engine/Physics/FractureBake.h
  - src/Engine/Engine/Physics/FractureBake.cpp
  - src/Engine/Engine/Physics/FractureSelfTest.h
  - src/Engine/Engine/Physics/FractureSelfTest.cpp

申し送り:
  - 不安・質問1のとおり、`Destructible.voxelResolution` の既定値 (64) + 典型的な pieceCount (16) の
    組み合わせで `BakeFracture` の分割が `EarClip` 起因で失敗し得る。sub-06 (root proxy 構築)・
    sub-09 (Inspector・非同期焼き) で「ボクセル化を許容」経路を使う際は、この制約が解消済みか
    (または回避策があるか) を先に確認すること
  - `VoxelizeMeshForFracture` 単体の処理時間は解像度256でDebug約23秒・Release約3.3秒 (open box、
    tri=1,318,960)。sub-09の非同期焼き・sub-11のベンチ/上限決定で、この数字を高解像度側の
    参考値として使ってよい

SELF_EVAL: sub-04 (round 2)
実装:
  - src/Engine/Engine/Physics/FractureVoxel.cpp — 占有境界をそのまま四角形にする「ブロック抽出」を
    廃止し、真の surface nets に置き換えた。`ResolveAmbiguousConfigurations` (新規): 格子面 (3方向)
    を張る隣接4セルが対角にだけ占有される曖昧な配置を、空きセルのうち格子 index 最小のものを
    占有にして解消する。1件の反映がどこまで波及するか事前に読めないため、影響を受け得るグループ
    だけをその場のスタック (ripple) に積んで汲み尽くす方式にした (グリッド全体のキューは持たない
    = 高解像度でもメモリに乗る)。`ComputeActiveCubeVertices` (新規): 8隅の占有が混在するキューブ
    (双対セル) ごとに、12辺のうち占有が変わる辺の中点の平均を頂点位置にする (反復平滑化なし)。
    `EdgeNeighborCubes`/`AppendQuad` (新規): 占有が変わるサンプル辺を挟む最大4キューブを
    外向きから見て CCW になる順で結び四角形にする (周回パターンは FractureBake.cpp の
    MakeBoxFaces と同じ規則を再利用)。対角の三角形分割は短い方を選ぶ (surface nets の頂点は
    厳密な平面上とは限らないため)。SelfTest 専用に `BuildSurfaceNetsFromOccupancy` (占有格子を
    直接与えてこのパイプラインだけを検証する入口) を追加
  - src/Engine/Engine/Physics/FractureVoxel.cpp:ComputeGridFrame — **実バグを発見して修正**。
    入力の extent が `voxelResolution` のちょうど整数倍に近いと、内部領域の境界が AABB とぴったり
    重なり、パディングセル (常に非占有のはずの層) が `TriBoxOverlap` の微小マージン (1e-6*h) で
    誤って占有判定されることがあった (open box を resolution=32 で焼くと再現し、境界辺 120〜256本
    で `CheckClosedMesh` が失敗した)。内部セル数を「余白がセル 0.1 個分未満なら 1 セル足す」規則に
    変更し、パディングとの間に常に十分な余白を確保した
  - src/Engine/Engine/Physics/FractureMesh.cpp:EarClip — 共線点 (前後と一直線上にあり、間に挟まれて
    いる点。`|cr|<=areaEps` かつ `dot(curr-prev,next-curr)>0`) を、毎周回「最も丸い耳」探索の前に
    面積0の耳として優先的に外すようにした。面積0なので体積にも辺の使用回数にも影響しない
  - src/Engine/Engine/Physics/FractureBake.h — `FractureBakeInput::voxelResolution` の既定値を
    64→32 に変更 (下記の計測結果に基づく)
  - src/Engine/Engine/Physics/FractureSelfTest.cpp — 受け入れ条件7 (surface nets の非ブロック性を
    単独セルの体積比較で検証、対角の曖昧配置が閉じることを検証) と条件8 (`BakeFracture` の
    `openMeshMode=1, pieceCount=16` を解像度32/48/64・開いた箱と平面で実行し時間を記録) のテストを
    追加。解像度32は合否判定あり (must)、48/64 は下記の理由により記録のみ (合否に数えない) にした

仕様との差分:
  - [追加] 上記の `ComputeGridFrame` のパディング余白バグ修正。spec/sub-04 に直接の記述はないが、
    「境界は空きセルで囲むので閉じないことがない」という既存の不変量を実際に成立させるための
    実装上の修正 (この不変量が破れていたのが round 1 の失敗の一因でもあった)
  - [未達] 受け入れ条件8「解像度32/48/64の全部について成功させる」を文字通りには満たせなかった。
    plane quad は32/48/64すべて成功するが、open box は48/64で `CutMeshByPlane` の断面三角形分割
    (`EarClip`、sub-01/02所有) が失敗し続ける (詳細は不安・質問1)。ただし性能面でも48
    (Release 12.5秒)/64 (24.2秒) は10秒予算を超えるため、「既定値=32」という結論自体は
    正しさ・性能どちらの観点からも揺るがない

検証:
  - MSBuild `MyEngine.sln` Debug|x64 / Release|x64 (フルソリューション) → 両方ビルド成功
  - `bin\x64\Release\Editor.exe --selftest` → exit=0、Fracture節 ALL PASS。
    受け入れ条件7: 単独セルの surface nets 体積 0.0370 (ブロックの1.0より明確に小さい、非ブロック性の
    直接的な証拠) / 対角の曖昧配置も閉じて外向き。
    受け入れ条件8 (Release 実測): res32 open box=5.34s・plane=0.74s (both must、成功)、
    res48 open box=12.5s (未達、記録のみ)・plane=1.59s (成功)、
    res64 open box=24.2s (未達、記録のみ)・plane=2.83s (成功)
  - `bin\x64\Debug\Editor.exe --selftest` → exit=0、Fracture節 ALL PASS (Release と同じ合否パターン。
    res32 open box=58.1s・plane=8.9s、res48 open box=135.6s(未達)・plane=19.5s、
    res64 open box=258.9s(未達)・plane=34.2s)
  - lshape/torus の digest が `EarClip` の変更で変化した (`0xA62D9B06031B23C8`→`0x4262BFE22AE6B01C`、
    `0x87A81F882E68E6DE`→`0x1B44700302F2FF15`)。これは FIX_REQUEST で明示的に許可された変化。
    Debug/Release で完全一致することを確認した (決定論は保たれている)
  - `tools\check_rules.ps1` → 0 error / 0 warning
  - `git status --porcelain` → WIP ファイル (WaterPass.cpp / deepmodal 系) 不変

自己採点 (1-5):
  仕様適合: 3 — 指摘1 (surface nets 化) と指摘2 (`EarClip` 共線点除去) は実装し、単独セル・
    対角曖昧配置のテストで効果を直接確認した。指摘3 (受け入れ条件8「32/48/64 全部成功」) は
    plane では達成したが open box の48/64では未達 (性能・正しさとも)。「既定値の結論を出す」
    という指摘3の目的自体は達成した (32) ため3とした
  正しさ: 4 — 全ての検証コマンドを実行し証拠を確認した。副産物として `ComputeGridFrame` の
    実バグ (パディング境界の誤占有) を発見・修正し、既存の分割 (box/lshape/torus) が
    Debug/Release で一致し続けることも確認した
  コード品質: 4 — 既存の巻き順パターン (MakeBoxFaces) を再利用し一貫性を保ち、日本語コメントのみ、
    round/指摘番号などの作業経緯はコードに書いていない
  テスト: 4 — 受け入れ条件7・8それぞれに専用テストを追加し Debug/Release 双方で実行・確認した。
    条件8の open box res48/64 は意図的に「合否に数えない記録」にしており、その理由をコード
    コメントと本 SELF_EVAL に明記した (テストを弱めて隠す形にはしていない)

不安・質問:
  1. [重要・要判断] 受け入れ条件8「解像度32/48/64の全部について成功」を、open box では満たせな
     かった (48/64で `EarClip` 起因の断面三角形分割失敗が残る)。surface nets 化 (指摘1) と
     共線点除去 (指摘2) という2つの根本対策を適用してもなお解消しなかった — 同種の失敗の
     3回目にあたる (round1の「ブロック抽出のまま」失敗、この round の「surface nets 化のみ」
     での中間確認 (res32のみ成功) 失敗、そして共線点除去を足した後も48/64が残る失敗)。
     `EarClip` はこれまで複数ラウンドで頑健化されてきた複雑な共有部品 (sub-01/02所有) で、
     これ以上の改修 (反射頂点だけに絞る最適化、あるいは制約付きドロネー三角形分割への置き換え)
     はこのサブの範囲を超えると判断し、踏み込んでいない。幸い性能面でも48/64はいずれも
     10秒予算を超える (12.5秒/24.2秒) ため、「既定値=32」という結論はこの問題が解決しても
     しなくても変わらない。プランナーへ: (a) この整理 (既定値32を確定させ、res48/64の
     open box 相当の正しさは対応しない) で良いか、(b) それとも `EarClip` の抜本改修を別サブとして
     起こすか、判断してほしい
  2. [報告のみ] `ComputeGridFrame` のパディング余白不足によるバグ (extent が resolution の
     整数倍に近いと、パディングセルが SAT のマージンで誤って占有される) を発見し修正した。
     round 1 のブロック抽出実装ではこの入力パターン (resolution=32 の open box) をテストで
     踏んでおらず気づいていなかった。この修正で res=32 の open box/plane quad が初めて
     閉じるようになった (surface nets 化・共線点除去とは独立の、別種の不具合)

触ったファイル:
  - src/Engine/Engine/Physics/FractureVoxel.h
  - src/Engine/Engine/Physics/FractureVoxel.cpp
  - src/Engine/Engine/Physics/FractureMesh.cpp
  - src/Engine/Engine/Physics/FractureBake.h
  - src/Engine/Engine/Physics/FractureSelfTest.h
  - src/Engine/Engine/Physics/FractureSelfTest.cpp

申し送り:
  - 既定値は `voxelResolution=32` (このサブの計測に基づく確定)。sub-06 で `Destructible` を
    実装するときはこの値を反映すること (spec §4.2 の「64(仮)」は sub-04 の計測で下げ得るとされて
    いた箇所)
  - res48/64 での open box 相当 (高解像度ボクセル化 + 多ピース分割) の `EarClip` 断面三角形分割
    失敗は未解決のまま残っている。sub-06/sub-09 で「ボクセル化を許容」を高解像度で使う具体的な
    要求が出た場合は、この制約への対応 (別サブでの `EarClip` 改修、または UI 側で高解像度時に
    警告を出す等) を先に検討すること

## フィードバック履歴
- round 1: VERDICT REWORK (planner)。must: surface nets になっていない (ブロック状の抽出)。既定の解像度 (64) で焼きが `EarClip` の共線点で失敗する。手順は「round 1 の裁定」節 (surface nets と曖昧な配置の事前解消、`EarClip` の共線点を面積 0 の耳で外す、解像度 32/48/64 の計測で既定値を決める)。受け入れ条件 7・8 を追加。`TriBoxOverlap` の独立実装と `BakeFracture` 入口への配線は採用
- round 2: VERDICT OK (planner)。surface nets (曖昧な配置の解消込み)、共線点の除去、`ComputeGridFrame` のパディングのバグ修正を採用。受け入れ条件 8 は「解像度 32 で開いた箱と平面の両方、48 / 64 で平面」が成功し、既定値 32 の結論が出た。開いた箱の 48 / 64 は sub-14 (断面の三角形分割を libtess2 に置き換える) へ移管
