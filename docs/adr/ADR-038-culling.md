# ADR-038: カリングの拡充 — 種類ごとのスイッチ、距離・画面サイズ、追加経路、デバッグ、BVH は見送り

- 状態: **確定** (2026-11-11、M99a〜M99i)
- 出所: M99 カリングの拡充。計画は `plans\m99-culling\spec.md`、判断の経緯は同じ場所の `harness.md` / `sub-NN.md`。仕様の正本は `engine_spec.md` §6.14。
- 先行: ADR-029 (M90 の視錐台・GPU オクルージョン・シャドウキャスター)。ここはそれに足すもので、置き換えない。

## 背景

視錐台カリング (M16) と GPU オクルージョンカリング (M90) は既にあった。足りなかったのは次の 4 点。

1. 種類ごとに切って、「カリングが絵を変えていない」ことを自分で確かめる手段 (視錐台はスイッチが無く、`cameraFound` と同じ旗だった)。
2. 広い屋外で遠い物・小さい物を落とす手段。
3. 視錐台判定の無い描画経路 (GPU パーティクル・トレイル・ビルボード・デカール・2D)。局所影のアトラスが画面外のキャスターを見ない穴。
4. 「なぜこの物が描かれていないか」を目で追う手段。

すべて描画専用で、sim・WorldHash・`.rep`・ABI (v35 のまま) に触れない。`RenderCulling` コンポーネントだけが TypeId 100 / SimSnapshot v82 を使う。

## 決定

