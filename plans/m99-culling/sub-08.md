# sub-08: カリング用カメラの固定と理由別の可視化

- 依存: sub-07
- 状態: OK (コミットは司会)
- 往復: 1

## やること
spec 4.1.7 / 4.3 / 受け入れ条件 9 (と 2 の `--cull-freeze-frame` / `--cull-debug`)。
- カリング用カメラの固定: Rendering > カリング の「カリング用カメラを固定」と `--cull-freeze-frame N`。ON の瞬間にビュー別 (1..3) の非ジッタ view-proj・位置・`proj._22`・正射影かを控え、そのビューの視錐台 / 距離 / 画面サイズの判定 (メッシュと 4.1.5 の経路) に使う。ライト選別・CSM・アトラスのライト側・オクルージョン・描画は今のカメラ。固定中は控えた視錐台の 12 辺を線で描く。OFF で解除、`ResetRenderHistory` でも解除しない (明示的な操作だけで外す)。セッション限り。
- 理由別の枠表示: 「落とした物を表示」と `--cull-debug`。落としたメッシュのワールド AABB を理由別の色 (名前付き定数。視錐台 / 距離 / 画面サイズ / オクルージョン) で線描き。オクルージョンの箱は既存の `OcclusionDebugBoxes` (`RenderSystem.cpp:2443-2470`) を `--hzb-debug-max` 無しでも集める。上限 4096 箱、超えた数を表示 (画面かログ)。凡例はメニューのツールチップ。
- Profiler: 理由別の数 (`culled` / `culledDistance` / `culledScreenSize` / `occluded` / `culledNonMesh` / `unculledGpuEmitters`) をビュー別の表に出す (sub-02 で列を作っていれば値が入るだけ)。
- ローカライズ英日。

- **sub-04 からの申し送り**: GPU パーティクル・トレイル・スプライト・デカールの判定は `RenderView::cullFrustum` (CollectLights で設定、非ジッタ) と `FrameContext::frustum` から来る。固定の控えを差し込むのはそこ。CPU パーティクルのプール判定は `view.proj` から視錐台を作り直している (ジッタあり) ので、固定に従わせるにはここも `cullFrustum` へ寄せる。寄せた結果 CPU パーティクルの判定がジッタ無しに変わる場合は、`particle_cpu.png` と cull_verify の一致で確かめる。

- **sub-05 からの追加**: `culledNonMesh` に CPU パーティクルのプールで視錐台により落とした数も足す (spec 4.1.7。タイルチャンクは sub-05 で数え済み)。

- **sub-07 からの持ち越し (should)**: 画面サイズの除外 (`exemptFromSizeCulling`) を RenderSystem 経由で確かめていない。`--culling-distance-demo` の**末尾に**「RenderCulling (maxDrawDistance 0、exemptFromSizeCulling = true) を付けた小さい遠景の物」と「付けない同じ物」を足し、`--no-distance-culling --cull-min-screen-size <両方が閾値未満になる値>` の dump で前者だけ残る (`culledScreenSize` の差が 1) ことを示す。`--cull-debug` の撮影で画面サイズの色の枠も確かめる。

## やらないこと (このサブでは)
- オクルージョンの固定 (spec D18)
- 4.1.5 の経路の枠表示 (メッシュだけ)

## 触る場所 (planner の見立て)
- `src\Engine\Engine\Rendering\RenderSystem.{h,cpp}` (`CollectLights` で作る視錐台とは別にカリング用の視錐台、デバッグ線 :2443 付近)
- `src\Engine\Engine\Rendering\DebugDraw.h`、`src\Engine\Renderer\Passes\OcclusionCullPass.*`
- `src\Engine\Engine\App\EngineCli.cpp`、`EngineCliSelfTest.cpp`、`src\Editor\App\EditorApp.cpp`、`src\Editor\Windows\Debug\ProfilerWindow.cpp`

