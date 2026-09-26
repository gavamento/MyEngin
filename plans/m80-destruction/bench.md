# M80 破壊物理 — 計測記録 (sub-11)

計測環境: Release|x64 (計測は Release で実施。焼き時間の Debug 値は sub-04/sub-14 の既存記録を参照)。
`bin\x64\Release\Editor.exe --fracture-bench` で自動生成 (フラグ名は coder 判断)。ヘッドレス
(ウィンドウ・D3D デバイスを作らず World を直接操作する専用パス、実装は
`src/Engine/Engine/Physics/FractureBenchmark.cpp`)。1 tick = 1/60 s、150 tick 実行。

## 1. シーン構成

`--fracture-demo` (`BuildFractureShowcaseScene`) と同じ配置を objectCount 体ぶん X 方向に
並べる: 床 (静的 Box)、破壊物 (立方体、質量 8kg、高さ 4m から回転を与えて落下)、水平発射の
球 (質量 20kg、速度 30 m/s、重力なし)。強度は 200N (`--fracture-demo` と同じ、既定 5000N では
このデモの衝突では割れないため)。破片数 N は 16/32/64/128/256、objectCount は 1 / 8。

計測は 3 区間:
- **壊れる前**: tick 0 〜 どれかの Destructible が割れる直前 (概ね tick 39、球が当たるまで)
- **割れた瞬間**: 割れた tick から 10 tick
- **割れた後**: それ以降 150 tick まで (全塊が動く定常状態)

各区間で `Profiler.h` のスコープ (`phys.collect` / `phys.broad` / `phys.narrow` / `phys.solve` /
`phys.writeback`、`fracture.collect` / `fracture.process`) の平均・最大 (ms) を積算する。

## 2. 焼き時間 (`BakeFracture`、Release)

立方体 1 個を Voronoi 分割。

| pieceCount | 実際の破片数 | 焼き時間 (ms) |
|---|---|---|
| 16 | 16 | 22〜27 |
| 32 | 32 | 48〜59 |
| 64 | 64 | 121〜146 |
| 128 | 128 | 344〜373 |
| 256 | 256 | 1031〜1044 |

開いたメッシュのボクセル化 (sub-04/sub-14 の既存記録、pieceCount=16、Release): res32 ≈ 4.7〜5.3 s、
res48 ≈ 10.8 s、res64 ≈ 20 s (`FractureSelfTest.cpp` の `bakeOpenMesh`)。
**Debug ビルドでは同じ計測が res32 で ≈ 56〜127 s (open box / plane quad)、res48 で ≈ 127 s
かかり Release 比で概ね 8〜12 倍遅い** (今回 `--selftest` 実行中に観測。Debug は最適化なし +
STL イテレータチェック込みのため、既存の `FractureSelfTest.cpp` の焼き系テストが元々持つ性質で
あり、sub-11 の変更 [プロファイルスコープの追加] とは無関係)。

## 3. 物理 + FractureSystem (1 tick あたり、破壊物 1 個、Release)

「割れた瞬間」区間 (ピーク負荷) の平均 (avg) / 最大 (max)、単位 ms。

| N | phys.collect | phys.broad | phys.narrow | phys.solve | phys.writeback | fracture.collect | fracture.process | 合計 avg | 合計 max (同一 tick とは限らない単純和) |
|---|---|---|---|---|---|---|---|---|---|
| 16 | 0.033 | 0.006 | 0.102 | 0.834 | 0.001 | 0.003 | 0.006 | **0.98** | 1.77 |
| 32 | 0.042 | 0.006 | 0.220 | 1.740 | 0.001 | 0.003 | 0.008 | **2.02** | 2.53 |
| 64 | 0.085 | 0.009 | 0.449 | 4.126 | 0.001 | 0.005 | 0.015 | **4.69** | 6.94 |
| 128 | 0.179 | 0.016 | 0.961 | 8.816 | 0.001 | 0.008 | 0.032 | **10.01** | 16.29 |
| 256 | 0.342 | 0.025 | 1.268 | 10.354 | 0.002 | 0.014 | 0.059 | **12.06** | 14.61 |