| # | 決定 | 却下した案と理由 |
|---|---|---|
| D1 | 既存の視錐台・オクルージョンを活かし、切り替え口と穴埋めだけを足す | 作り直す案: 二重実装になる |
| D2 | 「カメラがある」(`cameraFound`) と「視錐台で落とす」を別の旗にする。OFF でも LOD・URO・地形・ライト選別は ON と同じに動く | 旗を共有したまま: LOD・URO・地形の収集まで止まり、OFF の絵が ON と別物になる |
| D3 | 視錐台判定は OFF でも計算し、CSM のフィット AABB には「視錐台の内側の物」だけを入れる。OFF は「落とさない」だけ。不変量は **ON と OFF の全画素一致** (`drawCalls` だけが増える) | OFF でフィットに全物を入れる: 影の解像度が変わり、一致を不変量にできない |
| D4 | 局所影のアトラスにも画面外のキャスターを描く (影付き局所光が選ばれたフレームで集める。タイルごとに視錐台で判定。CSM のフィットは変えない) | 直さずに「OFF も同じ穴」にそろえる: 修正前は `--culling-demo` で ON/OFF が 12360 画素ずれた (= 穴の大きさ) |
| D5 | 物ごとの距離は新しい描画専用コンポーネント `RenderCulling` (`kComponentNoHash`、`maxDrawDistance` / `exemptFromSizeCulling`) | `MeshRenderer` に足す: 全シーンの MeshRenderer カラムが伸びる。タグ表と既定値だけで済ませる: 物ごとの上書きが無い (ユーザーが RenderCulling を選択) |
| D6 | 距離は**カメラ位置からワールド AABB の最近点まで**の球状の距離 (内側なら 0) | カメラ平面基準 (Unity の既定): カメラを回すだけで画面端の物が出入りする |
| D7 | 実効距離 = {物ごと, 物が持つ各タグ, プロジェクト既定} のうち 0 より大きい値の最小 (UE の Cull Distance Volume が重なったときと同じ) | 優先順位を固定する案: 「タグで 200 m にしたのに既定 50 m で消える」と「物ごとに 500 m にしたのに既定が効かない」のどちらかが起きる |
| D8 | 距離・画面サイズで落とした物は影も消す (画面外キャスターのリストにも入れない) | 影だけ残す: 遠い・小さい物を落とす目的 (コスト削減) が果たせない |
| D9 | 距離・画面サイズは viewKey 1 (Runtime) と 3 (Game View) に掛ける。Scene View (2) は既定で掛けず、セッション限りのトグルで掛ける。プレビュー・プローブ (0) には掛けない | Scene View にも常に掛ける: 編集中に遠くの物が消えて選べなくなる (UE も通常のビューには掛けず Game View で確かめる) |
| D10 | 画面サイズは `BoxScreenSize` (LOD と同じ式、`lodBias` なし) を閾値と比べる。既定 OFF・閾値 0.005 (1080p で約 5 px)。点滅防止に LOD と同じ 10% のヒステリシス (ビュー別) | 別の物差し: LOD の段と食い違う。既定 ON: 既存の絵が変わる |
| D11 | 距離にヒステリシスは付けない | 距離はカメラ位置だけで決まり、止まれば揺れない |
| D12 | GPU パーティクルの箱は記述子から解析的に上界を出す (形状の広がり + `speedMax·T + ½|g+w|·T²` + ビルボードの膨らみ)。乱流・かき乱し・速度継承・深度/解析床の衝突・ワールド空間の原点移動のあるエミッタは「上界を持てない」= 常に描く | 粒子位置の readback: GPU の進み具合で遅れが変わり、`counts` の決定性 (Debug / Release / WARP 一致) を壊す。物に固定 Bounds を持たせる (UE 方式): コンポーネントが伸びる |
| D13 | 視錐台を足す経路: GPU パーティクル・トレイル・SpriteRenderer・デカール・Sprite2D / Line2D / SpriteShape2D。足さない経路: TextMesh (広がりは組版後)、水面 (巨大な 1 枚)、2D マスク (画面外が画面内に効く) | 全部に足す: 落とす利益が小さく、見落とす危険が大きい |
| D14 | 2D の判定は既存のタイルマップと同じ `OrthoWorldBounds` の矩形で行う。透視カメラ・視差の繰り返し項目は落とさない | 別の判定を新設: タイルマップとの食い違い |
| D15 | 視錐台スイッチの対象は メッシュ・CPU パーティクルのプール・タイルマップのチャンク・D13 の経路。**地形は対象外** (チャンク選択と一体なので今と同じく常に判定する) | 地形も対象にする: チャンク選択の構造を変えることになる |
| D16 | BVH は見送り (下の計測) | 計測を待たずに入れる |
| D17 | ON/OFF は切り替えた瞬間に実行中の RenderSystem へ反映し、`project_settings.json` へも書く (オクルージョンと同じ)。数値欄は編集を終えたとき書く。CLI の `--no-*` はその起動中だけでファイルを書かない | 保存ボタン方式: オクルージョンの既存の挙動とずれる |
| D18 | カリング用カメラの固定は視錐台・距離・画面サイズ (CPU 側の判定) にだけ効く。オクルージョン・ライト選別・CSM・LOD・地形は描画カメラのまま | オクルージョンも固定する (UE の `FreezeRendering`): 固定した視点の深度ピラミッドを別に持つのは費用に見合わない |

判定の順序は 視錐台 → 距離 → 画面サイズで、先に当たった理由を 1 つだけ数える。数 (`culledDistance` / `culledScreenSize` / `culledNonMesh` / `unculledGpuEmitters`) は `prof::RenderStats` と `--render-stats-dump` の `counts` に足した。既存の `culled` は「視錐台で落としたメッシュ数」のまま意味を変えない。

### 公式情報 (2026-11-11 取得)

- UE: https://dev.epicgames.com/documentation/en-us/unreal-engine/visibility-and-occlusion-culling-in-unreal-engine (Min/Max Draw Distance、Cull Distance Volume、`FreezeRendering`、`r.VisualizeOccludedPrimitives`、`stat initviews` の理由別の数)
- UE3: https://docs.unrealengine.com/udk/Three/VisibilityCulling.html (重なった Cull Distance Volume は最も厳しい値)
- Unity: https://docs.unity.com/en-us/engine/current/script-reference/unityengine/camera/layerculldistances (0 = far、`layerCullSpherical`)
- Unity: https://docs.unity3d.com/6000.4/Documentation/Manual/class-LODGroup.html (Culled 段は画面高さ比)

