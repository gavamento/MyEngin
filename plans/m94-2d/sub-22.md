# sub-22: Camera2DFollow と PixelPerfectCamera2D

- 依存: sub-02, sub-11
- 状態: 未着手
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 2. #13, #31、4.1.12、7. R4

## やること
0. (sub-14 の申し送り) `boundsFromTilemap` の境界は「衝突レイヤー (`collision` = true) の非空セルの外接矩形」。`TilemapSystem::GetLayerBounds` は 1 レイヤー分なので、衝突レイヤー全部の和を取る小関数 `GetCollisionBounds` を足す (ABI には出さない)。
1. `Camera2DFollowComponent` (**hash 対象**、TypeId 末尾、spec 4.1.12 の全フィールド + `boundsAspect float (16/9)`)。`PixelPerfectCamera2DComponent` (NoHash、TypeId 末尾: `assetsPPU / refResolutionX / refResolutionY / upscaleRT / pixelSnapping`)。Catalog、Create → 2D → Camera 2D (Camera + Camera2DFollow + PixelPerfectCamera2D)。`kSimSnapshotVersion` +1 (2 型で 1 回)。
2. `Camera2DFollowSystem` (`src\Engine\Engine\Rendering\Camera2DFollow.{h,cpp}` または `Scene\`): TickRunner の 3.6 物理の後・`Nav::PostPhysics` の隣 (Transform 4 の前) で `if (stepSim)`。対象の位置は LocalTransform の連鎖を合成 (M89l の流儀)、`lookAheadX` は対象の Rigidbody.velocity.x か CC.velocity.x の符号 × 距離。`desired = target.xy + offset + lookAhead`、deadZone 内なら desired = 現在位置 (軸ごと)、`cam.xy += (desired − cam.xy) × damping`、bounds (`useBounds` / `boundsFromTilemap` の衝突レイヤー AABB) で `orthoSize` と `boundsAspect` から半幅半高を引いてクランプ。カメラの `LocalTransform.position.xy` に書く (カメラが子エンティティなら親の逆変換は**しない** — ルート前提、Inspector に警告)。`TickServices` / `EngineLoop` / `HeadlessSim` の配線。
3. `pixelperfect2d::Compute(W, H, settings) → { zoom, rtW, rtH, orthoSize }` (Renderer 層の純関数、spec 2. #13 の式。zoom = max(1, min(H / refY, W / refX))、upscaleRT && zoom > 1 で rtW = W / zoom / 2 * 2 (奇数の端数は +1 して blit で切る)、orthoSize = rtH × 0.5 / assetsPPU、それ以外 orthoSize = H × 0.5 / (zoom × assetsPPU))。`ResolveCamera` が主カメラの PixelPerfect を見て `orthoSize` を上書き、`UILayout.cpp` の射影も同じ関数。
4. `RenderSystem::Render`: zoom > 1 && upscaleRT なら、低解像度 `RenderTexture` (メンバ、`Resize`) を `FrameTarget` にして既存の全段 (Begin → path → VFX → Sprite2D → Post) を回し、終わりに Point サンプラの blit (`sprite2d_blit.hlsl`、`PostProcess` の blit と同型) で元の `target.rtv` へ整数倍拡大 (中央寄せ、余りは RT 側を大きく取っているので黒帯なし)。`DecideTaaAndFroxel` で TAA ジッタを無効化、`ResolvePost` で FXAA を無効化 (ログに 1 回出す)。UI Canvas / ImGui は `EngineLoop` / `GameView` が従来どおり後段で描く (触らない)。`pixelSnapping` (upscaleRT false) は `Sprite2DRenderer` が頂点位置を `1 / assetsPPU` 格子に丸める。
5. Inspector: PixelPerfect の有効値 (zoom / RT / orthoSize) を read-only 表示 (GameView のサイズ基準)。Scene View ギズモ: deadZone (青の矩形)、bounds (青の太線)。
6. selftest: `Camera2DFollowSelfTest` (deadZone 内不動、外れて damping 収束の数値、bounds クランプ、2 ワールド並走一致)、`pixelperfect2d::Compute` の表 (960×540 / 320×180 → 3, 320×180, 5.625 (PPU 16); 1280×720 → 4; 959×540 → 2 と奇数処理)。
7. 確認スクショ: 960×540、ref 320×180、PPU 16 のタイル (16 px) が 48 px (画素数)、UI Canvas の文字が 1 px エッジ (フル解像度)、1280×720 で 64 px。

## やらないこと (このサブでは)
- Stretch Fill / Crop Frame (U5)、Cinemachine 相当の複数カメラブレンド、カメラシェイク (スクリプト側)

## 触る場所 (planner の見立て)
- `Components.h` / `Components.cpp`、`EditorComponentCatalog.cpp`
- 新規 `src\Engine\Engine\Rendering\Camera2DFollow.{h,cpp}`、`src\Engine\Renderer\Pipeline\PixelPerfect2D.{h,cpp}` (純関数)、`assets\shaders\sprite2d_blit.hlsl`、selftest 2 本
- `src\Engine\Engine\Rendering\RenderSystem.{h,cpp}` (:577-623 Render、:626-657 BeginView、:661-726 ResolveCamera、:729-802 TAA、:2158-2355 ResolvePost)、`UILayout.cpp:529-544`
- `TickRunner.{h,cpp}` (:504-518)、`EngineLoop.cpp`、`HeadlessSim.cpp`
- `SceneViewWindow.cpp`、`InspectorWindow.cpp`、`SimSnapshot.h`、`AcousticAudioSelfTest.cpp:129`、`EditorMain.cpp`、`LocalizationTable.inl`、vcxproj
- 参考: `PostProcess.cpp:894-917` (blit)、`RenderTexture.h:19`、Unity の式 (spec 2. #13 の URL)

## 受け入れ条件 (このサブ)
1. (spec 36) `Camera2DFollowSelfTest` PASS (数値をログ)。
2. (spec 37) スクショ 2 枚 (`plans\m94-2d\screenshots\pixelperfect_960.png`、`_1280.png`) の画素数、UI のエッジ、TAA / FXAA 無効化のログ。`Compute` の表 selftest PASS。
3. PixelPerfect の無いシーンは経路が変わらない (コードで示し、`demo_forward` 1 枚を `--img-diff --tol 0`。30 枚は sub-23)。Camera2DFollow の無いシーンの hash-diff 無差分 (`--demo` 1 本)。
4. (spec 1-3) 0 警告、check_rules 0、`MYE_SELFTEST_ONLY=m94` に 2 本の selftest を足して両構成 PASS、snapshot +1 のみ。

## 検証コマンド
毎サブ必須: Debug / Release ビルド、`pwsh tools\check_rules.ps1`、`pwsh tools\gen_project_files.ps1`、`MYE_SELFTEST_ONLY=m94` の selftest (両構成)
- `Runtime.exe --demo --replay-record` → `--hash-diff`、`demo_forward` 1 枚の `--screenshot` → `--img-diff --tol 0`
- temp シーンの `Runtime.exe --warp --width 960 --height 540 ... --screenshot` と `--width 1280 --height 720`

チェックポイントではない (重い 3 本は sub-23)。

## 実装メモ (coder が追記)

## フィードバック履歴