「割れた後」区間 (定常状態、101 tick 平均) は同程度かやや軽い (破片が飛び散ると接触が減るため)。
「壊れる前」区間 (複合コライダー 1 個としてだけ動く) は 1 桁小さい (N=256 で合計 avg 0.92ms)。

## 4. 物理 + FractureSystem (破壊物 8 個を同時に並べたシーン、Release)

「割れた瞬間」区間、合計 avg / 合計 max (ms)。8 個が同時に割れる最悪ケース。

| N | 合計 avg | 合計 max |
|---|---|---|
| 16 | 4.60 | 8.11 |
| 32 | 16.38 | 20.60 |
| 64 | 41.78 | 53.85 |
| 128 | 76.91 | 101.68 |
| 256 | 97.09 | 117.94 |

生ログ全文は本サブの作業ログ (SELF_EVAL 参照) に残す。

## 5. 上限の決定

判定基準 (sub-11.md やること 4): 「物理 + FractureSystem が 1 tick (16.6 ms) の半分程度
(≈8.3 ms) に収まる破片数」。

- **破壊物 1 個あたり**: N=64 で 4.69ms (割れた瞬間 avg)・6.94ms (合計 max) は 8.3ms 以内。
  N=128 で 10.01ms (avg) は既に超過。→ **推奨: 1 Destructible あたり 64 破片以下**
- 既定値 (`pieceCount` = 16) は上記表で最も軽く (0.98ms)、変更不要と判断。
- ハード上限 256 は分割コアの契約 (`kMaxFracturePieces`、spec 固定) のためそのまま維持。
  256 破片 1 個 (12.06ms) は単独でも予算の 3/4 を使うが、シーンにいくつ置くかは
  利用者の設計判断であり、ハード上限を下げると凸包頂点 64 と並ぶ既存の仕様上限に触れる
  ([ユーザーに聞ける] の対象外、spec §4.1 の上限記述と矛盾するため coder 側では変更しない)
- 複数 (8 個) 同時破壊は N=32 の時点で 16.38ms と予算超過するが、これは「8 個の破壊物が
  同一 tick に同時に割れる」という設計上の worst case であり、推奨値は「Destructible 単体」
  基準で決める (spec のハード上限も Destructible ごと)

反映:
- `src/Engine/Core/Components.cpp`: `DestructibleComponent.pieceCount` のツールチップに
  「推奨 64 以下、Release で線形より速く増える」を追加 (範囲 2..256 は変更なし)
- `src/Editor/Windows/InspectorWindow.cpp`: 生成済み破片数が 64 を超えたら Inspector に
  黄色警告を表示 (`Insp_FracturePieceCountHigh`、`LocalizationTable.inl` に追加)

## 6. RT — 割れた瞬間の BLAS 焼き直しスパイク

`bin\x64\Release\Runtime.exe --fracture-demo --rt-gi --warp --no-audio --frames 200` で計測。
`RtScene::RebuildBlasIfNeeded` (`src/Engine/Engine/RayTracing/RtScene.cpp`) に、実際に連結 BLAS
を焼き直した回だけ出るログを追加 (`[rt] BLAS rebuild: ...`。毎フレームは出ない — 参照メッシュ
集合が変わった回だけ通る経路のため、sim 状態には影響しない)。

| タイミング | 参照メッシュ数 | ノード数 | 三角形数 | 焼き直し時間 |
|---|---|---|---|---|
| シーン起動直後 | 2 | 258 | 780 | 0.787 ms |
| 1 個目が割れた瞬間 (箱、8 破片) | 29 | 467 | 1438 | 0.670 ms |
| 2 個目が割れた瞬間 (壁、12 破片) | 45 | 577 | 1802 | 0.392 ms |

このデモの破片数 (8 / 12) では焼き直しが 1 ms 未満で、60 Hz 予算 (16.6 ms) に対して無視できる。
**対策は不要と判断** (sub-11.md やること 5 の「問題なら対策」の分岐で「問題なし」側)。
破片数が上限 256 に近いシーンでは三角形数に比例して伸びるはずだが、凸包 (kMaxConvexVerts=64)
基準の外側メッシュも軽いため、数十 ms 級のスパイクには通常至らないと見ている
(実測は既定破片数のデモのみ。極端な破片数 × 多数の破壊物を RT で同時に割るケースは未計測)。