### 検証の作り

- 視錐台 ON/OFF の全画素一致を `tools\cull_verify.bat` が回す (既定ベンチ・`--culling-demo`・`--culling-2d-demo`・`--particle-demo`・`--2d-demo`・`--render-demo`、tol 0)。固定の検査 (`--cull-freeze-frame` + `--render-bench-cut-frame`) と、`counts` の構成間一致 (Release `--warp` / Release / Debug) もここに同居する。
- 局所影の穴は `--culling-demo` が捕まえる (修正前は ON/OFF が 12360 画素ずれる)。距離・画面サイズは `--culling-distance-demo` と `DistanceCullSelfTest` (判定関数)。

## 計測 (BVH の採否)

### 目的と採用条件

空間分割 (BVH) を入れるかを、入れる前に測って決める (AGENTS §3.5)。採用条件は spec 4.1.8:
Release・実 GPU・開発機で、K = 16 (約 58k 個) の中央値 (`--shot-frame 300` を 5 回) が
**ステージ 2 の視錐台判定部分 >= 1.0 ms かつ `CollectDrawables` 全体の 30% 以上**。

### 方法

- 計る場所は `RenderSystem::CollectDrawables` (メッシュの収集)。3 段:
  ステージ 1 = 直列の候補収集 (ECS の走査と `GetComponent`)、ステージ 2 = 並列の判定 (LOD・URO・viewZ・視錐台)、
  ステージ 3 = 直列のキュー化。全体 (total) は地形・デカール・水面・スキンのパレットの評価も含む。
- 視錐台判定だけの時間 (`collectFrustum`) は、ステージ 2 の直後に**判定だけを同じ粒度で並列に回し直す計測専用パス**で取る
  (`--render-stats-dump` の run だけ有効。結果は捨てて出力へ書かない)。ステージ 2 の中の判定を切り出すと
  並列の粒度と LOD / URO の計算が変わるため、この形にした。直後なのでキャッシュが温まっており、ステージ 2 内の判定よりやや速く出る。
  下の判断はステージ 2 全体 (判定 + LOD + URO + viewZ) でも満たさないので、この偏りで結論は変わらない。
- 値は直近 32 回の `CollectDrawables` の平均 (`cpuMs.gbufferSubmit` と同じ規約)。dump の `cpuMs` は `counts` ではなく参考値で、ゲートにしない。
- 物量は `--render-bench-scale K`。元のグリッド (61 x 60 = 3660 個) の複製を (K - 1) 枚、元のグリッドの奥 (+Z) へ 4 列ずつ並べる。
  K = 1 のシーンは従来と 1 エンティティも変わらない (複製は生成順の末尾)。K = 16 で不透明候補 58586 個。
- コマンド: `bin\x64\Release\Editor.exe --render-bench-demo --render-bench-scale K --render-stats-dump <file> --shot-frame 300` を K = 1 / 4 / 16 各 5 回。
  ★`cache\render_bench.scene.json` が残っているとシーンを組まずにそれを読む (K が効かない)。計測の前に無いことを確認すること。
- 機材: Intel Core i5-10400 (6 コア 12 スレッド、2.9 GHz) / NVIDIA GeForce RTX 3060 / Windows 11。
  Release|x64、Editor.exe の Scene View カメラ (画面解像度は既定)。

### 結果 (中央値 [ms]、括弧は 5 回の最小〜最大)

