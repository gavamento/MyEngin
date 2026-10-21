# sub-23: `--2d-demo`、golden `2d` / `2d_lights`、replay_verify `2d`

- 依存: sub-01 〜 sub-22 (全部)
- 状態: OK (commit c1948da)
- 往復: 1
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 4.1.14、4.4 (性能)、5. 38-39

## やること
1. アセット生成 `tools\gen_2d_assets.py` (Python、PNG を Pillow 無しの純標準ライブラリ (zlib + struct) で書く): `assets\2d\tiles.png` (16 px タイル 8 種: 地面上 / 中 / 左縁 / 右縁 / 一方通行板 / 壊れるブロック / 背景 2 種) + `.meta` (filter point、PPU 16、sprites 8)、`assets\2d\hero.png` (32 px、Idle 2 コマ + Walk 4 コマ) + `.meta`、`assets\2d\crate.png` (16 px)、`assets\2d\bg_far.png` / `bg_near.png` (256×64)、`hero_normal.png` (法線)、`assets\2d\hero_idle.spriteanim.json` / `hero_walk.spriteanim.json`、`hero.controller.json` (v2、`"sprite"` ステート 2 つ、float パラメータ `speed` で遷移、Walk の tick 6 に `sound` イベント = 既存のサウンドアセットが無ければ `noise`)、`assets\2d\level.tilepalette.json` (ルール 4 種、アニメ 1 種、hp 3 のブロック)、`assets\2d\level.tilemap.json` (3 レイヤー 60×20)、`assets\2d\hill.spriteshape.json`、`assets\2d\hill_profile.spriteshape.json`。生成は決定的 (seed 固定)。外部素材は使わない。
2. `BuildTwoDShowcaseScene(EngineContext&)` (`DemoContent.cpp`、`kShowcases` に `{ L"--2d-demo", false, L"cache\\2d_showcase.scene.json", nullptr, &BuildPlain<&BuildTwoDShowcaseScene>, false }`。`--2d-night` オプションは `EngineCli` に足し、Build 関数が Light2D を Global 暗 + Spot 2 で組む): spec 4.1.14 の全部品。主人公は `PlayerInputComponent` + GameLogic の小さなスクリプト (`src\GameLogic\Scripts\TwoDHero.cpp`: 左右で `moveInput.x`、ジャンプで `jumpSpeed`、下で `ignoreOneWay`、`speed` パラメータを `AnimatorSetFloat`、攻撃で `TilemapDamageCell` の前方セル) + `--synth-input` の合成入力 (既存 `--flow-demo` の流儀)。敵は GridAgent2D + BT (MoveTo 主人公)。
3. golden: `shot_verify.bat` に `call :shot 2d --2d-demo` (frame 3) と `call :shot 2d_lights --2d-demo --2d-night` (`set SHOT=... --frames 125 --shot-frame 120` の既存流儀)。tol 既定。`tests\golden\2d.png` / `2d_lights.png` を `tests\actual\` からコピー (全部 `--update` しない)。
4. replay_verify: ジョブ `2d` (`:job_2d`: `del cache\2d_showcase.scene.json` → `call :chain cache\golden_2d.rep "--2d-demo --synth-input" "--2d-demo"`)。`-Jobs` 列、`:failed` の mismatch 分岐、シーン数のコメント、`tools\mcp\src\verify.rs` のジョブ表 (規則 15-e)。
5. 不変量と性能: `--selftest` に `TwoDDemoSelfTest` (デモシーンを HeadlessSim で 3000 tick 回し、全 plane2D 剛体の z / vz / wx / wy / qx / qy を検査。Debug / Release のハッシュ焼き込み)。`Runtime.exe --2d-demo --warp --frames 300` の平均フレーム時間 (`PerfDrawBenchmark` の流儀でログ) を Release で記録 → 16 ms 以内。
6. `docs\test_checklists.md` の M94 節に手動確認項目 (sub-24 が仕上げるので下書きでよい)。
9. (sub-22 の申し送り) Create → 2D → Camera 2D が CameraComponent.orthoSize を `refResolutionY / (2 × assetsPPU)` (既定 320×180 / PPU 100 なら 0.9) で初期化しているか確認し、していなければ 1 行足す (spec 4.1.12)。デモのカメラは orthoSize 5.625 (PPU 16 / ref 180)、Camera2DFollow.boundsFromTilemap = タイルマップ、useBounds = true。全体 selftest で NavAgent 庭の焼き値 8581BEE4A676337C を確認する。コンポーネントの TypeId は着手時の末尾から (スキーマ登録の型と衝突しないこと)。
8. (sub-14 の申し送り) 供給口 `Collect` を毎クエリ呼ぶコスト: デモで 1 tick に撃つ Raycast / BoxCast の本数と、物理 + スクリプト + Perception の 1 tick 時間をログ。1 tick の sim が 2 ms を超えたら、`StaticShapeSource` の全体 Collect を tick 単位でキャッシュする (tick 境界で無効化)。
7. (spec R11) `Runtime.exe --2d-demo --froxel --deferred --screenshot` を 1 枚撮り、正射影 + froxel で壊れない (真っ黒・NaN にならない) ことを目視。golden には登録しない。

## やらないこと (このサブでは)
- engine_spec / ADR / README (sub-24)、新機能の追加 (見つかった不具合は該当サブの範囲として「仕様との差分」に出す)

## 触る場所 (planner の見立て)
- 新規 `tools\gen_2d_assets.py`、`assets\2d\**`、`src\GameLogic\Scripts\TwoDHero.cpp`、`src\Engine\Engine\Demo\TwoDDemoSelfTest.{h,cpp}`
- `src\Engine\Engine\Demo\DemoContent.{h,cpp}`、`ShowcaseScenes.cpp:24-69`、`src\Engine\Engine\App\EngineCli.cpp`
- `tools\shot_verify.bat` (:117-417 の末尾)、`tests\golden\2d.png` / `2d_lights.png`、`tools\replay_verify.bat` (:84, :job_*, :failed, コメント)、`tools\mcp\src\verify.rs`
- `EditorMain.cpp`、vcxproj (GameLogic / Engine)、`docs\test_checklists.md`
- 参考: `demo-entity-order-is-rng-stream` (生成順は RNG ストリーム — デモへ足すなら末尾)、`screenshot-probe-recipes`、`replay-verify-triage`

## 受け入れ条件 (このサブ)
1. (spec 38、**チェックポイント = sub-21〜23 の分、spec 4-5**) `tools\shot_verify.bat` が 32 枚 PASS (既存 30 枚は maxDiff=0)、`tools\replay_verify.bat` の `2d` を含む全ジョブ PASS、`Editor.exe --selftest` 全体 (Debug / Release) に新 FAIL 無し、`check_rules` 15-e が 0。割れたら sub-21〜23 のどれが原因かを特定して SELF_EVAL に書く。
2. (spec 39) `TwoDDemoSelfTest` PASS (Debug / Release)、300 フレーム平均のログ (SELF_EVAL に数値)。
3. golden 2 枚を目視できる形で `plans\m94-2d\screenshots\` にもコピー (reviewer 用)。
4. (spec 1-3) 0 警告、check_rules 0、`MYE_SELFTEST_ONLY=m94` に `TwoDDemoSelfTest` を足して PASS、版 bump なし。

## 検証コマンド
- `python -I tools\gen_2d_assets.py`
毎サブ必須: Debug / Release ビルド、`pwsh tools\check_rules.ps1`、`pwsh tools\gen_project_files.ps1`、`MYE_SELFTEST_ONLY=m94` の selftest (両構成)
**チェックポイント (このサブで回す。golden を新設するので必須)**: `Editor.exe --selftest` 全体 (Debug / Release)、`tools\shot_verify.bat` (32 枚)、`tools\replay_verify.bat` (`2d` 込み)
- `bin\x64\Release\Runtime.exe --2d-demo --warp --no-audio --frames 300` (PowerShell ツールから)

## 実装メモ (coder が追記)

SELF_EVAL: sub-23 (round 1)

実装:
- `tools\gen_2d_assets.py` — 標準ライブラリだけで `assets\2d\` を生成 (tiles / hero / hero_normal / crate / bg_far / bg_near / hill_* の PNG と .meta、`*.spriteanim.json` ×2、`hero.controller.json` (v2、float `speed`、Walk の tick 6 に `step_hard` の sound)、`level.tilepalette.json` (ルール 4 種 + アニメ 1 + hp 3)、`level.tilemap.json` (3 層 60x20)、`hill_profile.spriteshape.json`、敵の `chase.bt.json` / `chase.bb.json`)。GUID 固定、seed 固定、2 回回して全ファイルのバイト一致を確認済み
- `src\Engine\Engine\Demo\DemoContent.cpp` (末尾) `BuildTwoDShowcaseScene(ctx, night, stress)` — spec 4.1.14 の全部品。カメラ orthoSize 5.625 / tonemapMode 0 / PixelPerfect 320x180 PPU 16 / Camera2DFollow (boundsFromTilemap = Level、useBounds)。`DemoContent.h` に宣言
- `src\Engine\Engine\Demo\ShowcaseScenes.{h,cpp}` — `kShowcases` に `--2d-demo`、`ShowcaseOptions.twoDNight` / `twoDStress`、専用ラッパ `BuildTwoD`。`RuntimeMain.cpp` / `EditorMain.cpp` / `ServerMain.cpp` が `--2d-night` / `--2d-stress` を解析
- `src\GameLogic\Scripts\ZTwoDHero.cpp` — `ZTwoDHero` (MoveX / Jump / MoveY < 0 で ignoreOneWay / Fire か壁への押し付けで前方 2 セルへ TilemapDamageCell / `speed` を AnimatorSetFloat / flipX) と `ZTwoDMover` (kinematic の移動床の velocity)
- `src\Engine\Engine\Demo\TwoDDemoSelfTest.{h,cpp}` — HeadlessSim + SynthLaneInput で 3000 tick。全 plane2D 剛体 (4) と CC (2) の z / vz / wx / wy / qx / qy を毎 tick 検査、WorldHash 焼き込み 0x55DEACAD78012785 (Debug == Release)、1 tick の sim 時間と供給口つきクエリ 1 本の時間をログ。`EditorMain.cpp` の全体 / m94 の両リストに追加、`MYE_SELFTEST_ONLY=twod` の単独入口を追加
- `tools\shot_verify.bat` — `2d` / `2d_lights` (MYE_SHOT_SKIP_2D で CI 側から外せる)。`tests\golden\2d.png` / `2d_lights.png`、`plans\m94-2d\screenshots\` に同じ 2 枚
- `tools\replay_verify.bat` — ジョブ `2d` (-Jobs 列、`:job_2d`、`:failed` の mismatch 分岐、diagnose の引数コメント、シーン数 15)。`tools\mcp\src\verify.rs` のジョブ表 22 件とテスト (件数、名前の文字種を小文字 + 数字に)。`docs\mcp-server.md` の件数
- `src\Editor\Widgets\CreateMenu.cpp` — Create → 2D → Camera 2D が orthoSize を PixelPerfect の既定から `refResolutionY / (2 × assetsPPU)` (= 0.9) で初期化 (やること 9)
- `docs\test_checklists.md` — M94 節の下書き
- build\*.vcxproj / filters は `tools\gen_project_files.ps1` の出力 (生成物)

仕様との差分:
- [逸脱] スクリプト名 `TwoDHero` → `ZTwoDHero` (ファイルも `ZTwoDHero.cpp`) — スクリプト名 (ファイル名) の並びで TypeId が決まる既存の規約 (WaveWalker.cpp などのヘッダ) に従い、既存スクリプトの TypeId を動かさないため末尾の 'Z' 始まりにした
- [追加] `ZTwoDMover` (同じファイル) — kinematic 移動床を動かす手段が仕様に無かった。velocity を書くだけ (plane2D + kinematic の積分に任せる)
- [逸脱] `--2d-night` の解析は EngineCli ではなく `ShowcaseOptions` + Runtime / Editor / Server の 3 Main (`--terrain-lod` と同じ流儀)。EngineCli の表は `EngineLoopConfig` へ書く口で、シーンの Build 関数へ値が届かないため。行は `BuildPlain` でなく専用ラッパ `BuildTwoD`
- [追加] `--2d-stress` (`ShowcaseOptions.twoDStress`、`AddTwoDStress`) — spec 4.4 の性能条件「Sprite2D 2000 枚 + タイル 100x50x3 層で 16 ms 以内」は通常のデモの量では測れない (台帳 sub-10 の「2000 枚との合算は sub-23」)。画面へ全部詰める (タイルは cellSize 0.2、衝突なし、メモリ上登録)。既定は無効で golden / replay には影響しない
- [逸脱] タイル 8 種 → 9 種 (前景用の tuft を足した。前景レイヤを不透明な正方形にしないため)。背景 256x64 → bg_far 256x192 / bg_near 256x96 (PPU 16 で表示高さを覆うため。整数倍の拡大を避けた)。hill のアセットは `hill.spriteshape.json` と `hill_profile.spriteshape.json` の 2 件書きだったが、プロファイル 1 件 (`hill_profile.spriteshape.json`) + 縁 3 / 塗りの PNG 4 枚にした
- [逸脱] 配置: 主人公は一方通行板の上 (10.5, 5.9) から始まり、敵は段差の右 (17.5)、丘は左端のポケット (A* がタイルしか見ないので、敵の経路に丘を置かない)、壊れるブロックはその右。合成入力が左へ偏る (A 1/2 : D 1/4) ので、主人公は左へ流れてブロックを掘り、左壁と丘に着く。ベルトと箱 3 個と移動床は x = 19〜36 (初期の画面の右端か外)
- [追加] `MYE_SELFTEST_ONLY=twod` — 焼き込みハッシュを 1 本だけ採るための入口 (`computescript` と同じ並び)
- [未実装] 供給口 `Collect` の tick 単位キャッシュ — やること 8 の条件 (1 tick の sim > 2 ms) を満たさないので入れない (下の測定)。デモが 1 tick に撃つ Raycast / BoxCast は 0 本 (スクリプト・BT・Perception はクエリを使わない、GridNav はタイルのセル値を直接読む)
- [未実装] 「PerfDrawBenchmark の流儀」の平均フレーム時間ログ — Runtime にフレーム時間のログが無く、新設は範囲外。代わりに `--frames 10` と `--frames 310` の実行時間の差 (起動の約 6 秒を打ち消す) を 2 組取った

検証:
- `python -I tools\gen_2d_assets.py` → 成功。2 回回して `assets\2d` の全ファイルのバイト一致
- Debug / Release `/p:MyeWarnAsError=true` → 0 警告 (SelfTestUnused.kernel.hlsl の C4100 で 1 回目だけ落ち、2 回目で通る既知の癖)
- `pwsh tools\check_rules.ps1` → 3 error / 50 warning (基準どおり。EditorMain.cpp:805 `#ifdef _DEBUG` + Editor.bat / Setup.bat の LF)、15-e は 0
- `MYE_SELFTEST_ONLY=m94` Debug / Release → どちらも PASS 3092 / FAIL 0 (TwoDDemoSelfTest PASS)。Debug と Release のワールドハッシュはどちらも 0x55DEACAD78012785
- (チェックポイント 5) `Editor.exe --selftest` 全体 Debug / Release → どちらも PASS 8737 / FAIL 0、NavAgent 庭ハッシュ 8581BEE4A676337C を両方で確認
- (チェックポイント 5) `tools\replay_verify.bat` (全文 `cache\m94\replay_verify_cp5.log`) → 22 ジョブのうち 21 が個別に `done : <名> -> PASS` (`2d` を含む: record Debug / Debug stress 37 / Release / Server.exe の 4 経路すべて)。`rules` だけ FAIL (既存 3 error)。割れたジョブ無し
- (チェックポイント 5) `tools\shot_verify.bat` (`cache\m94\shot_verify_cp5.log`) → 32 枚 PASS。既存 29 枚は maxDiff=0 (tol 3 の判定の中で maxDiff を読んだ)。`demo_terrain_deferred` は golden と maxDiff=5 (既知) で、`cache\m94\baseline_terrain.png` と `--img-diff --tol 0` は maxDiff=0。新 golden 2 枚は同じ条件で撮り直して maxDiff=0。`--update` は使っていない
- (チェックポイント 5) `tools\build_managed.bat Release` → 成功、0 警告 0 エラー
- `cargo test --manifest-path tools\mcp\Cargo.toml` → 49 件 PASS (JOBS 22 件)
- 目視 (やること 7): `Runtime.exe --warp --font-embedded --width 960 --height 540 --frames 6 --shot-frame 3 --no-fxaa --2d-demo --froxel --deferred --screenshot` → 真っ黒でも NaN でもなく、forward の `2d` と画素一致 (`--img-diff --tol 0` で diffPixels=0)。正射影の 2D 経路は froxel / deferred のシーン光に影響されない
- 性能 (spec 4.4 / 受け入れ 39、Release、`--warp --no-audio --no-vsync --width 960 --height 540`): デモ本体は 310 フレームと 10 フレームの差から 0.88 / 1.39 ms/フレーム。`--2d-stress` (Sprite2D 2000 枚 + 100x50x3 層 = 15000 セルを全部画面内へ) は 4.43 / 4.60 ms/フレーム。どちらも 16 ms 以内。1 tick の sim は平均 0.213 ms (最大 1.06 ms、Release)、Debug は 2.6 ms (最大 16.7 ms)
- (やること 8) 供給口つきクエリ 1 本 (Release): Raycast 12.7 us、BoxCast 67.5 us (Collect が返す静的な箱は 86 個)。Debug は 188 / 686 us。1 tick の sim が 2 ms を超えないのでキャッシュは足していない
- (やること 9) Create → 2D → Camera 2D の orthoSize は未初期化 (既定 5.0) だったので 1 行足した。実際の Create メニュー操作は未実施 (CreateCamera2D の中身を読んで直しただけ)