## 7. 描画 — インスタンシングが破片で切れるコスト (計測のみ)

`ForwardPath.cpp` / `DeferredPath.cpp` のインスタンシングはメッシュ AssetID が同じドローを
まとめる設計 (`DrawIndexedInstanced`)。破片は破片ごとに一意のメッシュ登録名
(`#frag<i>`) を持つため、同じ Destructible 内の破片同士でインスタンシングは効かない
(元は 1 メッシュ 1 ドローだった箇所が、割れると破片数ぶんのドローに増える)。

`--fracture-demo` (床 + 破壊物 3 体 + 弾 3 個) で一時プローブ (`prof::GetRenderStats()` を
毎フレームログへ、計測後に revert 済み・コミット対象外) を実測: ドローコール数は起動直後の
4 → 11 tick 以内に 45 で安定し、以後 (割れる前後を通じて) 45 のまま変化しない。三角形数は
3338 → 2570 (tick 96 前後、視錐台カリングの影響と見られる) に変化する。**このデモの規模
(破片 8/12/2 個) では「割れる前は 1 ドロー・割れた後は破片数ぶん」という変化を tick 単位で
明確には切り分けられなかった** (影・複数パスの合算値のため、45 という値は起動直後から到達
しており、破片の可視性ゲート [`ShouldHideUnbrokenFracturePiece`] の効果がドローコール数の
差として表に出にくい)。対策 (バッチング等) は求められていないため、このサブでは計測のみに
留める。破片数が多いシーン (N=128/256 × 複数体) ではドローコール数が線形に増える設計上の
制約として `engine_spec.md` 側に書き添えるのが適切 (sub-12 の文書化で反映)。

**(sub-12 追記) `prof::GetRenderStats()` の中身と、上の「45 で変化しない」の原因**:
`AddDraw()` は `RenderStats::drawCalls` を実際の `DrawIndexed(Instanced)` 呼び出し 1 回につき
+1 する (同一メッシュ・同一マテリアルの複数インスタンスを 1 回の `DrawIndexedInstanced` へ
まとめる「run」なら、instance 数によらず呼び出しは 1 回)。`triangles` はその呼び出しが描く
三角形数の合計。**フレーム単位** (`BeginFrame()` が `g_render={}` でクリア) の**全パス合算**
(shadow map・main pass 等をまとめた値) であって、パスごとの内訳や上限による丸めは無い —
つまり道具自体は正しく数えている。

上の「45 で変化しない」という記録は、**一時プローブを `Editor.exe --frames N --screenshot` で
撮っていたため**だった (screenshot-probe-recipes.md の「シーン撮りは Runtime.exe」の教訓と
同じ落とし穴)。`Editor.exe` の `--frames`/`--screenshot` はエディタの Edit モードのままフレームを
進めるだけで **Play (シミュレーション) を開始しない** — 物理も FractureSystem も一度も走らず、
どの Destructible も `broken` が立たないまま同じ絵を出し続けるので、ドローコール数が
変わらないのは当然だった (このサブで `Destructible.broken` の個数も同時にログして確認: 
`Editor.exe` 経路では 130 フレーム通して `broken=0/3` のまま)。

`tools\shot_verify.bat` が実際に呼んでいる **`Runtime.exe`** で同じシーンを撮り直すと、
期待どおりの変化が見える (`broken=X/3` も同時ログ): tick 0〜5 は `draws=4`
(床・破壊物 3 体のルート・カリングされた弾 3 個、`broken=0/3`)。M80l の
`FractureDamageProbe` が壁をスクリプトから割る tick 5 の直後、tick 6 で `draws=26`
(`broken=1/3`) へジャンプ。着弾で箱が割れる tick 台 (`broken=2/3`) で `draws=30`、
スキン腕まで割れ切る tick 50 前後 (`broken=3/3`) で `draws=45` に達し、以後 (frame 120 の
撮影ぶんを含め) その 45 のまま変化しない — **全 Destructible が一度でも割れたら、以後は
「割れる前 1 ドロー ⇔ 割れた後 N ドロー」の差分が固定されて増減しなくなる**ため
(既に分かれた破片がさらに細かく分かれても、破片エンティティの集合自体は増えない。
`_cap` を含む破片メッシュの集合が変わるのは新しい塊が初めて分離する瞬間だけ)。
**結論: root proxy の可視性ゲートはドローコール数に正しく効いている。sub-11 の「変化しない」は
計測ツールの選び方 (Editor.exe) が原因で、破片が壊れる前から描かれていたわけではない**
(むしろ逆で、Editor.exe 経路では一度も壊れていなかった)。