## 受け入れ条件 (このサブ)
1. `--render-bench-demo --cull-freeze-frame 10 --render-bench-cut-frame 20 --render-stats-dump` で、`culled` が固定しない run の「カット前の視点」の値と一致し、カット後の視点の値と異なる
2. `--cull-debug` (+ 固定 + カット) のスクショで、理由別の色の枠と固定した視錐台の線が見える (画像のパスを SELF_EVAL に書く)
3. 固定も枠表示も OFF のとき、`counts` と絵が sub-07 後と一致
4. CLI の解析と不正値 (EngineCliSelfTest)

## 検証コマンド
- Debug / Release ビルド
- 上記の dump 2 本と比較、`--screenshot` (memory: シーン撮りは Runtime.exe でもよい)
- `MYE_SELFTEST_ONLY=EngineCli,RenderStats` で `Editor.exe --selftest`
- `tools\check_rules.ps1`

## 実装メモ (coder が追記)

SELF_EVAL: sub-08 (round 1)
実装:
  - RenderSystem.{h,cpp}: `freezeCullCamera` / `cullDebug` と、viewKey 別の控え `CullFreezeSlot` (view / projNoJitter / 位置 / proj22 / 正射影 / 視錐台 / 8 隅)。CollectLights が FrameContext に `cullFrustum` / `cullCameraPos` / `cullProj22` / `cullOrthographic` / `frozen` を用意 (固定なしは今のカメラと同値、ありは控え)。CollectDrawables の視錐台 (メッシュ・デカール)・距離・画面サイズだけが読む。LOD・URO・地形・ライト選別・CSM は今のカメラのまま。`view.cullFrustum` も控え側を指す (GPU パーティクル・トレイル・ビルボード)。
  - CpuParticleBackend.cpp: プールの視錐台を `view.cullFrustum` (非ジッタ・固定対応) へ寄せ、落としたプール数を `culledNonMesh` に計上。旧 `RenderView::frustumCulling` は削除。
  - Sprite2DRenderer.{h,cpp}: `SetCullCameraOverride` (表示範囲の箱だけ控えの view/proj で作る。視差は今のカメラ)。
  - 枠表示: CollectDrawables が理由別 (視錐台 黄 / 距離 青 / 画面サイズ 緑) の AABB を `cullDebugBoxes_` に積み、オクルージョンの箱 (赤) は `RenderView::occlusionBoxCollect` (新設。--hzb-debug-max なしでも Forward/Deferred が集める) で集める。合計 4096 まで、超過はログ 1 回 + メニューに数。固定中は控えの視錐台 12 辺を白線で描く。色は名前付き定数 (0xRRGGBBAA)。
  - EngineCli.cpp / EngineLoop.{h,cpp}: `--cull-freeze-frame N` (N >= 0 の整数以外は解析エラー) と `--cull-debug`。N フレーム目に 1 回だけ固定を立てる (以後はメニューで外せる)。
  - EditorApp.cpp / LocalizationTable.inl: Rendering > カリング に「カリング用カメラを固定」「落とした物を表示」(英日、凡例はツールチップ、超過数の行)。ProfilerWindow.cpp: ビュー別の理由別の行 (`Prof_ViewCullReasons`、occluded 込み)。
  - DemoContentRender.cpp (+ DemoContent.h コメント): `--culling-distance-demo` の末尾に SmallFar_Exempt (exemptFromSizeCulling) / SmallFar_Plain の組 (z = 100、scale 0.3)。
  - EngineCliSelfTest.cpp: 既定値・正常値・不正値 5 種の解析。
