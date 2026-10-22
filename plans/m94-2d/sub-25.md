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

## フィードバック履歴