自己採点 (1-5):
  仕様適合: 4 — 全部品・golden 2 枚・replay `2d`・selftest・性能・8 / 9 まで入れた。差分は上に列挙 (タイル 9 種、背景の寸法、スクリプト名、`--2d-night` の解析場所、`--2d-stress` の追加、PerfDrawBenchmark 形式のログ無し)
  正しさ: 4 — チェックポイントの 4 本 (全体 selftest 両構成、replay 21 ジョブ個別 PASS、shot 32 枚、build_managed) が通り、Debug == Release のハッシュと Server.exe の照合も通った。GUI の実操作 (Create メニュー / Scene View) と実 GPU は未確認
  コード品質: 4 — ヘッダ付きの新規 3 ファイル、定数に名前、コメントは日本語で理由。`BuildTwoDShowcaseScene` は長いが他のデモと同じ構え (部品ごとのブロック)
  テスト: 4 — 3000 tick の不変量とハッシュの焼き込み (両構成)、replay の 4 経路、golden 2 枚。敵が一方通行板へ着くこと・タイルが壊れることはハッシュに入るだけで個別の期待値は持たない (ログでは tick 500 に敵が板の上、主人公の攻撃が積み上がるのを確認)

不安・質問:
- 合成入力は左へ偏る (A 1/2 : D 1/4) ので、主人公は左壁と丘のあたりに留まり、壁へ押し付けるたびに攻撃回数が増える (破壊不可の壁でも `attackCount` は増える)。見栄えは golden 2 枚が決めるので許容したが、replay の被覆としては「左端で止まった主人公」が 600 tick の大半になる。ブロックの破壊 (tick 500 までに攻撃 3 回) と敵の板上り・ジャンプは 600 tick 内で起きている (selftest のログ)。被覆を厚くするなら合成入力側 (全体の関数) ではなくデモ側の配置で調整する
- 敵は動く物 (箱) を見ずに A* で進み、CC の押し出し (`pushPower` 3) で箱の山を動かす。ゲート (箱が通路を塞ぐ) が敵を永久に止めるシーンは作っていないが、保証はしていない (v1 の限界として sub-24 の限界欄へ)
- 丘 (SpriteShape2D) はタイルではないので GridAgent2D の A* に載らない。敵の経路を丘の上に置くと「立っている足場がタイルでない」ので Inactive / Stuck 気味になる。デモでは敵の経路の外 (左端) へ置いた
- `--froxel --deferred` は 2D の正射影経路に効かず forward と同じ絵になる (「壊れない」は満たす)。spec R11 の意図が「効いた絵」だったなら、limit として sub-24 に書く
- ADR-032 (固定 21 ジョブ) の記述は直していない (歴史的な記述として残した)。`docs\mcp-server.md` だけ 22 個に直した