仕様との差分:
  - [追加] `RenderView::occlusionBoxCollect` (RenderTypes.h 末尾に 1 フィールド、RenderPath.h のコメント) — オクルージョンの箱を `--hzb-debug-max` 抜きで集めるのに必要 (spec 4.1.7)。ForwardPath / DeferredPath の収集条件に `|| view.occlusionBoxCollect` を足しただけ。
  - [追加] `RenderView::frustumCulling` を削除 (CPU プールが `cullFrustum` の有無で判定するため不要になった)。
  - [解釈] 「ON にした瞬間」は、フラグを立ててから各ビューが次に描かれたときのカメラ (viewKey 1..3 別。次の描画で 1 回控えて、OFF まで保持)。固定が表すカメラが無いビュー (カメラ無し) は控えない。
  - [解釈] Sprite2D は 2D の表示範囲判定だけ控えを使い、視差の基準位置は今のカメラのまま。透視の控えでは 2D は元々判定しない。
  - [解釈] オクルージョン箱は枠の上限 4096 を落としたメッシュ箱と共有 (spec の「描く箱は上限」)。
  - 持ち越し (sub-07 should): 小物の組を追加し、RenderSystem 経由で exempt が効くことを確認 (下記)。
検証:
  - Release / Debug ビルド (MSBuild MyEngine.sln x64) → 成功 (最終コードで両方)。
  - `Runtime.exe --render-bench-demo --warp --render-stats-dump` (Release): 既定 drawCalls 23 / culled 308 / occluded 2867 / shadowCasterCandidates 3686 / culledNonMesh 0 (sub-07 後と同値)。`--cull-debug` 付きも同値。
  - 同 `--render-bench-cut-frame 20` (固定なし): culled 1169 / drawCalls 14。`--cull-freeze-frame 10 --render-bench-cut-frame 20`: culled 308 (固定前の視点と一致、カット後の 1169 と異なる)。Debug ビルドの --warp でも同じ 308 / 22 / 46。(acc. 1, 3)
  - `--culling-distance-demo --no-distance-culling --cull-min-screen-size T`: T = 0.0005 / 0.003 は culledScreenSize 0、T = 0.005 / 0.02 は 1 (SmallFar_Plain だけ。exempt は残る = 差が 1)。(sub-07 持ち越し)
  - スクリーンショット (1280x720、--warp、--frames 63 --shot-frame 60): `C:\Users\akita\AppData\Local\Temp\claude\58c0e4c841a0685f\f2580926-5c7d-48c2-9842-9b1a8a51de55\scratchpad\s_bench.png` (固定 + カット + --cull-debug: 黄 = 視錐台、赤 = オクルージョン、白 = 固定した視錐台の線)、`...\s_dist.png` と拡大 `...\s_dist_zoom.png` (距離 = 青、画面サイズ = 緑の箱を SmallFar_Plain に確認、exempt の方は箱なし)、固定のみ `...\s_bench_frz.png`、枠のみ `...\s_bench_nofrz.png`。(acc. 2)
  - `tools\shot_verify.bat` (Release): 全 golden 一致 (particle_cpu.png は maxDiff 0)、FAIL は既知の flow_title のみ。
  - `MYE_SELFTEST_ONLY=enginecli,renderstats,distancecull,tilemap,sprite2dasset,particle,vfx,decal,occlusion,render` の Debug `Editor.exe --selftest` → 10 suites, 0 failed。(acc. 4)
  - `tools\check_rules.ps1` → 0 error (warning 60 は既存)。
  - `--culling-demo` の culledNonMesh は `--particle-backend cpu` と既定 (GPU) で同じ 5、`--no-frustum-culling` で 0 / drawCalls 2 → 4 (CPU プール計上が効いている状態証拠。専用の単体テストは足していない)。
  - 未実行: replay_verify (描画専用の変更で sim に触れていないため。sub-09 の通し検証に任せる)、Editor の Scene View / Game View での目視と Profiler・メニューの画面確認 (ユーザーの目視)、selftest-full。
