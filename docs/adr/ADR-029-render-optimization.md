# ADR-029: 描画の軽量化 — LOD はアセット単位のオプトイン、オクルージョンは 2 フェーズの max-Z、URO と影のカリングは描画側だけ

- 状態: **確定** (2026-10-09、M90a〜M90i)
- 出所: M90 描画の軽量化。計画は `plans\m90-render-optimization\spec.md`、判断の経緯は同じ場所の `harness.md` / `sub-NN.md`。
- 実体:
  - 計測: `Engine\Rendering\RenderStatsDump.{h,cpp}`、`Core\Diagnostics\Profiler.{h,cpp}`、`Renderer\Device\GpuTimer.h`、`Engine\Demo\DemoContent.cpp` の `BuildRenderBenchScene`。
  - LOD: `Engine\Asset\MeshLodBuilder.{h,cpp}`、`Engine\Asset\ModelCook.{h,cpp}`、`Engine\Asset\CookedCache.h`、`Renderer\Mesh\MeshLod.h`、`Renderer\Device\GpuResources.{h,cpp}`、`Core\Asset\ImportMetaResolver.{h,cpp}`、`external\meshoptimizer\`。
  - オクルージョン: `Renderer\Passes\OcclusionCullPass.{h,cpp}`、`OcclusionMath.h`、`HzbPass.{h,cpp}`、`HzbDebugPass.{h,cpp}`、`assets\shaders\occlusion_cull.cs.hlsl`、`hzb_reduce_max.cs.hlsl`、`Renderer\Pipeline\DeferredPath.cpp` / `ForwardPath.cpp`。
  - 影・スキン・URO: `Renderer\Pipeline\FrustumCull.h`、`Renderer\Passes\ShadowPass.cpp`、`Renderer\Mesh\SkinBounds.{h,cpp}`、`Engine\Rendering\SkinPaletteCache.{h,cpp}`、`Engine\Rendering\RenderSystem.{h,cpp}`。
  - 設定: `Engine\Scene\TagNames.{h,cpp}` (`Load/SaveOcclusionCullingSetting`)、`Engine\Loop\EngineLoop.cpp`、`Engine\App\EngineCli.cpp`、`Editor\App\EditorApp.cpp` (Rendering メニュー)。
  - sim の並列化は別の決定: [ADR-028](ADR-028-sim-parallelism.md)。
- 番号: `kCookVersion` 5 → **6** (LOD の段表が blob に入る)。ABI (v28)・TypeId (80)・SimSnapshot (v48)・`.rep`・シーン JSON は**変更なし**。
  描画の設定 (オクルージョン・URO・lodBias) は RenderSystem のメンバで、GameLogic へ出す API は無い。

## 背景

物が多いシーン (三校の街・群衆・破壊の破片) で、見えない物と細部が見えない遠景に GPU と CPU を使いたくない。
ただしこのエンジンの契約 (sim のビット一致、決定的撮影、golden) を崩さないことが前提で、
「効いたか」は数字で示す (AGENTS §3.5)。方針は次の 4 つ。

1. **sim の状態に入れない。** LOD・オクルージョン・URO・影のカリングはすべて描画側の最適化で、ハッシュに入らない。
   カメラは sim の入力ではないので、カメラ距離で何かを変えるものを sim に入れるとリプレイ・ネット対戦が割れる。
2. **既定の絵を 1 ビットも変えない側に倒す。** LOD はオプトイン、オクルージョンは保守的、URO は閾値より近い物は毎 tick。
3. **欠けない側に倒す。** 判定が怪しいとき (ニア面をまたぐ、画面からはみ出す、履歴がない) は描く。
4. **性能のゲートは決定的な数だけ。** CI は WARP で、WARP の ms は実機の効果を表さない。ms は参考値。

## 決定 1: 計測を先に作り、決定的な数と ms を分ける

- GPU ms は `GpuTimer` で GBuffer / Forward 不透明 / CSM / 局所影アトラス / オクルージョン / Render 全体を計る。
  「Render 全体」は `RenderSystem::Render` 1 回 (ビュー 1 本) 分で、フレーム全体ではない。ProfilerWindow に既存の段と並べて出る。
- `prof::RenderStats` はビュー (viewKey) 別に集計し、従来の累積値も残す。影の draw / tri は本描画の欄と別欄。
  LOD 段ごとの描画数と三角形数、オクルージョンの phase1 / phase2 / occluded、URO のパレット評価数・再利用数がある。
- `--render-stats-dump <file>` (+ `--shot-frame N`): 決定的撮影と同じ固定条件で `N` フレーム描き、最後のフレームの統計を JSON に書いて終了する
  (終了コード 6 = 書けなかった)。JSON は **`counts`** (決定的な数。Debug / Release / `--warp` で一致する)、**`gpuMs`** (参考値)、**`cpuMs`** (CPU の提出時間。同じく参考値) の節に分かれる。
  回帰の比較は `counts` だけで行う。
- `render_bench` シーン (`--render-bench-demo`): 3660 個のグリッド、遮蔽する壁、遠景の球、LOD 球、スキンのキャラ 7 体、画面外の影キャスター (Tower)。
  `--render-bench-cut-frame N` は描画側のカメラ上書きでカメラカットを再現する (sim には触れない)。
  `--render-bench-unique-demo` は「メッシュ・材質が全部別の 1500 個 + 遠いスキン 6 体」の変種で、**インスタンシングが効かず draw が多い**ときの CPU 提出コストを見るためにある。
- `drawCalls` / `triangles` は **CPU が提出した論理数**。オクルージョンが GPU で間引いた分は引かない (ON/OFF と各構成で同じ値になる)。間引いた効果は `occlusion*` 欄で見る (GPU のカウンタを 2 フレーム遅れで読む。統計にだけ使い、描画判断には使わない)。

## 決定 2: メッシュ LOD は「モデルの `.meta` でオプトイン」、頂点は共有して IB の範囲だけ増やす

### 2-1. オプトイン

既定は段なしで、LOD を設定していないモデルは今と 1 ビットも同じ (golden が動かない)。UE の Static Mesh LOD と Unity 6 の Mesh LOD もアセット単位の設定、という前提による
(公式ドキュメントでの照合は未実施で、記憶による)。全メッシュを既定 ON にすると、遠景にメッシュがある golden が全部動き、見た目の責任をアセットに戻せない。

`.meta` の形 (段なしなら `"lod"` を書かない = 既存の `.meta` はバイト一致):

```json
"lod": { "levels": 3, "ratio1": 0.5, "ratio2": 0.25, "ratio3": 0.125, "screenSize1": 0.3 }
```

- `levels`: LOD0 に足す段数 0..3 (最大 4 段)。`ratioN`: その段の目標三角形比 (0.01..0.95、既定 0.5 / 0.25 / 0.125)。
- `screenSizeN`: その段へ落ちる画面高さ比 (省略または 0 = 自動)。自動は `0.5 * sqrt(ratio)` (0.354 / 0.25 / 0.177) を前の段の 0.8 倍以下に頭打ちした値。
- `ModelLodSettings::Normalize` が範囲に収める。blob に書く値が 1 通りになり、比較が安定する。
- エディタでは Inspector のモデルアセットから設定でき、適用すると `.meta` を書いてメッシュを再読み込みする (アセットブラウザの右クリックには出していない)。

### 2-2. 生成 (`MeshLodBuilder`、meshoptimizer v1.3)

- ソースは `external\meshoptimizer\` にそのままコミット (改変なし、`external\VERSIONS.md` に追記、`Common.props` の `/fp:precise` を継承)。
- 先に頂点を溶接 (`meshopt_generateVertexRemap`)。FBX は三角形ごとに頂点を持つので、溶接しないと何も減らせない。
- 段ごとに `meshopt_simplifyWithAttributes` (法線・UV、スキンは骨ウェイトも属性、重み 0.5) を `meshopt_SimplifyLockBorder` で掛け、結果を**元の頂点を指す index**へ戻す。誤差上限は 0.05。
- 目標の 1.25 倍以内に届かない段、前の段より減らない段は作らない (段数が減る)。最初の段から作れなければ段なしで登録し、WARN を key ごとに 1 回 (届いた数と目標つき)。
- 生成は決定的 (同じ入力から同じバイト列)。フレッシュパースとクックキャッシュの再生は同じ登録地点 (`ModelCook::RegisterMeshWithLods`) を通る。

### 2-3. 持ち方: 共有 VB + IB の範囲

`MeshVertex` (52B) は変えない。`Mesh` の IB は LOD0 の後ろに LOD1 以降を連結し、`Mesh::lods[k]` が `{indexOffset, indexCount, screenSize}` を持つ。
描画は `Mesh::LodRange(lod)` の範囲で `DrawIndexed*` する。スキンも頂点を共有するので VS スキニングはそのまま動く。
`Mesh::indices` / `indexCount` は LOD0 のままで、**コライダー・NavMesh・RT の BVH・MeshLibrary の CPU コピーは LOD0 を使い続ける**。
Picking・Ghost・Terrain・Water は LOD0 のまま。

却下した案:

- 段ごとに別の頂点バッファを持つ: `meshopt_simplify` は元の頂点を指す index を返すので頂点を複製する理由がない。手作りの LOD (glTF `MSFT_lod`、FBX の LOD グループ) を後で読むときに VB を足す余地はあるが、v1 は共有 VB 固定。
- dither / crossfade での切り替え: GBuffer に discard が入り、TAA との相性も見る必要がある。v1 はヒステリシス付きの即時切り替え。

### 2-4. クックキャッシュ (`kCookVersion` 6)

blob の先頭に `.meta` の LOD 設定 (`ModelLodSettings`、28 バイトの POD) が入り、メッシュごとに `lodIndices` (LOD1 以降の index 列) と `lods` (段表) が続く。
旧 blob はこの並びで読めないので版で弾く。

`CookedCache` の無効化はソースの size / mtime / 内容ハッシュと deps しか見ず、`.meta` を見ない。
そこで **LOD 設定そのものを blob に記録し、読み込み時に現在の `.meta` と比べて違えば再クックする** (`ModelCook::TryReplayFromCache`)。
キャッシュの手動削除を要求しない。封印キャッシュ (`.sealed`) は比べない (配布物は exe とキャッシュを一緒に出す)。

### 2-5. 選択式 (`MeshLod.h` の純関数)

- `LodScreenSize`: ワールド AABB の外接球が画面の高さに占める割合 (`radius * proj._22 / distance`、正射影は `radius * proj._22`)。URO も同じ式を使う (`BoxScreenSize`)。
- `SelectLod`: `screenSize * lodBias` が `lods[k].screenSize` を下回ると段 k。**`lodBias` は screen-size 側に掛ける** (> 1 で詳細な段を長く使う。Unity の `QualitySettings.lodBias` と同じ向き)。
- ヒステリシス `kLodHysteresis` = 10%: 前フレームの段から粗くするときは閾値の 0.9 倍を下回るまで、細かくするときは 1.1 倍以上になるまで切り替えない。
  前の段は viewKey ごとに `LodHistory` へ持つ (エンティティの index と世代で引く)。履歴の無いビュー・初回は閾値だけで決まる。
- 強制段 `lodForcedStage` (-1 = 自動 / 0.. = その段、無ければ最も粗い段) はデバッグ用。
- 影のキャスターもそのエンティティのカメラ基準の段を使う (画面外のキャスターも同じ式)。
- 段の選択は `CollectDrawables` のステージ 2 (並列、履歴は読むだけ)、履歴の書き込みはステージ 3 (直列)。

### 2-6. 知っておく限界

- **硬い面のメッシュ** (平面と鋭い辺だけの箱・パネル。例: `Lab_Door.fbx`) は、既定 (`LockBorder`、非 Permissive) では目標まで減らず、段が作れない (段なし + WARN)。
  `meshopt_SimplifyPermissive` を使えば継ぎ目をまたいで潰せるが、法線・UV が崩れうるので入れていない。三校の素材に段を付けるなら、比を緩めるか Permissive をオプトインの設定に足す必要がある (v1 の範囲外)。
- LOD の切り替えのポップと、法線・UV の見た目はユーザーの目視で確認する (`docs\test_checklists.md`)。
- テスト素材: `tools\gen_lod_test_gltf.ps1` が `assets\models\lod_sphere.glb(.meta)` を生成する (3 段)。`cache\render_bench.scene.json` が古いと LOD 球が出ない。
- CLI: `--lod-bias F`、`--lod-force N`。

## 決定 3: GPU オクルージョンは 2 フェーズ、max-Z の HZB を viewKey ごとに持つ

### 3-1. 方式

対象は viewKey 1/2/3 の**不透明**の本描画 (Deferred の GBuffer と Forward の不透明)。CPU の視錐台カリングは残り、通った物だけが判定に入る。

1. **フェーズ 1**: 前フレームに可視だったインスタンス (と、判定せず常に描く物) を描く。
2. 地形を描き (遮蔽物にするため)、その深度から **max-Z の HZB** を作る。
3. **フェーズ 2**: 全インスタンスの AABB を compute で HZB と判定し、フェーズ 1 で描いていない可視の物を詰めて描く。判定結果を可視ビットとして次フレームへ残す。

- 描画は run (インスタンシングの塊) ごとの `DrawIndexedInstancedIndirect`。compute が可視インスタンスの index を詰め、`instanceCount` を書く。
  CPU への読み戻しで描画を決めない。ボーン付きなどインスタンシングできない項目は単発の indirect。
- **詰め込みは prefix sum** (順序を保つ)。アトミックで詰めると、同じ深度で重なる物の描画順が実行ごとに揺れる。
- **max-Z**: このエンジンは reversed-Z を使わない (深度は 1.0 クリア、小さいほど手前)。そのため「ある領域で最も奥」が max で、箱の最前面がそれより奥に隠れていれば隠れている。
  既存の SSR 用 min-Z ピラミッド (`HzbPass`) の縮小カーネル (奇数辺を広げて取りこぼさない `HzbReduceSpan` の分割規則) と mip の作り方を流用し、演算だけを `hzb_reduce_max.cs.hlsl` で max にした。
  オクルージョン用は別インスタンスを viewKey ごとに持ち、SSR 用の既存ピラミッドは 1 ビットも変えていない。
- 可視ビットは viewKey ごと・エンティティごと (`entity.index`、`PrevRenderWorldStore` と同じ流儀の安定スロット) に持つ。新しく出たエンティティは前フレーム不可視として扱われ、フェーズ 2 で判定される (欠けない)。
  無効なエンティティ・巨大な index は履歴を持たず常に描く。
- **保守側の規則**: ニア面をまたぐ (w が 0.001 以下の頂点がある、最前面が深度 0 未満) / 余白込みの画面矩形が画面からはみ出す、のいずれかは HZB を引かず可視。
  深度の比較は 24bit 深度の 32 刻みぶん甘くする (厚みの無い箱が自分自身を隠れていると誤判定しない)。矩形に 1 画素の余白。
  判定の定数は CPU の鏡 (`OcclusionMath.h`) と HLSL が持ち、整数のものは `tools\check_rules.ps1` が機械照合する。
- **判定に載せない物**: サーフェスマテリアル・水面など別経路で描く物は常に描く。**スキンは常にフェーズ 1** (保守的 AABB が IK で外へ出うるので、欠けのリスクが影より目立つ。M90 の範囲外)。
  影・半透明は対象外 (影は決定 4 のカスケード別カリングで賄う)。
- viewKey 0 (AssetPreview・履歴なし) と ProbeBaker は OFF。履歴 (可視ビット) が使えるのは、同じサイズで、ビュー別の描画通番が前回 + 1 のときだけ。リサイズ・通番の飛び・デバイス消失 (M88) の後は全部を前フレーム不可視として扱う (欠けない側)。
カメラカットは履歴を捨てなくてよい (フェーズ 2 が今フレームの深度で判定し直す)。
- **統計の読み戻し**: GPU が数えた統計 (phase1 / phase2 / occluded) は 3 本のステージングのリングへ写し、2 フレーム遅れで読む。統計にだけ使い、描画判断には使わない。
  読む呼び出し (`OcclusionCuller::PollStats`) は CPU 提出の計測区間 (`cpuMs.gbufferSubmit`) の外に置く。
  対話の描画は `D3D11_MAP_FLAG_DO_NOT_WAIT` で読み、終わっていなければ前の値を残す (待つと GPU の完了までフレームが止まる。review-1 #1 で、旧実装がフラグ 0 で待っていて、ON の CPU 提出が解像度に比例していたと分かった)。
  決定的な撮影 (`--screenshot` / `--render-stats-dump`) と selftest は待つ。待たないと counts が GPU の進み具合で揺れ、Debug / Release / WARP の一致が崩れる。切り替えは `RenderSystem::occlusionStatsWait` (既定 true、対話の `EngineLoop` が false にする)。

### 3-2. 却下した案: 1 フェーズ (前フレームの HZB を再投影)

サブが 1 本軽くなり、フェーズ 2 の提出コストも無い。しかし**ディスオクルージョンとカメラカットで 1 フレーム欠ける** (前フレームの深度に写っていなかった物が、再投影した HZB では隠れて見える)。
AGENTS §3.6 は正しさを速度より上に置くので採らない。2 フェーズは前フレームの可視性を「描く順序のヒント」にしか使わず、最終の判定は今フレームの深度で行う。
そのためカメラカットの直後でも欠けず、遅くなるだけ (実測: カットのフレームは phase2 が 2207 個に増える)。
**ただし描く順序は OFF と変わる**: カットのフレームの可視ビットは古いカメラのものなので、フェーズ 1 は「古いカメラで見えていた物」から先に描く。同じ深度で交わる面 (交線上の画素) は勝つ物が入れ替わりうる (z-fight)。
実測は 3-6 に書く。UE の旧 HZB オクルージョンは遅延付きクエリ、Nanite と Unity 6 の GPU occlusion culling は 2 パス、と記憶している (一次資料は未確認)。

読み戻しで run を飛ばす案 (数フレーム遅れの可視ビットで、フェーズ 2 に候補が無い run を CPU が省く) も採らない。新しく見えた物の run を飛ばすと欠ける。

### 3-3. リソース確保の失敗は局所化する

HZB / indirect のリソース作成に失敗したら、オクルージョンだけを全ビューで OFF にしてログを 1 回出し、描画は従来の経路で続く (Shutdown で復帰)。
HZB を作れないフレームは判定せず全部描き切り、次のフレームから OFF。selftest は失敗の注入で確かめる。

### 3-4. 切り替え口と優先順位

| 口 | 効き方 | 保存 |
|---|---|---|
| `RenderSystem::enableOcclusionCulling` (既定 true) | 実行時のスイッチ | しない |
| `assets\project_settings.json` の `"rendering": {"occlusionCulling": bool}` | Editor と Runtime が起動時に読む。**キーが無い・壊れたファイルは true** (壊れたファイルは上書きしない)。他のキー (`rayTracingTags` 等) は保持される | する |
| CLI `--no-occlusion` | 起動中だけ OFF にする | **しない** (ファイルを書き換えない) |
| エディタの Rendering メニュー | その場で切り替え、プロジェクト設定へ**保存する** | する |

- 起動時の値は「ファイルが true かつ `--no-occlusion` でない」。`--no-occlusion` はファイルより優先して OFF にするが、**CLI で強制 ON にする旗は無い** (ファイルが false なら false)。
- `--no-occlusion` で起動したエディタでメニューを操作すると、メニューで選んだ値が保存される。メニュー操作はユーザーの明示的な選択で、CLI は起動中の上書きにすぎない。
- プレイヤー向けのオプション画面と GameLogic の API は無い (ABI を上げない)。RT のタグ規則 (`rayTracingTags`) と同じく、設定の置き場は `project_settings.json`。
- URO・lodBias・強制 LOD 段は保存しない (起動ごと)。

### 3-5. デバッグ表示

`--hzb-debug N` に `--hzb-debug-max` を足すと max-Z ピラミッドを表示する (`HzbDebugPass`)。落とした物の AABB は赤線 (onTop) で描く。

### 3-6. カメラカットのフレームの画素差 (z-fight)

`--render-bench-demo --warp --width 960 --height 540 --frames 32 --shot-frame 30 --render-bench-cut-frame 30` の ON と `--no-occlusion` を `--img-diff --tol 0` で比べると、**11 画素が違う** (maxDiff 47。位置は x=363〜365 / y=325〜331、LOD 見本の球とグリッドの球が交わる線の上)。カットの次のフレーム・カット後の定常・カット無しは差 0。

原因は描く順序による同深度の勝ち負け (z-fight) と断定した。観測:

- 全メッシュを LOD0 に固定 (`--lod-force 0`) すると差 0。LOD1 に固定 (`--lod-force 1`) すると同じ 11 画素が違う。LOD の段を選び間違えているなら固定した段では差が出ないはずなので、段の取り違えではない。
- 一時的に、可視ビットの履歴を捨てて (全部「前フレーム不可視」にして) カットのフレームを撮ると、ON は `--no-occlusion` と全画素一致 (diffPixels=0)。フェーズ 1 を空にして、フェーズ 2 だけで OFF と同じ順序・同じ内容で描いたときだけ一致するので、差の原因は描く順序だけ (一時コードは削除済み)。
- 通常は、カットのフレームの可視ビットが古いカメラのものなので、フェーズ 1 が「古いカメラで見えていた物」から先に描き、フェーズ 2 が残りを描く。OFF は collect の並びで描く。この順序の違いで、交線上の同深度の画素で勝つ物が入れ替わる。

許容する理由: 同深度の面の勝ち負けは描く順序の関数で、OFF 自身も別の並び (ソートキー) に変われば変わる。ON は「描く物の集合」(欠けない) を保っており、違うのは交線上の数画素の描画順だけ。カメラカットの 1 フレーム限りで、次のフレームから一致する。
履歴を捨てれば消せるが、エンジンはカットを知らない (カメラ上書きは render_bench の再現用) ので、実用上のカットで履歴を捨てる口を足すのは見合わない。

`OcclusionSelfTest` は LOD 付きのメッシュ (LOD1 / 段数を超える指定 / LOD0 が混ざる run) を入れて、履歴なし・定常・カット直後・通番の飛びで ON/OFF の全画素一致を確かめる (交線に同深度が出ない配置)。

## 決定 4: 影はカスケードごとにカリングし、スキンは保守的な AABB で落とす

### 4-1. 影のキャスター

従来、影のキャスターはカメラの視錐台でカリングした後の `queue_.opaque` から取っていたので、**画面外のキャスターの影が消える**不具合があった。
キャスター候補をカメラの視錐台から切り離し、カスケードごとにライトの直交視錐台で判定する (`RenderCascadeShadows`、キャスター単位で並列)。

- 判定は**ライト側の近平面を除く 5 面**で行い、CSM の描画だけ `DepthClipEnable = FALSE` (深度クランプ。pancaking) のラスタライザを使う。
  CSM の zNear は画面内の物とカメラのスライスだけから決まるため、ライト側へ離れた画面外キャスター (街の高い建物) は 6 面判定では落ち、一部がはみ出す物は近平面でクリップされて影が消える。
  局所影アトラス (`ShadowAtlas`) のラスタライザは変えない (画面内の `queue_.opaque` のタイル単位カリングのまま。画面外キャスターの局所影は出ない)。
- **CSM のフィット (xy の範囲・zNear/zFar) は変えない。** 変えると影の解像度配分が変わり golden が動く。画面外のスキンもバインドポーズの world AABB でフィットに入れ続ける (従来は常に可視だったため)。
- カスケード別のキューは本描画と同じキー (material → mesh → lod) で `Sort()` してから `ShadowPass` へ渡す (インスタンシングが効く)。render_bench では 1 カスケード 3364 draw が [4, 10, 14] draw になる。
- golden は 3 枚更新した (demo_forward / demo_deferred / demo_forward_fxaa)。差は手前の床へ画面外の立方体の影が増えたこと (直った動作) と、run の組が変わったことによる 1〜2 画素の丸め差。4 点計測で断定済み。

### 4-2. スキンの保守的 AABB

バインドポーズの AABB しか無いスキンは従来は常に可視扱いだった。(モデル, メッシュ) の組ごとに、**全クリップの全キー時刻 + キー間 1/30 秒刻み + バインドの姿勢**でボーンごとの頂点包絡を送った和集合に、最長辺の半分 (下限 5cm) の余白を足した固定箱を初回描画時に計算してキャッシュする (`SkinBoundsCache`、登録の通番 `revision` で無効化)。
クックしないので `kCookVersion` に関係しない。ラグドール作動中は常に可視。
UE の固定 bounds / Unity の `localBounds` と同じく、パレットを評価する前に判定できる。評価後に判定すると画面外のキャラのパレット評価を省けない。

箱は実アセットでバインド高の 2.4〜2.9 倍と緩い (余白は IK / ブレンドへの安全側の値で実測ではない)。画面端の判定が甘いだけで正しさには影響しない。詰めるのは計測で効果が見えてから。

## 決定 5: URO (アニメの距離間引き) は描画側だけ

### 5-1. 描画側に置いた理由

ポーズは `SkinnedMeshComponent` の入力からの純関数で、sim で姿勢を使うのは部位追従・IK・ラグドールだけ。描画のパレットは `RenderSystem` が別に評価している。
**sim に入れてカメラ距離で間引くとリプレイ・ネット対戦で割れる** (カメラは sim の入力ではない)。tick とエンティティだけから間引きを決めるなら割れないが、カメラ距離が使えず効果が薄い。
sim のポーズ評価には一切触れず、描画のパレットだけを間引く (UE の URO と同じく見た目だけの最適化)。

### 5-2. 間引きの表 (`kUroTiers`)

外接球の screen-size (`BoxScreenSize`、LOD と同じ式) に対して:

| screen-size | 画素数の目安 (1080p) | 作り直す間隔 |
|---|---|---|
| 5% 以上 | 54px 以上 | 毎 tick (1) |
| 2% 以上 | 22px 以上 | 2 tick (30Hz) |
| 0.8% 以上 | 9px 以上 | 4 tick (15Hz) |
| それ未満 | | 8 tick (7.5Hz) |

画素差を測って決めた値ではなく、目視で調整する前提の初期値。ラグドール作動中と、クリップ・層を混ぜている最中 (`IsPoseBlending`) は間引かない。
画面外のスキンにも掛かる (影のために評価する場合)。`RenderSystem::enableAnimUro` (既定 true)、CLI `--no-uro`、Rendering メニューで切る。

### 5-3. 窓の規則と位相

- 作り直す tick = `(simTick + UroPhase(entity.index)) % interval == 0`。`simTick` は EngineLoop が `ctx.tickIndex` で渡す tick 番号で、**実時間・描画フレーム数に依存しない**。
- **位相は fmix32 (murmur3 の整数ミックス) で散らす。** `ModelLoader` はキャラ 1 体ごとに 20 エンティティを作るので `entity.index` が 20 刻みになり、interval 2 / 4 のキャラの位相が全員揃って更新が 1 tick に集中する
  (unique demo の frame 33 で実測)。乱数ではなく決定的な整数ミックス。unique demo で更新が 2/4、3/3、3/3、4/2、2/4 と分散することを確認した。
- 窓の先頭 `UroWindowStart` = `simTick - (simTick + phase) % interval`。窓の途中の tick では、同じ窓で作ったパレットをそのまま使う (補間しない)。
  同じ tick に作ったものは再利用しない (tick が進まない編集中にポーズが変わったとき、見た目へ反映するため)。

### 5-4. ビュー間キャッシュ

`SkinPaletteCache` を Scene View と Game View で共有し、同じキャラのパレットを 1 フレームに 2 回作らない。描画専用で sim から見えない。

- 鍵: エンティティ + モデル + ポーズ入力 (`SamePoseInputs` と同じ等価に、使用層の `prevTimeQ` / `stepQ` を足した `SameRenderPoseInputs`) + 補間 alpha。
  入力が全部同じなら、いつ作ったパレットでも今作り直したものと同じなので再利用する。URO の間引きは、さらに窓・間隔・作った tick の条件を満たすとき。
- ポーズ入力が変われば必ず作り直す。ラグドールは ECS のポーズ入力だけでは決まらないので再利用しない (`cacheable = false`)。
- 検索と確保は直列段だけ、`palette` への書き込みだけを並列段が行う。エントリは `std::deque` なのでアドレスが動かない。

### 5-5. 描画履歴への依存と `ResetRenderHistory`

LOD のヒステリシスと URO の窓は「その run でどの tick を描いたか」の履歴に依存する (ユーザー判断 2026-10-09: 許す)。
URO の窓の途中で初めて見えたキャラは、その tick のポーズでパレットを作る (窓の先頭 tick のポーズは持っていない)。
同じ run・同じ撮影手順なら再現するが、**シークや巻き戻しの直後は、連続再生と比べて遠景のキャラの姿勢が最大 interval-1 tick ずれうる** (見た目だけで sim には入らない)。

逆 (どの描画履歴でも同じ絵) を選ぶには、全スキンのポーズ入力を更新 tick ごとに記録して窓の先頭のポーズで評価する必要がある。画面外のキャラにも毎 tick の記録が要り、コストが効果を食う。

履歴 (`lodHistory_` とパレットキャッシュ) を捨てる `RenderSystem::ResetRenderHistory()` の契機:

1. `ReleaseGpu` (GPU リソースの解放。デバイス消失の復旧 (M88) を含む)。
2. タイムトラベル等の状態復元の後 (`ResetNonSimLanesAfterRestore`)。
3. シーン読み込み (`TickServices::sceneLoadSerial` が変わったとき)。
4. `simTick` が前に戻ったとき (巻き戻し・Play の開始や停止)。

## 決定 6: 描画側の並列化は「出力スロットを先に確保し、結合は index 順」

sim の並列化の 3 条件 (ADR-028) と同じ流儀を描画に適用する。

- `CollectDrawables` のステージ 2 (並列) に LOD 選択とスキンの視錐台判定を入れる。履歴は読むだけで、書くのはステージ 3 (直列)。
- パレット評価は「直列でパレットが要る項目を数えてエントリを確保 → `ParallelRanges` で評価 (`RunPaletteJobs`) → 直列で index 順にキューの項目へ繋ぐ」。WARN の 1 回出し (`boneOverflowWarned_`) は並列段で書かない。
- カスケード判定は並列 (キャスター単位、出力は自分の要素)。キューへの push は直列のまま。
- 証明: `--no-jobs` とスクショの画素差 0 (render_bench と既存のスキン入りデモ、WARP で)。

D3D11 の deferred context は範囲外 (ユーザー判断)。

## 計測: どういうシーンでオクルージョンを OFF にした方が得か

既定 ON はユーザー判断 (2026-10-09。読み戻しで run を飛ばす案は不採用)。ここは、OFF にした方が得な条件を実機と WARP で測った結果。切り替えは決定 3-4 の口で行う。

条件 (2026-10-10): `Runtime.exe` Release、実 GPU は RTX 3060 (ドライバ 32.0.15.9597)、WARP は `--warp`。
`--render-bench-demo` / `--render-bench-unique-demo` を `--no-audio --font-embedded` で `--render-stats-dump`、
ON と `--no-occlusion` を交互に実行した中央値。実 GPU は 1920x1080・`--shot-frame 300`・各 10 回、WARP は 960x540・`--shot-frame 30`・各 5 回。
「描画 ms」= Deferred なら GBuffer、Forward なら Forward 不透明の GpuTimer で、**ON のときはオクルージョンの判定 + ピラミッド構築を含む**。
「CPU 提出」= `RenderGeometry` の CPU 時間 (`cpuMs.gbufferSubmit`、直近 32 回平均。Deferred のみ計る)。統計の読み戻し (`PollStats`) は計測区間の外。
実 GPU の ms は GPU のクロック状態で run 間に 2 倍以上揺れる (同じ入力で 0.4 ms と 10 ms の二峰になる run もあった)。傾向を見る参考値で、ゲートにはしない。

**2026-10-10 に測り直した** (統計の読み戻しを待たなくした後。review-1 #1)。旧表の CPU 提出 (0.23 → 1.31 ms、0.71 → 2.52 ms) は読み戻しの GPU 待ちを含んでいて、解像度に比例して伸びていた。
実 GPU の列は 1920x1080・`--shot-frame 300`・各 10 回の中央値で取り直した。WARP の列は GPU の処理時間で今回の修正の影響を受けないので、旧計測のまま (再測定していない)。

| シーン (描画 draw 数 / 落ちた数) | 経路 | 実 GPU 描画 ms OFF → ON | 実 GPU CPU 提出 ms OFF → ON | WARP 描画 ms OFF → ON |
|---|---|---|---|---|
| 既定の bench (23 / 2800。インスタンシングの run が少ない) | Deferred | 0.60 → 0.58 (うち判定 0.21) | 0.18 → 0.27 | 63.4 → 28.0 |
| 同上 | Forward | 0.42 → 0.47 (うち判定 0.21) | (計測対象外) | 127.4 → 59.5 |
| unique bench (1352 / 967。メッシュ・材質が全部別) | Deferred | 1.13 → 1.66 (うち判定 0.20) | 0.55 → 1.01 | 16.1 → 34.2 |
| 同上 | Forward | 0.22 → 2.96 (うち判定 0.20) | (計測対象外) | 50.4 → 70.0 |

CPU 提出が解像度に依存しないことの確認 (既定 bench、Deferred、Release、実 GPU、`--shot-frame 300`、中央値 ms):

| 解像度 | OFF | ON | ON − OFF | 回数 |
|---|---|---|---|---|
| 320x180 | 0.177 | 0.253 | 0.076 | 5 |
| 1920x1080 | 0.196 | 0.259 | 0.064 | 10 |
| 3840x2160 | 0.200 | 0.248 | 0.048 | 5 |

修正前は同じ条件で ON が 320x180 で約 0.3 ms、1080p で約 1.5 ms、4K で約 3 ms と、画素数に比例して伸びていた (OFF は 0.2 ms 前後で一定)。

読み取れること (測定した範囲):

- **ON が得なのは、隠れた物の頂点・ラスタライズが GPU の主なコストで、draw のコマンド数が少ないシーン。** WARP の既定 bench では描画が約 2.2〜2.3 倍速い。
- **OFF が得なのは、メッシュ・材質がばらばらで run がほとんど組めない (draw が千単位の) シーン。** フェーズ 2 は run / 単発ごとに状態設定・CB 更新・indirect の発行をもう 1 回出す。
  CPU 提出は unique で約 1.8 倍 (0.55 → 1.01 ms)、GPU は unique の Forward で 0.22 → 2.96 ms (WARP でも Deferred が 2.1 倍、Forward が 1.4 倍遅い)。70% が遮蔽されていても取り戻せない。
- **ON の固定費**: 判定 + ピラミッド構築が 1080p で GPU 約 0.2 ms (WARP の 960x540 で 4〜6 ms)、CPU 提出が既定 bench で +0.05〜0.08 ms 程度 (解像度によらない)。
- **速い実 GPU で形状が軽いシーンでは、そもそも効果が測れない。** 既定 bench は 3364 個で 130 万三角形あるが、Deferred の描画が OFF でも 0.6 ms で、Forward も 0.4 ms (推測: early-Z が隠れた画素のシェーディングを落としている。未確認)。
  ON の差 (±0.1 ms) は固定費と揺れに埋もれる。

方針 (推奨。自動では切り替えない):

| 状況 | 設定 |
|---|---|
| 隠れる物が多い街・屋内で、物が似たメッシュ・材質に集まる (インスタンシングが効く)。ラスタライズが遅い GPU (内蔵 GPU・WARP) | ON (既定) |
| draw のコマンドが千単位で、ほぼ全部が別メッシュ・別材質 (ユニークな小物の山)。CPU がボトルネックのシーン。形状が軽い物ばかりのシーン | OFF (`rendering.occlusionCulling: false`) |
| 判断がつかない | 既定 ON のまま、ProfilerWindow の Render ms とフレーム時間をメニューの切り替えで比べる |

分かっていないこと: 三校の実シーンでの効果 (実機での計測は未実施)、描画コマンド数に応じた自動の切り替え (閾値を決める根拠がまだ無い)。

`--shot-frame` が小さい (30 付近) と、直近 32 回の平均にウォームアップが混ざって CPU 提出が大きく揺れる。比較するときは 300 フレーム以降を取る。
`--render-stats-dump` の run は統計を待って読む (決定的にするため) ので、ここの CPU 提出は「待ちを計測区間の外へ出した値」。対話の描画 (待たない) の `cpuMs` は dump から取れない。

## 検証

- selftest: `MeshLod` (選択・履歴・生成・blob 往復・`.meta` 変更の再クック・実アセット)、`Occlusion` (max-Z がビット一致、判定の境界、Deferred / Forward の ON/OFF 全画素一致、カット直後、通番の飛び、失敗注入、統計)、`SkinBounds`、`ShadowCull`、`RenderStats`、`TagSelfTest` (設定の保存と読み戻し)、`EngineCliSelfTest`。
- `tools\shot_verify.bat`: golden 30 枚 (LOD 未設定・オクルージョン既定 ON・URO 既定 ON の状態で既存の許容値のまま PASS。更新は決定 4 の 3 枚だけ)。
- `tools\replay_verify.bat`: 描画の変更が sim に漏れていない (kCookVersion 6 でも cook 有無のビット一致 (M51b) が保たれる)。
- `render_bench` の `counts` は Debug / Release / `--warp` で完全一致。オクルージョン ON/OFF の画素差は、render_bench の Forward / Deferred、境界の解像度、各デモ、デバイス消失で 0。例外はカメラカットのフレームで LOD メッシュがあるとき (11 画素、z-fight。決定 3-6)。
- 実 GPU の Release は同じ入力の撮影が run 間で 51 画素・最大差 1 だけ揺れる (M90 前の `29a775e` でも出る)。画素の A/B は WARP で取る。

## 残る制約

- 目視待ち: LOD のポップと法線・UV、`--hzb-debug` の見た目、ProfilerWindow、エディタのメニューの保存経路 (`docs\test_checklists.md` の M90)。
- 硬い面のメッシュに LOD の段が作れない (決定 2-6)。
- スキンはオクルージョンの判定箱に載らない (決定 3-1)。局所影アトラスは画面外キャスターを見ない (決定 4-1)。
- Unity / UE の方式は記憶による照合で、一次資料 (公式ドキュメント) は未確認。
- 実 GPU の画素が run 間で揺れる件は M90 以前からの別件。
