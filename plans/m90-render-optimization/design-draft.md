# M90 描画の軽量化 — 事前調査 (design-draft)

2026-10-09 作成。/harness の planner へ渡す事前調査。ユーザー回答で範囲と進め方は確定済み。

## Context
engine_spec.md §12.3 で「mesh LOD and GPU occlusion culling」が未着手のまま。
plans\sparkling-gliding-quokka.md の 4-F に「既存 HzbPass (M56c) を再利用」と 1 行あるだけで、計画ファイルは無い。
ユーザー依頼: メッシュ LOD、Hi-Z を流用した GPU オクルージョンカリング、マルチスレッドなどの軽量化をエンジンへ入れる。
回答で決まったこと:
- 範囲: 計測の土台 / メッシュ LOD / GPU オクルージョン / 影とスキンのカリング
- 並列化: 描画側の CPU 処理 / アニメ更新の距離間引き / シミュレーションの並列化
- 進め方: /harness で回す

番号は着手時の末尾から取る (spec §12.2 時点: ABI v28 / TypeId 80 / ADR-027、snapshot v48、kCookVersion 5)。memory では M75h が ABI v29 を予定しているので衝突を確認すること。

## 現状 (調査で確認済み、design-draft.md に載せる)
- **メッシュ LOD**
  - 実装は地形だけ (`src\Engine\Engine\Rendering\TerrainSystem.cpp` の `SelectTerrainLod`)。
  - meshoptimizer は無い (`external\VERSIONS.md`、ソースをそのままコミットする方針)。
  - `CookedMesh` (`Asset\ModelCook.h`) と `Mesh` (`Renderer\Device\GpuResources.h`) は VB/IB が 1 組だけで、LOD 欄が無い。
  - `MeshVertex` は 52B で static_assert されている。kCookVersion は 5。
- **HzbPass** (`Renderer\Passes\HzbPass.cpp`、`assets\shaders\hzb_reduce.cs.hlsl`)
  - 中身は R32F の **min-Z** (SSR 用) で、Deferred だけが SSR やデバッグのときに作る。
  - 前フレームの分は保持せず、`DeferredPath` に 1 個だけ置いてビューで共有している。
  - サーフェスマテリアルと水面の深度は含まない。
  - オクルージョン判定には **max-Z** が要り、ビュー (viewKey) ごとに前フレームの分を持つ必要がある。
- **カリング**
  - CPU の視錐台カリングだけ (`RenderSystem::CollectDrawables` で `ParallelRanges` を使う。判定は `Renderer\Pipeline\FrustumCull.h`)。
  - スキンメッシュは常に可視扱い (AABB がバインドポーズのものしか無いため)。
  - CSM (`ShadowPass.cpp`) はカスケードごとにカリングしない。影にもカメラの視錐台で落とした後のキューを使うので、画面外のキャスターの影が消える。
  - `ShadowAtlas` はタイル単位でカリングしている。これが流用できる前例。
- **描画**
  - キューは `RenderQueue` (`RenderTypes.h`、material → mesh → viewZ でソート) で、インスタンシングは `Renderer\Mesh\MeshInstancing.h` (StructuredBuffer の t0)。
  - メッシュでは Indirect 描画を使っていない (前例は `GpuParticleBackend.cpp`)。深度プリパスは足さない方針 (DeferredPath.cpp:1421)。
- **計測**
  - `GpuTimer` は GBuffer と Forward の本描画、フレーム全体を計っていない。
  - 影の draw は `prof::AddDraw` に入らない。
  - `prof::RenderStats` はビューをまたいで累積する。
- **並列化**
  - `Core\Jobs\JobSystem` は `ParallelRanges` / `ParallelFor` だけ。使っているのは Transform と視錐台の 2 箇所。
  - `--no-jobs` は設定ビットに入る。
  - ボーンパレットは直列で評価し、アニメの距離間引きは無い。
  - 物理、粒子、Crowd、BT は直列。
  - 空力と XPBD は「並列化を永久に禁止」(加算順が結果の一部)。ADR-020 は「出力次元だけを割る」流儀。

## 設計の論点 (planner がユーザーと詰める)
1. **計測の土台を最初に置く**
   - GBuffer / Forward / フレーム全体の GPU ms、影の draw を統計に足し、ビュー別に集計する。
   - LOD とオクルージョンの効果を測るベンチシーン (多数のメッシュと遮蔽物) を作る。
2. **LOD**
   - クック時に `meshopt_simplify` で段を作り、kCookVersion を 6 に上げる。段は screen-size の閾値とヒステリシスで選ぶ。
   - コライダー、RT、NavMesh は LOD0 のまま。
   - golden と既存シーンへの影響 (既定 ON か OFF か)、スキンメッシュの LOD を対象にするか、手作り LOD (glTF の MSFT_lod、FBX の LOD グループ) の扱いも決める。
   - Unity の LODGroup と UE の Static Mesh LOD を先に当たる。
3. **GPU オクルージョン**
   - 前フレームの max-Z HZB を作り、再投影して compute で AABB を判定する。可視のインスタンスを詰め、run ごとに `DrawIndexedInstancedIndirect` で描く (CPU への読み戻しは無し)。
   - 2 フェーズ (今フレームの深度で再判定) まで入れるか、ディスオクルージョンで 1 フレームの欠けを許すかを決める。
   - Forward パスと影パスへ適用するか、ProbeBaker やスクショ撮影 (決定的撮影) で無効にするかも決める。
   - 描画だけの変更なので sim のハッシュには入らない。
4. **影とスキン**
   - キャスターの収集をカメラの視錐台から切り離し、カスケードごとに視錐台カリングする。
   - スキンの AABB は、アニメクリップのクック時に全フレームの包絡を取るか、ボーン位置から実行時に求めるか。
5. **描画側の並列化**: ボーンパレット、LOD 選択、影のカリングを `ParallelRanges` に載せる (出力は互いに素で、結合は index 順)。
6. **アニメ更新の距離間引き (URO)**
   - sim に入るなら、間引きの判断も Tick 基準で決定的にする。カメラ位置に依存すると、リプレイやネット対戦で割れる。
   - sim 内に入れるか、描画側だけで済ませるかを決める。
7. **シミュレーションの並列化**
   - 対象 (粒子の CPU 版、Crowd、BT、アニメ評価など。空力と XPBD は除外) と、`useJobs` の ON/OFF でビット一致することの証明方法 (replay_verify の A/B) を決める。
   - 規模が大きいので最後のフェーズに置く。並列化の規約は ADR-028 にまとめる。

## 検証 (各サブで harness が実施)
- `bin\x64\Debug\Editor.exe --selftest` と Release 版。LOD 選択、HZB の max 縮小、カリング判定、スキン AABB のテストを `*SelfTest.cpp` に足す。
- `tools\check_rules.ps1`、`tools\replay_verify.bat`。並列化は `--no-jobs` との A/B でビット一致すること。
- ベンチシーンで、変更前後の draw / tri / GPU ms を比べる。
- `--screenshot` で LOD の切り替えとオクルージョンの欠けを確かめる (`--hzb-debug` の可視化を拡張する)。
