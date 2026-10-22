# sub-25: レビュー round 1 の修正

- 依存: sub-24
- 状態: 未着手
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む** (特に 4.3「未定義動作へ依存しない」)。指摘の全文は `C:\HAL\MyEngin\plans\m94-2d\review-1.md`。spec: 4.1.5 (oneWay)、4.1.12 (TAA / FXAA)、5. 41〜45、8. 変更履歴 (reviewer round 1)

## やること
1. (review-1 #1、major) `Sprite2DPass` のパス開始時 (bindSpriteState) に s1 へ `samplerLinear_` を**常に**張る。ライトの無いバッチでも、前段 (影) の比較サンプラが s1 に残ったまま `sprite2d.hlsl` の PS を Draw しないこと (`gLightSamp : register(s1)`)。同じ観点で t1 / t2 / s2 など、PS が宣言しているスロットに前段の別種のリソースが残る経路が無いかも見る。
2. (review-1 #2、major) `ShaderManager::ReleaseGpu` (`ShaderManager.cpp:537-548`) に `SurfaceProgram` の `spriteVS` / `spritePS` / `spriteInputLayout` の Reset を足す。M94 で増えた他の GPU オブジェクト (Sprite2DPass の RT・サンプラ・状態、PixelPerfectBlitPass、ライト / 影 / 法線 RT) も M88 の解放契約 (ReleaseGpu で全部手放す) に入っているかを確認し、漏れがあれば同じ場所で直す。
3. (review-1 #9) `Sprite2DPass` の開始時に RS / Blend (factor・mask 込み) / DS (stencil ref 込み) の状態を `OMGet*` / `RSGetState` で控え、終了時に戻す (取得した COM 参照は Release する)。
4. (review-1 #5) 一方通行の判定 (`RejectOneWayContact` と CC の `CharPassesThroughOneWay` / `CharRejectsOneWayNormal`) で、床の天面を**床の tick 開始時の姿勢**から取る (mover の `startMinY` と時刻をそろえる。静的床は同じ値なので結果不変、kinematic / kinDrive の床だけ変わる)。selftest: 上昇速度 4 m/s の oneWay kinematic plane2D 床に CC と箱を載せ、120 tick 床の上に留まる (y が天面 ± 0.05)。対照として 1 m/s の床と、静的床の既存項目は不変。焼き値 (plane2D / CC2D / Shapes2D / tile / Hill / effector2D / 2d デモ) が動いたら、動いた理由 (動く oneWay 床がシーンにあるか) を示して再採取する。spec 4.4 の焼き値規則どおり。
5. (review-1 #10) 主カメラに PixelPerfectCamera2D がある間は、zoom 1 / upscaleRT = false / pixelSnapping でも TAA ジッタと FXAA を止める (`RenderSystem.cpp` の DecideTaaAndFroxel / ResolvePost の条件を「drawLowRes」から「PixelPerfect が有効」に)。ログは従来どおり 1 回。
6. (review-1 #3) `engine_spec.md:3026` の `tools\mcp\srcerify.rs` (垂直タブ 0x0B に化けた) を `tools\mcp\src\verify.rs` に直す。`grep -c $'\v' engine_spec.md` が 0。M94 で触った他の文書 (ADR-033、test_checklists、README) にも制御文字が無いことを同じ方法で確認。
7. (review-1 #4) `engine_spec.md:2930` の限界欄を「a plane2D body in an active region never sleeps」に一般化。限界欄に次の 2 行も足す: v1 の oneWay は水平な床だけ (review-1 #6)、PixelPerfect 中は TAA / FXAA が常に止まる (#10)。
8. (review-1 #7) `docs\test_checklists.md` の M94 節に 4 行: (a) Perception の XY の知覚扇形ギズモ (b) SpriteShape2D の透視カメラ・回転 / スケール付きの親での見た目 (c) Light2D の Inspector の実操作 (d) タイルパレットのキー U (矩形)。

## やらないこと (このサブでは)
- 傾いた oneWay 床の対応 (v1 は水平だけと決めた)
- check_rules の既存 3 error (別件、Notion Q-9)

## 触る場所 (planner の見立て)
- `src\Engine\Renderer\Passes\Sprite2DPass.{h,cpp}` (:763, :807-842, :875-882)、`assets\shaders\sprite2d.hlsl` (:46、読むだけのはず)
- `src\Engine\Renderer\Shader\ShaderManager.cpp:537-548`、`SurfaceProgram.h:69-71`、参考 `src\Engine\Renderer\Device\DeviceRecovery.cpp:29-40`、`GraphicsDevice.cpp:159-167`
- `src\Engine\Engine\Physics\Rigid\PhysicsSystem.cpp` (:343-367 `RejectOneWayContact`、:1382-1392 / :1540 CC の一方通行)、`PhysicsSelfTest.cpp` (CC2D 節)
- `src\Engine\Engine\Rendering\RenderSystem.cpp` (:683 付近、:896、:2316)
- `engine_spec.md` (:2930、:3026)、`docs\test_checklists.md` (:714-780)

## 受け入れ条件 (このサブ)
1. (spec 41) `tools\replay_verify.bat` の全文ログ `cache\m94\replay_verify_cp6.log` の [2d] 節で D3D の ERROR が 0 件 (grep の件数をログに)。他ジョブも 0 件のまま。
2. (spec 42) SpriteFx 材質のスプライトを含む temp シーン (または `--2d-demo` に一時的に材質を付けたもの) で `--simulate-device-lost 10` → 「external references: 0」と復旧完了のログ。修正前に同じ手順で復旧が打ち切られることを先に再現する (故障点で検証)。
3. (spec 43) 上昇床の selftest が両構成 PASS。修正前に同じ selftest が FAIL することを先に確認する。
4. (spec 44・45) 状態の復元と TAA / FXAA の条件をコードで示し、`--2d-demo --taa --deferred` (PixelPerfect zoom 3) と、upscaleRT = false の variant で TAA / FXAA が止まるログ。
5. (review-1 #3 #4 #7) 文書 3 件の修正、`grep -c $'\v'` が 0。
6. **チェックポイント** (spec 4-5): 全体 selftest 両構成 FAIL 0、replay_verify 全ジョブ個別 PASS (rules は既存 3 error)、shot_verify 32 枚 PASS + 既存 30 枚と新 2 枚を `--tol 0` (terrain は基点と)、build_managed Release。golden が動いたら原因を断定してから扱う (`--update` で塗り潰さない)。
7. 毎サブ必須: 両構成 `/p:MyeWarnAsError=true` 0 警告、check_rules 3 / 50 で増減なし、`MYE_SELFTEST_ONLY=m94` 両構成 FAIL 0、版 bump なし (シミュレーションの形を変えない — 一方通行の判定の値が変わるだけ)。

## 検証コマンド
毎サブ必須: Debug / Release ビルド、`pwsh tools\check_rules.ps1`、`MYE_SELFTEST_ONLY=m94` の selftest (両構成)
**チェックポイント (このサブで回す)**: `Editor.exe --selftest` 全体 (Debug / Release)、`tools\replay_verify.bat` (全文を `cache\m94\replay_verify_cp6.log`)、`tools\shot_verify.bat` + `--tol 0`、`tools\build_managed.bat Release`
固有: `Runtime.exe --scene <temp 材質シーン> --simulate-device-lost 10`、`Runtime.exe --2d-demo --taa --deferred --screenshot`

## 実装メモ (coder が追記)

SELF_EVAL: sub-25 (round 1)
実装:
  - #1 Sprite2DPass.cpp bindSpriteState — s1 へ samplerLinear_ を常に張る (ライト無しバッチでも前段の比較サンプラを残さない)。replay_verify cp6 の `comparison filtering` は 0 件 (cp5 は 196)
  - #2 ShaderManager.cpp ReleaseGpu — spriteVS / spritePS / spriteInputLayout の Reset を追加。Sprite2DPass の RT・サンプラ・状態、PixelPerfectBlitPass、ライト / 影 / 法線 RT は RenderSystem::ReleaseGpu が `Sprite2DPass()` / `PixelPerfectBlitPass()` / `RenderTexture()` へ差し替えており漏れなし (確認のみ)
  - #3 Sprite2DPass.cpp Render — 開始時に RSGetState / OMGetBlendState (factor・mask) / OMGetDepthStencilState (ref) を控え、終了時に RS / Blend / DS を戻す (従来の「useStencil || lighting のとき blend_ / depth_ を張って返す」を置き換え)。RT / DSV の復元は従来どおり
  - #5 PhysicsSystem.cpp — Body に startPy を追加 (sub 0 で startMinY と同じ場所に記録)。OneWayFloorShift(floor) = rb を持つ床の (現在 pose.py - startPy)、静的床は 0。RejectOneWayContact と CharPassesThroughOneWay の天面から引く (床の tick 開始時の姿勢)。PhysicsSelfTest.cpp に CC2D (10) を追加: 上昇 1 / 4 m/s の oneWay kinematic plane2D 床に CC と箱を 120 tick 載せる (6 項目)
  - #10 RenderSystem.cpp — f.pixelPerfect = true (PixelPerfectCamera2D が有効な間は常に)。TAA ジッタ (view.taaEnabled) と FXAA の無効化条件が drawLowRes から PixelPerfect 有効に変わる。ログは 1 回のまま、低解像度 RT を使わないときは別文面
  - #3 engine_spec.md:3026 の垂直タブを `tools\mcp\src\verify.rs` に直した。0x0B は engine_spec.md / test_checklists.md / README.md とも 0 (M94 で触った *.md / *.rs / *.bat / *.ps1 を 25fb355..HEAD で調べ、残るのは plans\m94-2d\sub-20.md の 4 バイトだけ = planner の文書、触っていない)
  - #4 engine_spec.md 限界欄 — 「a plane2D body in an active region never sleeps」に一般化。「oneWay は水平な床だけ」「PixelPerfect 中は TAA / FXAA が常に止まる」を追記 (Q-19)
  - #7 docs\test_checklists.md — M94 節に 4 行 (Perception XY の知覚扇形ギズモ / SpriteShape2D の透視カメラ・回転 / スケール付き親 / Light2D の Inspector 実操作 / タイルパレットのキー U)
  - #9 は「コメントで固定」ではなく、状態を控えて戻す方を実装
  - #6 (planner 宛) は対象外
仕様との差分:
  - [追加] 上昇床の selftest で、動的な箱は最初の 2 tick を判定から外した — 止まった箱が 1 tick ぶん先に動いた床へ追いつく遅れ (0.07 m) で、床の判定の欠陥ではない
  - [追加] RenderSystem のログ文面を、低解像度 RT を使わない PixelPerfect 用にもう 1 本足した (「ログは従来どおり 1 回」)
  - [追加] 剛体側 (RejectOneWayContact) にも同じ shift を入れた。修正前の selftest は 4 m/s でも箱が FAIL せず (CC だけ FAIL) 実害は未確認だが、CC と時刻をそろえるため (静的床は shift 0 で結果不変)
検証:
  - 修正前の再現 (#2): TEMP で DemoContent の主人公スプライトに SpriteFx 材質を付け、Release Runtime `--warp --2d-demo --frames 40 --simulate-device-lost 10` → exit 6、`old device external references: 3 (allowed 0)` で打ち切り (cache\m94\s25\dl_before.out)。ReleaseGpu 修正後 → exit 0、`external references: 0`、`device recovered ... world hash ... unchanged=1` (dl_after.out)。TEMP は git checkout で消した
  - 修正前の再現 (#5): Release Editor `MYE_SELFTEST_ONLY=m94 --selftest` → `FAIL: CC2D rising oneWay (4 m/s): the character stays on the lift for 120 ticks` (cc y -18.883)、1 m/s は PASS (st_before.out / st_before2.out)。修正後は Debug / Release とも 4 m/s: cc y 8.900 (天面 8.000 + 0.9)、箱 8.249 で全 PASS
  - #1: Debug Runtime `--2d-demo --2d-night` (SpriteFx 材質あり、debug layer: on) → comparison 0 件。replay_verify cp6: `comparison filtering` 0 件、ERROR 行 0 件 (INFO の `D3D11 device created` 78 行のみ。cp5 は 274 行 = ERROR 196 件を含む)
  - #10: Release Runtime `--2d-demo --taa --deferred` 960x540 → `PixelPerfectCamera2D zoom 3, RT 320x180 -> 960x540 (TAA / FXAA are turned off)`、300x200 (zoom 1、RT なし) → `zoom 1, drawn at 300x200 without a low-res RT (TAA / FXAA are turned off)`。TAA / FXAA が止まった画素までは未確認 (コードで view.taaEnabled = 0 / effective.fxaa = false)
  - MSBuild MyEngine.sln Debug / Release `/p:MyeWarnAsError=true` → 0 警告 0 エラー。ただし Release は「/WX なしで直前に Editor だけ Release ビルドした直後」の 1 回目だけ `SelfTestUnused.kernel.hlsl(10,1): error C2220` (C4100 'skipped' / 'params') で落ち、同じコマンドの 2 回目で通った (2 回再現。このファイルと該当 cpp は触っていない = 既存の再ビルド順依存の癖)
  - `MYE_SELFTEST_ONLY=m94` Debug / Release → 修正後 FAIL 0
  - `Editor.exe --selftest` 全体 (Debug / Release 並行) → exit 0、FAIL 0、PASS 8743 (Release、cp5 の 8737 に +6 = 新規 6 項目)。ログ cache\m94\selftest_full_cp6_{Debug,Release}.log
  - `tools\replay_verify.bat` (MYE_MSBUILD_ARGS=/p:MyeWarnAsError=true、全文 cache\m94\replay_verify_cp6.log) → 22 ジョブ中 21 個別 PASS (demo parts flow mp physics acoustic joints ui fracture perception nav bt anim 2d compute jobsab computeab ttdebug ttrelease whatifrelease whatifdebug)、rules だけ FAIL = 既存 3 error (50 warning)。VERIFY PASS 53 行 (cp5 と同数)
  - `tools\shot_verify.bat` → 32 枚 PASS (cache\m94\shot_verify_cp6.log)。31 枚は maxDiff=0、demo_terrain_deferred は golden と maxDiff=5 (既知)、基点の絵 cache\m94\baseline_terrain.png とは `--tol 0` で maxDiff=0。golden は動いていない (--update は使っていない)
  - `tools\build_managed.bat Release` → exit 0。`pwsh tools\check_rules.ps1` → 3 error / 50 warning (増減なし)
  - 焼き値 (2d デモ 55DEACAD78012785、CC2D 0xC04B7FFA3E31E5D0 ほか) は動かなかった (selftest 全 PASS、replay 2d PASS)。再採取なし
  - 未実施: SpriteFx 材質付きシーンのデバイス消失を selftest / replay へ固定すること (ログ証跡のみ)。TAA / FXAA 停止の画素比較。実 GPU の Debug 層の目視
自己採点 (1-5):
  仕様適合: 4 — 指摘 1〜5・7・9・10 と文書 3 件を直し、修正前に FAIL / 打ち切りを再現した。限界欄も追記
  正しさ: 4 — 上昇 4 m/s の CC は修正前 FAIL → PASS、デバイス消失は参照 3 → 0。静的床は shift = 0 で結果不変 (焼き値・replay 不変)。傾いた床と 4 m/s 超の限界は未検証 (v1 は水平だけ)
  コード品質: 4 — 変更は小さく局所的。startPy は startMinY と同じ場所、OneWayFloorShift は 1 本で 2 呼び出しが共有
  テスト: 4 — 新規 6 項目 (1 / 4 m/s の CC・箱)。デバイス消失は手動ログ証跡のみで自動化していない
不安・質問:
  - 上の [追加] 3 点の確認。特に剛体側の shift は修正前に FAIL しなかったので不要だった可能性がある (対称性のために入れた。不要なら CC 側だけにできる)
  - Release の SelfTestUnused.kernel.hlsl C4100 は既存の癖 (別件)
触ったファイル:
  - src\Engine\Renderer\Passes\Sprite2DPass.cpp
  - src\Engine\Renderer\Shader\ShaderManager.cpp
  - src\Engine\Engine\Physics\Rigid\PhysicsSystem.cpp
  - src\Engine\Engine\Physics\Rigid\PhysicsSelfTest.cpp
  - src\Engine\Engine\Rendering\RenderSystem.cpp
  - engine_spec.md
  - docs\test_checklists.md
  - plans\m94-2d\sub-25.md (実装メモ節のみ)
申し送り:
  - plans\m94-2d\sub-20.md に制御文字が 4 バイトある (planner 文書。別件)
  - cache\m94\s25\ に再現ログ (dl_before / dl_after / st_before / pp_zoom3 / pp_zoom1 など) を残した (git 管理外)

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-22、チェックポイント 6)。#2 は修正前の打ち切り (external references 3) を再現してから 0 に、#5 は修正前の FAIL (cc y −18.883) を再現してから PASS に — 故障点で検証。#1 は cp6 で comparison / ERROR 0 件 (cp5 は 196)。チェックポイント 6: 全体 selftest 両構成 FAIL 0 (PASS 8743)、replay_verify 21 ジョブ個別 PASS (rules は既存 3 error)、shot_verify 32 枚 PASS + 31 枚 tol 0 と terrain は基点と 0、build_managed Release、焼き値・版不変。[追加] 3 点はすべて採用 (剛体側の shift は spec 4.1.5「床の天面は tick 開始時の姿勢」の文言どおりで、静的床は結果不変)。sub-20.md の制御文字 4 バイトは planner が直した。
