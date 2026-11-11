# sub-04: 3D の経路に視錐台判定を足す (GPU パーティクル・トレイル・ビルボード・デカール)

- 依存: sub-03
- 状態: OK (コミットは司会)
- 往復: 1

## やること
spec 4.1.5 (3D の 4 経路) / 受け入れ条件 5 (3D)。どれも描画だけを落とし、sim・粒子の更新・トレイルの点列の更新は止めない。sub-02 の視錐台スイッチに従う。
- **GPU パーティクル** (エミッタ単位): 解析的な上界 (spec D12)。
  - 上界 = 放出形状の広がり (`shape` / `shapeRadius` / `coneAngleDeg` / `boxExtents`) + `speedMax·T + ½|gravity + wind|·T²` (T = `lifetimeMax`) を原点の周りに取り、`ParticleBillboardExpand` と同じ式 (`maxSize0` の代わりに `sizeMax`、`stretchMax` 込み) で広げる。ローカル空間は `TransformAabbToWorld(renderWorld, …)`。判定は `ParticlePoolVisible` を再利用。
  - 上界を持てないエミッタ = `turbulence != 0`、`fieldInfluence != 0`、`velocityInheritance != 0`、またはワールド空間で直近 ceil(T·60) tick の間に原点が動いた (原点が動かなくなってからの tick 数を持つ)。これらは常に描き、`unculledGpuEmitters` に数える。他に式が外れる要素 (衝突の反発 > 1、`prewarmTime`、未知の力) を見つけたら同じく「常に描く」側へ入れ、実装メモに書く。
  - 落としたエミッタは `SortEmittersForDraw` の Dispatch と描画を飛ばす。
- **トレイル** (`VfxRenderer::Render` :299-308): 点列の AABB を幅の半分だけ広げて判定。
- **SpriteRenderer** (`VfxRenderer.cpp:258-297`): 中心 = 位置、半径 = `0.5·|size|` × ワールド行列の最大軸スケール の球の外接箱で判定。TextMesh は判定しない。
- **デカール** (`RenderSystem.cpp:1181-1236`): 投影ボックス (ワールド行列 × デカールの単位箱。単位箱の範囲は `FillDecalTransform` とシェーダで確かめる) で判定し、コメント (:1184-1186) を今の挙動に直す。
- 落とした項目を `culledNonMesh` に数える (ビュー別)。
- `--culling-demo` の**末尾に**、画面内外の GPU エミッタ (静止・判定可)、乱流付き GPU エミッタ (判定不可)、トレイル、SpriteRenderer、デカールを足す。

## やらないこと (このサブでは)
- 2D (sub-05)、TextMesh・水面
- GPU 粒子の位置の readback
- エミッタに固定 Bounds を持たせる欄 (spec U4 の逆を選んだ場合の後回し)

## 触る場所 (planner の見立て)
- `src\Engine\Engine\Particles\GpuParticleBackend.{h,cpp}` (`GpuEmitter` :79-104、`Render` :989、`SortEmittersForDraw` :907、描画 :1078-1179)
- `src\Engine\Engine\Particles\ParticleCurves.h:244-272` (判定と膨らみの式)
- `src\Engine\Engine\Vfx\VfxRenderer.cpp`
- `src\Engine\Engine\Rendering\RenderSystem.cpp` (デカールの収集、`particles->Render` / `vfx->Render` への視錐台の受け渡し :2370 / :2412 付近)
- デモ: `DemoContentRender.cpp`

## 受け入れ条件 (このサブ)
1. `--culling-demo` で視錐台 ON/OFF が全画素一致 (tol 0)。ON で `culledNonMesh` > 0、OFF で 0
2. 乱流付きエミッタ・動くワールド空間エミッタが `unculledGpuEmitters` に数えられ、描かれる
3. 上界の単体テスト: 形状 4 種 × ローカル / ワールド × 重力あり / なしで、上界が粒子の取りうる位置を包む (CPU バックエンドで同じ記述子を回し、全粒子が上界に入ることを確かめる)
4. 既存の `particle_gpu.png` / `particle_cpu.png` など shot_verify が差なし (画面内の物は 1 つも落とさない)
5. `counts` が Debug / Release / `--warp` で一致

## 検証コマンド
- Debug / Release ビルド
- `Editor.exe --culling-demo --screenshot` ON / `--no-frustum-culling` + img-diff、`--render-stats-dump`
- `MYE_SELFTEST_ONLY=Particle...,<新テスト>` で `Editor.exe --selftest`
- `tools\shot_verify.bat`、`tools\check_rules.ps1`

## 実装メモ (coder が追記)

