# sub-09: 文書と通し検証

- 依存: sub-08、sub-04、sub-05 (BVH のサブを足した場合はそれも)
- 状態: OK (コミットは司会)
- 往復: 1

## やること
spec 受け入れ条件 8, 12, 13, 14。
- `engine_spec.md`: §6.14 に「切り替えとプロジェクト設定」を書き直し (4 種、キー、既定、CLI の優先)、距離・画面サイズ・追加経路・局所影 (「The local-light shadow atlas still sees on-screen casters only.」の文を直す)・デバッグを足し、CLI 表を更新。§6.6 の「No frustum culling」行を消す。機能一覧表 (:430-433 付近) と M99 の行を足す。契約番号 (TypeId / SimSnapshot) の記述を実コードに合わせる。
- `docs\adr\ADR-038-culling.md`: sub-01 の計測節に加え、決定 (spec 2. の D2〜D18 の要点、却下した案と理由、公式情報の URL) を書く。
- `docs\test_checklists.md`: カリングの手動確認項目 (各スイッチ、Scene View のトグル、固定、枠表示、Project Settings の保存、Scene View で動く GPU エミッタが画面端で消えないこと、Scene View の 2D で画面端のスプライト・線・シェイプが消えないこと、Rendering > カリング の固定と枠表示をエディタで操作して使えること、距離の枠の青が見分けられるか)。
- `tools\cull_verify.bat` (新規): `Runtime.exe` で視錐台 ON と `--no-frustum-culling` を撮り、`Editor.exe` の img-diff `--tol 0` で照合する。対象は既定ベンチ・`--culling-demo`・`--culling-2d-demo`・`--particle-demo`・`--2d-demo`・`--render-demo`。撮影条件は `shot_verify.bat` に揃える (`--warp`・`--font-embedded`・`--no-fxaa`、`cache\` の保存済みシーンを先に消す)。`docs\test_checklists.md` と engine_spec の検証表に載せる。CI に載せるかは実行時間を測ってから決め、SELF_EVAL に書く。
- `ResolveOcclusionCulling` (`TagNames.cpp`) は sub-02 から本番経路で使われていない。EngineCliSelfTest の旧検査を `ResolveCullingSettings` 側へ寄せて削除するか、残す理由をコメントに書く。
- engine_spec §6.14 の制限に、spec 7. の既知の制限 2 件 (`hasScene == false` で局所影アトラスを作らない、アトラスのスキンは bind 箱で判定) を書く。
- `flow_title` の shot_verify FAIL (sub-03 で発見、diffPixels 944) の出所を切り分ける: 基点 `d52e91f3` を別ディレクトリへ出して Release をビルドし、同じ撮影条件で `flow_title` を撮って golden と比べる。基点でも同じ差なら M99 と無関係として報告 (golden は更新しない)、基点で一致するなら M99 のどのサブが原因かを特定して SELF_EVAL に書く。
- sub-08 からの持ち越し: (1) `Runtime.exe --screenshot` を `--frames` 無しで使うと撮影後に終了せず CPU を使い続ける件を、基点 `d52e91f3` の exe でも起きるか確かめる。基点でも起きれば M99 と無関係として報告、M99 で起きるようになったなら原因を特定して直す (直すのはこのサブの範囲。挙動の変更として SELF_EVAL の差分に書く)。(2) engine_spec に固定の仕様 (viewKey 別、固定 ON 後の最初の描画で控える、LOD・ライト・CSM・オクルージョン・地形・視差は今のカメラ) と `--cull-freeze-frame` / `--cull-debug`、枠の色と上限を書く。(3) cull_verify に「`--cull-freeze-frame 10 --render-bench-cut-frame 20` の `culled` がカット無しの値と一致」を足す。
- 通し検証: `Editor.exe --selftest-full`、`tools\replay_verify.bat`、`tools\shot_verify.bat`、`tools\check_rules.ps1`。既定ベンチの `counts` を M99 前 (基点 `d52e91f3`) と比べ、新しい欄以外の一致を確かめる (受け入れ条件 8)。
- Debug / Release / `--warp` の `counts` 一致 (既定ベンチ、`--culling-demo`、`--culling-2d-demo`、`--culling-distance-demo`、既定ベンチ + `--cull-min-screen-size 0.02`)。比較は目視ではなく機械で行う (JSON の `counts` 節を取り出して完全一致を判定するスクリプト。cull_verify.bat に同居させてよい)。

## やらないこと (このサブでは)
- 機能の追加・挙動の変更 (見つけた不具合は SELF_EVAL に書き、planner が差し戻し先を決める)

## 触る場所 (planner の見立て)
- `engine_spec.md` §6.6 (:590-640)、§6.14 (:1009-)、機能表、マイルストーン表 (:3973 付近)、ADR 一覧 (:4037 付近)
- `docs\adr\ADR-038-culling.md`、`docs\test_checklists.md`

## 受け入れ条件 (このサブ)
0. `tools\cull_verify.bat` が全シーン PASS (diffPixels = 0)
1. 文書の記述がコードと一致 (キー名・既定値・CLI 名・TypeId・SimSnapshot 版)
2. `--selftest-full` PASS、`replay_verify` PASS、`shot_verify` PASS (sub-03 で説明付き更新したものを除く)、`check_rules` 0 error
3. 既定ベンチの `counts` が基点と一致 (新しい欄を除く)、3 シーンの `counts` が 3 構成で一致

## 検証コマンド
- `bin\x64\Debug\Editor.exe --selftest-full`
- `tools\replay_verify.bat`、`tools\shot_verify.bat`、`tools\check_rules.ps1`
- `--render-stats-dump` の比較

## 実装メモ (coder が追記)

SELF_EVAL: sub-09 (round 1)
実装:
  - tools\cull_verify.bat (新規、CRLF): (1) 視錐台 ON/OFF の全画素一致 (img-diff --tol 0) を 9 組 (既定ベンチ / --culling-demo / --culling-2d-demo / --particle-demo の CPU・GPU / --2d-demo の昼・夜 / --render-demo の Forward・Deferred)、既定ベンチは OFF で culled = 0・drawCalls 増も確認。(2) 固定: --cull-freeze-frame 10 に --render-bench-cut-frame 20 を重ねた culled 等 4 欄 = カット無し、固定無しのカットは culled が動く (空振り防止)。(3) counts を Release --warp 基準に Release 実 GPU・Debug 実 GPU と比較 (既定ベンチ / --culling-demo / --culling-2d-demo / --culling-distance-demo / 既定ベンチ + --cull-min-screen-size 0.02)。(4) 任意: MYE_CULL_BASE_ROOT の基点 exe と既定ベンチの counts を比較 (新欄 4 つを除く)。MYE_CULL_SKIP_CONFIGS=1 で (3) を飛ばす。撮影条件は shot_verify.bat に揃え、cache\ の保存済みシーンを先に消す。
  - tools\cull_counts.ps1 (新規): dump の counts 節を欄ごとに比べる (完全一致 / -Ignore / -Only / -View / -ExpectDiff / -Greater / -Zero)。views は viewKey で引く。
  - engine_spec.md: §6.14 の「切り替え」を 4 種・キー・既定・CLI の優先に書き直し、視錐台スイッチ・距離・RenderCulling・画面サイズ・追加経路・局所影 (M90d の「still sees on-screen casters only」の文も直した)・デバッグ (固定の仕様・枠の色と上限 4096・理由別の数)・ベンチ物量と BVH 見送り・既知の制限 2 件 + 他・検証を追記。CLI 表に 5 行。§6.6 の「No frustum culling」行を削除。§6.2 の機能表に 1 行、§11.3 に cull_verify の段落、§12.2 に M99 の行と見出し (M0-M99)・番号の行・ADR 一覧に ADR-038、Last Updated。
  - docs\adr\ADR-038-culling.md: 「途中」→ 確定。決定の表 (D1〜D18 と却下した案)、公式情報の URL、検証の作り、結果・既知の制限を追加。計測の節は sub-01 のまま、nit 1 (判定は早期脱出するので落ちる物が多いほど線形の判定は安く、BVH で削れる上限は 0.477 ms) を反映し、旧文「ステージ 2 の判定費用は落ちる数によらない」を直した。
  - docs\test_checklists.md: M99 の節 (スイッチと保存、Scene View / Game View、固定・枠表示、距離の青の見分け、Scene View の GPU エミッタ・2D の端、回帰)。
  - src\Engine\Engine\Scene\TagNames.{h,cpp}: ResolveOcclusionCulling を削除 (本番経路で未使用)。src\Engine\Engine\App\EngineCliSelfTest.cpp: 旧検査 (ファイル false / true / --no-occlusion / 書き戻さない) を ResolveCullingSettings(...).occlusion 経由に付け替え (検査の中身は同じ)。
仕様との差分:
  - [追加] cull_verify.bat から呼ぶ補助 tools\cull_counts.ps1 を別ファイルにした (sub-09 は「cull_verify.bat に同居させてよい」。JSON の比較を bat で書けないため)。呼び出しは pwsh (replay_verify.bat と同じ)。
  - [追加] --particle-demo を CPU / GPU の 2 バックエンド、--2d-demo を昼 / 夜、--render-demo を Forward / Deferred の 2 組ずつ回した (sub-09 は 6 シーン。経路の違う実装を片方だけ回すと壊れても緑になるため)。
  - [追加] ON/OFF の組に「既定ベンチの OFF で culled = 0、drawCalls は ON より多い」(受け入れ 3 の残り)、固定の検査に「固定無しのカットは culled が動く」を足した。
  - [追加] 通し検証 4 (基点との counts 比較) を MYE_CULL_BASE_ROOT で任意実行にした (基点の exe が要るため既定では回さない)。
  - [解釈] 受け入れ 3 の「3 構成」は Release --warp / Release 実 GPU / Debug 実 GPU とした。
  - [未実装] CI への組み込みは行わない (下記)。flow_title は golden を更新していない。
検証:
  - Release / Debug ビルド (MSBuild MyEngine.sln x64) → 成功 (ResolveOcclusionCulling 削除後)。replay_verify が再度両方をビルドして成功。
  - tools\cull_verify.bat (MYE_CULL_BASE_ROOT = 基点の Release、フル) → PASS (24 checks)、5 分 41 秒。ON/OFF の 9 組すべて diffPixels=0 (tol 0)。既定ベンチの OFF は culled = 0、drawCalls 23 → 25。固定: freeze+cut と freeze のみで culled/culledDistance/culledScreenSize/culledNonMesh 一致、固定無しのカットは culled が動く。counts: 5 シーン × 3 構成 (Release warp / Release GPU / Debug GPU) すべて 42 欄一致。基点 d52e91f3 の Release (別ディレクトリへ git archive → MSBuild Release) の既定ベンチの dump と、新欄 4 つを除く 34 欄が完全一致 (受け入れ 3)。
  - MYE_CULL_SKIP_CONFIGS=1 (WARP だけ、13 checks) → PASS、2 分 37 秒。
  - Editor.exe --selftest-full (Debug) → 121 suites, 1 failed: disturbancelong (「determinism: the final hash at tick 600 equals the baked value」)。基点 d52e91f3 の Release でも M99 後の Release でも同じ FAIL、同じ hash (F37D0323362CEF1D / 8AB83A2F89E9FC32) = M99 と無関係。他の 120 suites PASS。
  - tools\replay_verify.bat → PASS (17 scenes + snapshot round-trip + jobs A/B + compute A/B + time travel + rule check)、5 分 17 秒。
  - tools\shot_verify.bat → 32 枚中 31 枚 PASS、FAIL は flow_title のみ (diffPixels=944, maxDiff=62)。
  - flow_title の切り分け: 基点 d52e91f3 (git archive で別ディレクトリへ、eos-sdk は junction、Release ビルド) で同じ撮影条件 (Editor --flow-demo でシーンを生成 → Runtime --warp --font-embedded --width 960 --height 540 --frames 6 --shot-frame 3 --no-fxaa) を撮って基点の golden と比較 → 同じ FAIL (maxDiff=62 diffPixels=944 worst pixel (278, 469))。M99 と無関係。golden は更新していない。
  - Runtime.exe --screenshot を --frames 無しで使うと終了しない件: 基点 d52e91f3 の exe でも同じ (--render-bench-demo --screenshot out.png で 60 秒待っても終了せず、PNG は書かれる。自分で起動した PID を指定して終了させた)。EngineLoop.cpp は maxFrames > 0 でしかループを抜けない (--screenshot は撮影するだけ) = 既存の挙動で、M99 とは無関係。コードは変えていない。
  - tools\check_rules.ps1 (pwsh) → 0 error, 60 warning (既存の rule 17)。tools\check_rules_selftest.ps1 → PASS。
  - 未実行: Editor を開いての UI の目視 (Rendering > カリング、Project Settings、固定・枠、Scene View の GPU エミッタ / 2D の端、距離の青) — test_checklists の M99 の節に書いた。Release の --selftest-full (Debug だけ)。
CI の判断: cull_verify (WARP だけ、MYE_CULL_SKIP_CONFIGS=1) は 2 分 37 秒、既存の shot_verify (約 4 分) と同程度。CI に載せるなら ci.yml に shot_verify の次の 1 行で足りるが、master の CI は M98e 以降 failure が続いていて載せた結果を確認できないので、このサブでは載せていない (planner の判断を仰ぐ)。
自己採点 (1-5):
  仕様適合: 4 — 0〜3 を実測で満たす。CI 組み込みと UI の目視は未実施 (書き分け済み)
  正しさ: 4 — ON/OFF 全画素一致・固定・3 構成の counts・基点との counts を機械で確認。disturbancelong と flow_title は基点でも同じ FAIL で M99 起因でないことを確認
  コード品質: 4 — bat の補助関数が多めだが、判定は cull_counts.ps1 へ寄せた。bat 内のコメントは日本語
  テスト: 4 — 回帰スクリプトを新設し、空振りしないこと (固定なしのカットで値が動く、ベンチ OFF で drawCalls が増える) も検査に含めた。selftest の新規追加は ResolveOcclusionCulling の付け替えのみ
不安・質問:
  - selftest-full の disturbancelong が基点でも FAIL (baked hash が古い)。受け入れ条件 2 の「--selftest-full PASS」を満たせない原因。planner が別件として扱うか判断してほしい (M96 のスイート。台帳の申し送りへ)。
  - CI に cull_verify を載せるか (上記)。
  - 距離の青の見やすさはユーザーの目視 (test_checklists に項目あり)。
触ったファイル:
  - C:\HAL\MyEngin\tools\cull_verify.bat
  - C:\HAL\MyEngin\tools\cull_counts.ps1
  - C:\HAL\MyEngin\engine_spec.md
  - C:\HAL\MyEngin\docs\adr\ADR-038-culling.md
  - C:\HAL\MyEngin\docs\test_checklists.md
  - C:\HAL\MyEngin\src\Engine\Engine\Scene\TagNames.h
  - C:\HAL\MyEngin\src\Engine\Engine\Scene\TagNames.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\App\EngineCliSelfTest.cpp
  - C:\HAL\MyEngin\plans\m99-culling\sub-09.md (実装メモ)
申し送り:
  - 基点の検証用チェックアウト (C:\HAL\_m99_base) は作業後に削除した (自分で作った一時ディレクトリ。eos-sdk の junction を先に外したので本体は無傷)。基点との比較を再度やるなら git archive d52e91f3 で出して eos-sdk の Bin / Include / Lib を junction して Release をビルドする。
  - Release の --selftest-full と、cull_verify の Debug 実 GPU 部分は実 GPU のある機械でだけ回る (CI は MYE_CULL_SKIP_CONFIGS=1)。
  - nit (任意): sub-01 の nit 2 (scale ≠ 1 で保存済みシーンを読んだら警告) は未対応。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-11)。受け入れ 0 (cull_verify 24 checks PASS: ON/OFF 9 組 diffPixels 0、固定の一致と空振り防止、5 シーン × 3 構成の counts 機械一致)、1 (engine_spec §6.14 / §6.6 / 機能表 / §12.2 / 番号行 / ADR 一覧、ADR-038 確定、test_checklists を実文で抜き取り確認。`ResolveOcclusionCulling` 削除と selftest の付け替え)、2 (replay_verify PASS、check_rules 0 error、shot_verify は flow_title 以外 PASS、selftest-full は disturbancelong 以外 PASS。例外 2 件は基点でも同じ FAIL)、3 (基点 d52e91f3 と既定ベンチの counts 34 欄一致) を確認。[追加] 5 件と [解釈] は採用。CI 組み込みは見送り (spec 7.)。nit: Release の `--selftest-full` は未実行 (Debug で実施済み、Release は replay_verify / cull_verify で被覆)。