## 9. strength の既定値 (sub-18、review-1 #5)

`RunFractureStrengthCalibration()` (`FractureBenchmark.cpp`、`--fracture-bench` の一部) が、質量
1kg・破片16・一辺1mの箱で spec §2 の3基準それぞれの境界を対数二分探索で求める (PhysicsEnvironment
を置いた状態、詳細はコード comment 参照)。実測 (Release):

| 基準 | 境界 |
|---|---|
| (a) 3m 落下で割れる | S ≤ 91.7N |
| (b) 床に 600 tick 置いても割れない | S ≥ 1.8N |
| (c) 1m 落下では割れない | S ≥ 53.5N |

3 基準を同時に満たす範囲は `[53.5, 91.7]`。対数中央値 **70N** を既定値にした
(`DestructibleComponent::strength`)。旧既定 5000N は普通の衝突では割れないほど過大だった
(review-1 #5)。デモ・ベンチ・テストで下げていた 100〜200N の値は、新既定 70N で足りるものは
戻した (`--fracture-demo` の箱・壁・スキン腕、`--fracture-bench` は引き続き 200N のまま —
物理精度ではなく性能計測が目的で、既定値へ揃える必要が薄いため)。

## 10. voxelResolution の Inspector 上限 (sub-18、review-1 #6)

開いた箱・16破片 (`--fracture-bench` の一部) で解像度を伸ばすと、72 までは成功し 80 以上は
失敗する:

| 解像度 | 結果 | 焼き時間 (Release) |
|---|---|---|
| 64 | 成功 | 17.7s |
| 72 | 成功 | 22.2s |
| 80 | 失敗 | 26.2s |
| 96 | 失敗 | 36.5s |
| 128 | 失敗 | 67.6s |

失敗はすべて同じ経路: `FractureBake.cpp` の `ProcessAdjacentPair` が呼ぶ `CutMeshByPlane`
(セルの二等分面でボクセル化済みメッシュ全体を 1 回切る、sub-01 の単一平面カット) が
`positive側の蓋: 外側面と閉じ合わない` で失敗する (`FractureMesh.cpp` の `VerifyCapOrientation`
または `GeometricCapClosureValid`)。高解像度になるほど、この 1 回切りに渡される三角形数が
非常に多くなり (数千〜数万)、境界ループが長く複雑になる。ratio (欠けの大きさ) は解像度に対して
単調ではない (80: 0.0306、96: 0.0041、128: 0.0269) — 特定の頂点配置が閉じ判定の許容誤差
(`FractureMesh.cpp` の固定・相対 ε) の境界に近いかどうかに左右される離散的な失敗と見られる。
根本原因 (多頂点の境界ループに対する `VerifyCapOrientation`/`CapLoops` の頑健性) の追究・修正は
本サブの範囲を超える (単一平面カットの改修は既存の閉じ判定・体積・接着面積の契約に触れ、
決定性の再検証が要る大きめの変更になる)。Inspector の選べる範囲を実測で成功する最大の
**72** に下げた (`Components.cpp` の `voxelResolution` フィールド)。内部のハードクランプ
(`FractureVoxel.h` の [16,256]) は変えていない — スクリプト等で 72 を超える値を明示的に
設定すること自体は妨げない。

## 11. 検証コマンド

```
bin\x64\Release\Editor.exe --fracture-bench
bin\x64\Release\Runtime.exe --fracture-demo --rt-gi --warp --no-audio --frames 200
tools\replay_verify.bat
tools\shot_verify.bat
tools\check_rules.ps1
```