SELF_EVAL: sub-04 (round 1)
実装:
  - ParticleCurves.h:ParticleGpuBound / ComputeParticleGpuBound / ParticleGpuBoundSame / ParticleGpuBoundSettleTicks / ParticleGpuBoundVisible — 記述子から粒子の取りうる位置の球 (中心 = ワールド空間はエミッタ原点、ローカル空間は 0) を計算。半径 = 形状の広がり × 基底のフロベニウスノルム (ローカルは 1) + speedMax·T + ½|gravity+wind|·T² (T = max(0.01, lifetimeMin, lifetimeMax) + 3 tick)。張り出しは ParticleBillboardExpand (sizeMax 側)。判定は ParticlePoolVisible 再利用、ローカルは TransformAabbToWorld。
  - GpuParticleBackend.{h,cpp}:GpuEmitter::bound 他 / Update / MarkCulledEmitters / SortEmittersForDraw / Render — Update で毎 tick 上界を作り、前 tick と 1 ビットでも違えば「安定 tick 数」を 0 に戻す (移動・スケール・記述子の編集)。新規エミッタは過去の放出が無いので最初から使える。安定 tick 数 >= ceil(T/dt)+2 のときだけ判定に使う。Render は MarkCulledEmitters で viewCulled を立て、Sort と描画を飛ばす。使えないエミッタ (上界なし / 不安定) は unculledGpuEmitters に数える。
  - RenderTypes.h:RenderView::cullFrustum / RenderSystem.cpp:CollectLights — 非ジッタの視錐台を指す非所有ポインタを足した (視錐台 OFF・カメラなし・RenderSystem を通らないビューは null = 落とさない)。
  - VfxGeometry.h:SpriteInFrustum / TrailInFrustum、VfxRenderer.cpp:Render — スプライトとトレイルを収集時に落とす (残る項目の並びは変わらない)。TextMesh は判定しない。
  - RenderSystem.cpp:デカールの収集 — AabbInFrustum(world, [-0.5,0.5]^3) で落とす。単位箱は decal_project.hlsl の頂点と PS の |lp| <= 0.5 で確認。:1210 のコメントを今の挙動に直した。
  - DemoContentRender.cpp:BuildCullingShowcaseScene の末尾 — GPU エミッタ 4 (画面内・画面外・乱流の画面内・乱流の画面外)、画面内の回転台 (トレイル + 動く GPU エミッタ)、画面外の回転台 (トレイル)、スプライト 2、デカール 2。
  - ParticleSelfTest.cpp / VfxSelfTest.cpp — 上界の包含 (形状 4 × 空間 2 × 力あり/なし)、非対応の記述子、変化の検出、視錐台判定、スプライト・トレイルの判定の単体テスト。
仕様との差分:
  - [逸脱] スプライトの半径は `0.5·|size| × max(1, 最大軸スケール)`。spec は `× 最大軸スケール` だが、描画は size をそのまま使いワールドのスケールを掛けない (VfxRenderer の BillboardBasis は単位基底)。そのまま掛けると縮小スケールの見えているスプライトを落とす (単体テストで確認する形にした)。
  - [追加] 上界を持てない条件に `depthCollision` と `collisionFloor` を足した。深度衝突は `p.pos += n*pen` で押し戻し、解析床は `p.pos.y = floorY` へ持ち上げるので、原点・速度・重力だけの式から外れる (sub 記述の「他に式が外れる要素」)。`prewarmTime` は同じ原点・同じ式で先回しするだけなので式の内側 (対象外にしていない)。衝突の反発 > 1 は depthCollision / collisionFloor に含まれる。
  - [追加] 「原点が動いてから ceil(T·60) tick」の代わりに「上界 (中心・半径・張り出し) が前 tick と 1 ビットでも違った tick から ceil(T/dt)+2 tick」にした。移動に加え、スケール・記述子の編集 (速度や寿命を上げると残っている粒子が新しい式の外にいる) でも同じ理由で使えなくなるため。新規エミッタは過去の放出が無いので待たない (待つとデモの 6 フレームで一度も落ちない)。
  - [追加] `unculledGpuEmitters` は視錐台 ON のときだけ数える (OFF は判定をしないので「判定しなかった」ではない)。凍結・空 Dispatch 回避中・歪み (GPU は描かない) のエミッタは数えない。
  - [追加] 判定の視錐台は RenderView に足した `cullFrustum` (非ジッタ) を使う。CPU パーティクルのプール判定は sub-02 のまま `view.proj` (ジッタあり) を使っていて、そちらは触っていない。
  - [追加] CPU バックエンドの粒子 (プール) は `culledNonMesh` に数えていない (spec 4.1.5 の 4 経路に無いため)。
