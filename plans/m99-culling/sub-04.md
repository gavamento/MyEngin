# sub-04: 3D の経路に視錐台判定を足す (GPU パーティクル・トレイル・ビルボード・デカール)

- 依存: sub-03
- 状態: 未着手
- 往復: 0

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

## フィードバック履歴