自己採点 (1-5):
  仕様適合: 4 — 固定 (メッシュ・非メッシュ全経路・距離・画面サイズ)、枠表示、理由別の数、CLI、ローカライズは入れた。固定中の Sprite2D・GPU/CPU パーティクルの実描画は個別に撮っていない。
  正しさ: 4 — 受け入れ 1〜4 を dump・画像・selftest で確認。既定の counts と全 golden が不変。メニュー経由 (Editor) の固定は未確認。
  コード品質: 4 — 控え・判定入力を FrameContext 1 箇所に集約。RenderSystem.cpp が更に長くなった。
  テスト: 3 — CLI 解析は単体テスト、固定・枠は dump とスクショの実走のみ (RenderSystem を通る単体テストは無し)。
不安・質問:
  - `--screenshot` を `--frames` 無しで使うと Runtime が撮影後に終了しない (CPU を使い続ける) ことを確認した。`--cull-debug` 起因か撮影の既存挙動かは切り分けていない (`--frames 63` を付けると正常に終了)。sub-09 で確認してほしい。
  - Scene View の「カメラ固定」は、固定 ON 後に最初に描かれた Scene View のカメラを控える解釈。エディタで操作しながら固定する流れの使い勝手は目視確認が要る。
触ったファイル:
  - src\Engine\Engine\Rendering\RenderSystem.h / RenderSystem.cpp
  - src\Engine\Engine\Rendering\Sprite2D\Sprite2DRenderer.h / Sprite2DRenderer.cpp
  - src\Engine\Engine\Particles\CpuParticleBackend.cpp
  - src\Engine\Renderer\Pipeline\RenderTypes.h / ForwardPath.cpp / DeferredPath.cpp / RenderPath.h
  - src\Engine\Engine\App\EngineCli.cpp / EngineCliSelfTest.cpp
  - src\Engine\Engine\Loop\EngineLoop.h / EngineLoop.cpp
  - src\Engine\Engine\Demo\DemoContent.h / DemoContentRender.cpp
  - src\Engine\Core\Localization\LocalizationTable.inl
  - src\Editor\App\EditorApp.cpp / src\Editor\Windows\Debug\ProfilerWindow.cpp
  - plans\m99-culling\sub-08.md (実装メモ)
申し送り:
  - sub-09: engine_spec / docs に新 CLI (`--cull-freeze-frame` / `--cull-debug`) と固定の仕様 (viewKey 別・次の描画で控える・LOD/ライト/CSM/オクルージョン/地形は今のカメラ) を書く。`RenderView::frustumCulling` を削除し `occlusionBoxCollect` を足した点も記録。cull_verify に固定 ON/OFF の一致を入れるなら `--cull-freeze-frame N --render-bench-cut-frame M` の culled == カット無しの値、を使える。
  - `--render-bench-demo` を Editor.exe で撮ると viewKey 2 で、カットは効かない (Runtime.exe を使う)。
  - 枠の色は RenderSystem.cpp 冒頭の名前付き定数。距離の青は描画上かなり薄く見えるので、必要なら濃くする (目視で判断)。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-11)。受け入れ 1 (固定 + カットで culled 308 = 固定前の視点、固定なしのカット後 1169。Release / Debug --warp)、2 (s_bench.png / s_dist.png / s_dist_zoom.png で黄・赤・白・青・緑の枠と固定した視錐台の線)、3 (既定と `--cull-debug` の counts が sub-07 後と一致、shot_verify は既知の flow_title 以外一致、particle_cpu maxDiff 0)、4 (CLI の解析) を確認。sub-07 から持ち越した should (除外フラグの RenderSystem 経由) は SmallFar_Exempt / SmallFar_Plain の dump で解消。sub-05 からの CPU プールの計上も入った。[追加] occlusionBoxCollect と frustumCulling の削除、[解釈] 3 件は採用 (spec 4.1.7 に記録)。持ち越し: `--frames` 無しの撮影が終わらない件の切り分け、文書化、cull_verify への固定の検査、エディタ操作と青の見分けの目視は sub-09。