触ったファイル:
- `tools\gen_2d_assets.py`、`assets\2d\**` (.png / .json / .meta の新規 36 ファイル)
- `src\Engine\Engine\Demo\DemoContent.cpp` / `DemoContent.h` / `ShowcaseScenes.cpp` / `ShowcaseScenes.h` / `TwoDDemoSelfTest.cpp` / `TwoDDemoSelfTest.h`
- `src\GameLogic\Scripts\ZTwoDHero.cpp`
- `src\Runtime\RuntimeMain.cpp`、`src\Editor\App\EditorMain.cpp`、`src\Server\ServerMain.cpp`、`src\Editor\Widgets\CreateMenu.cpp`
- `tools\shot_verify.bat`、`tools\replay_verify.bat`、`tools\mcp\src\verify.rs`、`docs\mcp-server.md`、`docs\test_checklists.md`
- `tests\golden\2d.png`、`tests\golden\2d_lights.png`、`plans\m94-2d\screenshots\2d.png`、`plans\m94-2d\screenshots\2d_lights.png`
- `build\Engine.vcxproj` / `.filters`、`build\GameLogic.vcxproj` / `.filters` (gen_project_files.ps1 の出力)

申し送り:
- 版: snapshot / ABI / kDocVersion / kCookVersion / TypeId はどれも触っていない (v65 / v31 = 188 / TypeId 93 まで)。新しいコンポーネント型も足していない
- sub-24: 限界欄へ — 敵は動く物と非タイルの足場を見ない / 合成入力の偏りで左端に溜まる / froxel は 2D に効かない / クエリ 1 本の時間 (Release: Raycast 12.7 us、BoxCast 67.5 us、1 tick の sim 0.21 ms)。engine_spec に `--2d-demo` / `--2d-night` / `--2d-stress`、ADR-033 に平面拘束の焼き値 (台帳)。ADR-032 の「固定 21 ジョブ」は 22 へ
- `cache\m94\replay_verify_cp5.log` / `shot_verify_cp5.log` / `selftest_full_{Debug,Release}.log` に全文 (git 管理外)
- デモを触るときの注意: タイルマップの配置を変えると `TwoDDemoSelfTest` の焼き値 (`kExpectedHash`) と golden 2 枚が変わる。`MYE_SELFTEST_ONLY=twod` で値を採る。生成スクリプトの GUID は `DemoContent.cpp` の `kTwoD*Guid` と同じ値で揃える
- check_rules の既存 3 error のうち EditorMain.cpp の `#ifdef _DEBUG` は行番号が 741 → 805 に動いた (この sub の追加行のため。中身は同じ)

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-22、チェックポイント 5 = sub-21〜23)。golden `2d` (昼、タイル 3 層・一方通行板・壊れるブロック・丘・ロープ・視差・主人公・敵・箱、ピクセルパーフェクト) と `2d_lights` (夜、Spot の影と法線、主人公の青い法線ハイライト、敵の追跡ジャンプ) を見た。チェックポイント: 全体 selftest 両構成 FAIL 0 (NavAgent 庭不動)、replay_verify 21 ジョブ個別 PASS (2d は 4 経路)、shot_verify 32 枚 PASS (既存 29 枚 0、terrain は基点と 0、新 2 枚は撮り直し 0)、build_managed Release、cargo test 49。3000 tick 不変量とハッシュ 55DEACAD78012785 両構成同値。性能 16 ms 以内。逸脱 (ZTwoDHero、ShowcaseOptions、資産の差) と追加 (ZTwoDMover、--2d-stress、twod 入口) は採用。限界の集約は sub-24 5b へ。