| K | 不透明候補 | total | ステージ 1 (直列) | ステージ 2 (並列) | うち視錐台判定 | ステージ 3 (直列) |
|---|---|---|---|---|---|---|
| 1 | 3686 | 0.614 (0.592〜0.652) | 0.273 (0.265〜0.282) | 0.057 (0.054〜0.071) | 0.041 (0.037〜0.043) | 0.244 (0.229〜0.251) |
| 4 | 14666 | 3.050 (2.909〜3.298) | 1.755 (1.681〜1.932) | 0.195 (0.188〜0.224) | 0.144 (0.127〜0.157) | 0.942 (0.877〜0.988) |
| 16 | 58586 | 12.725 (12.615〜12.920) | 7.862 (7.762〜7.923) | 0.648 (0.608〜0.680) | 0.477 (0.445〜0.518) | 3.724 (3.682〜3.930) |

K = 16 の割合: 視錐台判定 / total = 3.7%、ステージ 2 全体 / total = 5.1%、ステージ 1 / total = 61.8%、ステージ 3 / total = 29.3%。
`counts` は各 K で 5 回とも完全に一致 (決定的)。K = 1 の `counts` は M99a 着手前の exe の dump と完全に一致した。

### 判定: BVH は見送り

- 採用条件の視錐台判定 >= 1.0 ms: K = 16 で 0.477 ms (ステージ 2 全体でも 0.648 ms) = **満たさない**。
- 全体の 30% 以上: 3.7% = **満たさない**。
- 支配的なのはステージ 1 (全体の 62%)。ECS の直列走査と `GetComponent` の費用で、BVH があっても減らない。
  ステージ 2 は 12 スレッドの並列で、58k 個でも 0.65 ms で済んでいる。
- **BVH で削れる時間の上限**は、この構図 (ほとんどが視錐台の内側) での線形の判定時間 0.477 ms。判定は箱ごとに早期脱出する
  (最初に外れた平面で打ち切る) ので、視錐台で落ちる物が多い構図ほど線形の判定は安くなり、BVH の取り分はさらに小さくなる。
  BVH を入れても、ステージ 1 と 3 (合わせて全体の 91%) は変わらない。
- よって BVH のサブは足さない。58k 個を超える規模で CPU が問題になったら、先にステージ 1 (候補の収集を並列化する、
  変化のない物をキャッシュする) を測り直す。
- 補足: K = 16 でも視錐台で落ちた物は 2436 個にとどまり (`culled`)、残りの多くは GPU オクルージョンが落とした (`occluded` 47893)。
  シーンのカメラの `farZ = 400` より奥の複製が落ちていないので、Editor の撮影は別のカメラ (far が遠い) を使っていると推定される (未確認)。
  この結果は「ほとんどが視錐台の内側」の構図での値で、視錐台で大半を落とす構図は未計測 (上のとおり、その構図では線形の判定がさらに安い)。

## 結果

- 既定設定 (新キー無しの `project_settings.json`) では、既存の golden と既定ベンチの `counts` (新しい欄を除く) は M99 前と同じ。距離は既定 0 (無制限)、画面サイズは既定 OFF。
- 局所影の画面外キャスターは正しく直った結果として絵が変わりうるが、既存の golden は動かなかった。
- 既知の制限 (M99 では直さない): (a) 画面内に不透明の物が 1 つも無い (`hasScene == false`) フレームは局所影アトラスを作らないので、画面外キャスターの影も出ない。(b) `ShadowAtlas::Render` は各物をメッシュの bind 姿勢の AABB でタイル判定するので、スキンの姿勢が bind の箱の外へ出た部分は局所影で欠けうる (画面内のスキンも同じ、M90 からの挙動)。
- 実体: `Engine\Rendering\{CullingSettings,RenderSystem,RenderStatsDump,DistanceCullSelfTest}`、`Engine\Particles\GpuParticleBackend` (上界)、`Engine\Rendering\Sprite2D\Sprite2DRenderer` (2D)、`Engine\Demo\DemoContentRender.cpp` / `DemoContent.cpp` (`--culling-demo` / `--culling-distance-demo` / `--culling-2d-demo`)、`Engine\App\EngineCli.cpp`、Editor の Rendering > カリング / Project Settings。