検証:
  - Debug / Release の MSBuild (MyEngine.sln) → どちらも成功。
  - `MYE_SELFTEST_ONLY=particle,vfx,decal,render` で `Editor.exe --selftest` (Debug) → 4 suites, 0 failed (m99 の上界 23 項目、スプライト / トレイル 9 項目 PASS)。fast 層全体 → 107 suites, 0 failed。
  - `Runtime.exe --culling-demo --warp --particle-backend gpu --width 960 --height 540 --frames 40 --shot-frame 35 --no-fxaa` の ON と `--no-frustum-culling` を img-diff --tol 0 → forward / `--deferred` とも PASS (diffPixels=0 / 518400)。画像で画面内のエミッタ 3、スプライト、デカール (deferred)、トレイル (小さく) の描画を確認 (c4 系の撮影は frame 3 でも PASS)。
  - `--render-stats-dump`: ON は culledNonMesh 4 (画面外の GPU エミッタ・スプライト・トレイル・デカール)・unculledGpuEmitters 3 (乱流 2 + 動くエミッタ 1)。OFF は両方 0。counts は Debug / Release x `--warp` / GPU の 4 通りで JSON 完全一致。
  - `tools\check_rules.ps1` → 0 error (60 warning は既存)。
  - `tools\shot_verify.bat` → 31 PASS / 1 FAIL (flow_title。diffPixels=944, maxDiff=62 で sub-03 の記録と同じ = 既知)。particle_gpu / particle_cpu を含む他は全部 PASS。
自己採点 (1-5):
  仕様適合: 4 — 受け入れ 1〜5 を実測で確認。スプライトの半径の式を 1 点逸脱 (理由は上)
  正しさ: 4 — 包含を CPU バックエンドの全粒子で検査・ON/OFF 全画素一致。ただし GPU 実機の粒子位置との突き合わせは CPU の同じ記述子で代用 (GPU 側の位置は読み戻さない)
  コード品質: 4 — 上界は純関数にして GpuEmitter は結果と安定 tick 数だけを持つ。GpuParticleBackend.h が ParticleCurves.h を取り込むようになった
  テスト: 4 — 上界と判定は単体テストあり。GPU バックエンドの Render 経由の統計は撮影 + dump で確認 (selftest には無い)
不安・質問:
  - 乱流 / fieldInfluence / velocityInheritance を持つエミッタは「常に描く」(spec どおり)。ローカル空間では fieldInfluence と velocityInheritance が無効なので上界を持てるが、spec どおり一律に外した。ローカル空間だけ許すかは planner の判断。
  - 画面内の判定は画面に 1 粒も無い (放出が止まって全粒子が死んだ) エミッタも、箱が視錐台に掛かっていれば描く (CPU のプールのように空なら skip する判定はしない)。
  - 安定待ちの間 (動くエミッタを止めた直後) は粒子の寿命ぶん常に描く。トラッキングされた動くエミッタの Scene View での扱いは未確認。
触ったファイル:
  - C:\HAL\MyEngin\src\Engine\Engine\Particles\ParticleCurves.h
  - C:\HAL\MyEngin\src\Engine\Engine\Particles\GpuParticleBackend.h
  - C:\HAL\MyEngin\src\Engine\Engine\Particles\GpuParticleBackend.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Particles\ParticleSelfTest.cpp
  - C:\HAL\MyEngin\src\Engine\Renderer\Pipeline\RenderTypes.h
  - C:\HAL\MyEngin\src\Engine\Engine\Rendering\RenderSystem.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Vfx\VfxGeometry.h
  - C:\HAL\MyEngin\src\Engine\Engine\Vfx\VfxRenderer.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Vfx\VfxSelfTest.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Demo\DemoContentRender.cpp
  - C:\HAL\MyEngin\plans\m99-culling\sub-04.md (実装メモ)
申し送り:
  - engine_spec §6.6 の「No frustum culling」行は sub-09 で消す (このサブでは文書を触っていない)。
  - sub-08 (カメラ固定): GPU / VFX / デカールは `RenderView::cullFrustum` と `FrameContext::frustum` を使う。固定の控えを差し込むなら CollectLights の `view.cullFrustum` の 1 箇所。CPU パーティクルのプール判定は `view.proj` から作り直しているので別に直す必要がある。
  - sub-06/07 で RenderCulling 等を足すとき、`culledNonMesh` は理由別に分けず「視錐台で落とした非メッシュ」のまま。
  - 撮影の再現: `Runtime.exe --culling-demo --warp --particle-backend gpu --frames 40 --shot-frame 35 --no-fxaa` (デカールは `--deferred`)。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-11)。受け入れ 1 (forward / deferred の ON/OFF 全画素一致、culledNonMesh 4 / OFF 0)、2 (unculledGpuEmitters 3 = 乱流 2 + 動くエミッタ 1)、3 (上界の包含を形状 4 × 空間 2 × 力あり/なしで CPU バックエンドの 200 tick 全粒子)、4 (particle_gpu / particle_cpu ほか PASS。flow_title は既知の別件)、5 (4 構成の counts 一致) を確認。[逸脱] スプライトの半径は仕様の誤りとして採用し spec 4.1.5 を直した。[追加] 衝突 2 種の除外、安定待ちを上界の変化基準にしたこと、新規エミッタを待たないこと、cullFrustum (非ジッタ) は採用。不安への回答: ローカル空間だけ fieldInfluence / velocityInheritance を許す拡張はしない (乱流は残るので得が小さく、条件分岐を増やさない)。粒子 0 個のエミッタを描くこと、止めた直後に寿命ぶん待つことは安全側で容認。Scene View での動くエミッタは sub-09 の手動確認項目 (test_checklists) に入れる。
