# sub-02: カリング設定の土台と視錐台のスイッチ

- 依存: sub-01
- 状態: OK (コミットは司会)
- 往復: 1

## やること
spec 4.1.1 / 4.1.2 / 4.1.7 の「理由別の数」の欄 / 4.3。受け入れ条件 1, 2, 3, 12 (一部)。
- 設定モジュール (例 `src\Engine\Engine\Rendering\CullingSettings.{h,cpp}`): spec 4.1.1 の 7 キーを読む・書く (1 キーずつ差し替える read-modify-write。読めないファイルは上書きしない)。実効値を CLI と合成する関数を 1 つ置き、EngineLoop と selftest が同じ関数を使う。既存の `Load/Save/ResolveOcclusionCulling` (`TagNames.cpp:200-226`) は呼び出し元と既存テストを壊さずに新モジュールへ寄せるか、そのまま残して新モジュールから呼ぶ (どちらにしたか実装メモに書く)。**距離・画面サイズのキーもこのサブで読み書きまで作る** (判定は sub-06 / 07)。
- CLI: `--no-frustum-culling`、`--no-distance-culling`、`--no-size-culling`、`--cull-max-distance D` (D ≥ 0)、`--cull-min-screen-size F` (0 < F ≤ 1)。後ろ 4 つはこのサブでは値を RenderSystem に渡すだけ。
- RenderSystem: `enableFrustumCulling` (と距離・画面サイズの設定を入れる場所) を足す。`f.cullEnabled` を「カメラがある」と「視錐台で落とす」に分ける。OFF のときの規則は spec 4.1.2 (CSM フィットは視錐台の内側だけ、`prevRender` の記録は ON と同じ規則、LOD / URO / 地形 / ライト選別は不変)。CPU パーティクルのプール (`CpuParticleBackend.cpp:739`) とタイルマップのチャンク (`Sprite2DRenderer.cpp:1089-1101`) もこのスイッチに従わせる。地形は従わせない。
- `prof::RenderStats` に `culledDistance` / `culledScreenSize` / `culledNonMesh` / `unculledGpuEmitters` の欄を**先に**足し (このサブでは 0 のまま)、`RenderStatsDump` の `counts`・`HasActivity`・`RenderStatsSelfTest`・ProfilerWindow の表まで形を固定する (M90a の「欄と dump の形を先に固定」と同じ)。`culled` の意味は変えない。
- UI: Project Settings > Rendering の「カリング」区切りに 4 つのチェックと数値欄 2 つ (閾値・既定距離)、タグ別の距離の表 (このサブで表の編集と保存まで。効くのは sub-06)。Rendering メニューに「カリング」サブメニューを作り、既存のオクルージョンの項目を移し、視錐台・距離・画面サイズのスイッチを足す。切り替えた瞬間に RenderSystem へ反映しファイルへ書く (`scmhint::Changed` も既存と同じく呼ぶ)。数値は編集を終えたときに書く。
- ローカライズ: 新しい文字列を `LocalizationTable.inl` に英日。

## やらないこと (このサブでは)
- 距離・画面サイズの判定 (sub-06 / 07)、局所影 (sub-03)、新しい経路の判定 (sub-04 / 05)
- 地形のカリングの変更

## 触る場所 (planner の見立て)
- `src\Engine\Engine\Scene\TagNames.{h,cpp}` (既存のオクルージョン設定)、新規 `CullingSettings`
- `src\Engine\Engine\Loop\EngineLoop.cpp:473-476`、`src\Engine\Engine\App\EngineCli.cpp` / `EngineConfig` 相当 / `EngineCliSelfTest.cpp:261-283`
- `src\Engine\Engine\Rendering\RenderSystem.{h,cpp}` — `CollectLights` (:1010-1020)、`CollectDrawables` ステージ 2 (:1476-1504) とステージ 3 (:1508-1628)
- `src\Engine\Engine\Particles\CpuParticleBackend.cpp:720-740`、`src\Engine\Engine\Rendering\Sprite2D\Sprite2DRenderer.cpp:1089-1101`
- `src\Engine\Core\Diagnostics\Profiler.h:34-55`、`src\Engine\Engine\Rendering\RenderStatsDump.cpp`、`RenderStatsSelfTest.cpp`、`src\Editor\Windows\Debug\ProfilerWindow.cpp`
- `src\Editor\Windows\Project\ProjectSettingsWindow.cpp` (Rendering 節 :59-98)、`src\Editor\App\EditorApp.cpp:1268-1282`
- `src\Engine\Core\Localization\LocalizationTable.inl`
- ファイル追加後は `tools\gen_project_files.ps1`

## 受け入れ条件 (このサブ)
1. 設定の往復 (spec 5-1): 7 キーの保存・読み戻し、キー無し / 型違い / 範囲外 → 既定値、読めないファイルを上書きしない、`rayTracingTags` と `rendering` 内の他キーが残る
2. CLI (spec 5-2 のうち `--no-frustum-culling` / `--no-distance-culling` / `--no-size-culling` / `--cull-max-distance` / `--cull-min-screen-size`): 解析・不正値の拒否・実効値の式・ファイル不変
3. 視錐台 OFF (spec 5-3): 既定ベンチで ON / OFF の撮影が全画素一致 (tol 0)、OFF で `drawCalls` 増・`culled` = 0、影の数 (`shadowCascadeCasters` など) が ON と同じ。可能なら selftest (OcclusionSelfTest の ON/OFF 全画素一致の作り) にする
4. 既定設定で、既定ベンチの `counts` (新しい欄を除く) が sub-01 後と一致
5. `counts` が Debug / Release / `--warp` で一致 (新しい欄を含む)
6. ローカライズ規則と check_rules が 0 error

## 検証コマンド
- Debug / Release ビルド、`tools\gen_project_files.ps1` (ファイル追加時)
- `MYE_SELFTEST_ONLY=Tag,EngineCli,RenderStats,Occlusion,<新スイート>` で `Editor.exe --selftest`
- `Editor.exe --render-bench-demo --render-stats-dump` (ON / `--no-frustum-culling`)、`--screenshot` の ON/OFF を img-diff (memory: img-diff は Editor.exe)
- `tools\check_rules.ps1`

## 実装メモ (coder が追記)

SELF_EVAL: sub-02 (round 1)
実装:
  - src\Engine\Engine\Rendering\CullingSettings.{h,cpp} (新規): `CullingSettings` (7 キー) / `CullingCliOverrides` / `LoadCullingSettings` / `SaveCullingFlag` / `SaveMinScreenSize` / `SaveDefaultMaxDrawDistance` / `SaveTagCullDistances` (1 キーずつ read-modify-write、読めないファイルは上書きしない) / `ApplyCullingCli` / `ResolveCullingSettings` (EngineLoop と selftest が共有)。既存の `Load/Save/ResolveOcclusionCulling` (TagNames.cpp) は**そのまま残し**、新モジュールのオクルージョンの読み書きがそれを呼ぶ (既存テスト無変更)。float は最短桁の十進で書く (0.005 が 0.004999999888 にならない)。
  - EngineLoop.{h,cpp}: `EngineConfig::cullingCli` を追加。起動時に `ResolveCullingSettings` → `RenderSystem::ApplyCullingSettings`。occlusion の CLI は従来の `config.occlusionCulling` を正本のまま合成。
  - EngineCli.cpp: `--no-frustum-culling` / `--no-distance-culling` / `--no-size-culling` / `--cull-max-distance D` (D >= 0) / `--cull-min-screen-size F` (0 < F <= 1)。不正値は stderr + 解析エラー (黙って捨てない。`--render-bench-scale` と同じ流儀)。
  - RenderSystem.{h,cpp}: `enableFrustumCulling` / `enableDistanceCulling` / `enableSizeCulling` / `cullMinScreenSize` / `cullDefaultMaxDrawDistance` / `cullTagDistance[64]` と `ApplyCullingSettings`。`FrameContext::frustumDrop` (= カメラあり && enableFrustumCulling) を足し、`cullEnabled` は「カメラがある」のまま (LOD・URO・地形・ライト選別は不変)。`CollectDrawables` ステージ 3: 視錐台の外の物は OFF のとき落とさずキューへ入れるが、(a) 影の候補は ON と同じ画面外キャスターの経路 (offscreenCasters_) だけに入れ queue 経由では入れない、(b) CSM のフィット AABB に入れない、(c) `prevRender` の Lookup / Record をしない。`culled` は OFF で 0。
  - RenderTypes.h `RenderView::frustumCulling` (末尾追加、既定 1) → CpuParticleBackend.cpp のプール判定を OFF で飛ばす。Sprite2DRenderer.{h,cpp} `SetFrustumCulling` → タイルマップのチャンク判定を OFF で飛ばす。地形は従わせない。
  - Profiler.{h,cpp}: `RenderStats` に `culledDistance` / `culledScreenSize` / `culledNonMesh` / `unculledGpuEmitters` (今は 0 のまま、`culled` の意味は不変)。RenderStatsDump.cpp: `counts` 出力と `HasActivity`。RenderStatsSelfTest.cpp: Fingerprint・加算・JSON の検査。ProfilerWindow.cpp: 「理由別」の 1 行 (どれかが 0 でないときだけ)。
  - EditorApp.cpp: Rendering > カリング サブメニュー (視錐台・オクルージョン (既存項目を移動)・距離・画面サイズ。切替の瞬間に RenderSystem へ反映 + 保存 + `scmhint::Changed`)。ProjectSettingsWindow.cpp: Rendering 節に「カリング」区切り (チェック 4、最小画面サイズのログスライダー、既定距離、タグ別距離の表、注記)。数値は `IsItemDeactivatedAfterEdit` で保存、編集中も RenderSystem へは即反映。
  - LocalizationTable.inl: 12 個の文字列 (英日)。
  - TagSelfTest.cpp: CullingSettings の 15 検査。EngineCliSelfTest.cpp: CLI 解析と実効値の 20 検査強。
  - build\*.vcxproj(.filters): `tools\gen_project_files.ps1` の結果 (Engine.vcxproj / .filters が変更、ファイル追加のため。tracked なのでステージ対象)。
仕様との差分:
  - [追加] `CullingSettings.h` に `kCullingTagCount = 64` を置いた (Components.h の kMaxTags と static_assert で照合)。EngineLoop.h が CullingCliOverrides のために取り込むので、Components.h を持ち込まないため。
  - [追加] `RenderView` に `frustumCulling` を足した (spec の「触る場所」に無い。CPU パーティクルのバックエンドが RenderView しか受け取らないため。ABI・共有構造ではない)。
  - [追加] `--no-size-culling` と `--cull-min-screen-size` を併記したら OFF が勝つ (spec は「F > 0 で ON」だけで併記時の規則が無かった)。最も小さい解釈 (CLI の OFF は常に勝つ、既存の occlusion と同じ考え方)。
  - [未実装] 視錐台 ON/OFF 全画素一致の selftest 化 (OcclusionSelfTest は RenderSystem ではなくパスを直接駆動する作りで、RenderSystem::Render を回す selftest の先例が無い)。代わりに CLI + img-diff で確認した (下記)。
  - [逸脱なし・注意] OFF 時、視錐台の外の物は queue_ に入る。そのため局所影アトラス (queue_ を描く) には OFF のとき画面外キャスターが入る = D4 の裏返し。既定ベンチには局所影のライトが無い (atlas tiles = 0) ので全画素一致を崩さなかった。局所影のあるシーンでの一致は sub-03 の仕事。
検証:
  - Debug / Release ビルド (MyEngine.sln) → 成功 (エラー 0)。`tools\gen_project_files.ps1` 実行済み。
  - MYE_SELFTEST_ONLY=tag,enginecli,renderstats,occlusion,particle,tilemap で `Editor.exe --selftest` (Debug) → 6 suites 0 failed (CullingSettings 15 / CLI 追加分 / RenderStats の新欄 全 PASS)。
  - `tools\check_rules.ps1` → 0 error (60 warning は既存の rule 7 / 17 のみ)。
  - 受け入れ 3: `Runtime.exe --render-bench-demo --screenshot --shot-frame 60` (Debug、ON と `--no-frustum-culling`) → img-diff `--tol 0` PASS (diffPixels=0)。dump: ON は drawCalls 23 / culled 308、OFF は drawCalls 25 / culled 0、shadowCascadeCasters は両方 [6, 43, 833]、shadowDrawCalls 44 で同一。Editor.exe の撮影は ImGui 込み (Profiler の文字が写る) ので比較に使えない → Runtime で撮影。
  - 追加で `--particle-demo` / `--2d-demo` / `--render-demo` を Release Runtime で ON / OFF 撮影 → いずれも img-diff tol 0 PASS (CPU パーティクル・タイルを含む)。
  - 受け入れ 4: sub-01 後のコミット (877dc523) を `git archive` → 別ディレクトリで Release Runtime をビルドして dump を取り、新 exe (既定設定) の `counts` から新 4 欄を除いて JSON 比較 → 完全一致。
  - 受け入れ 5: `counts` (新欄を含む) を Debug (GPU) / Release (GPU) / Release `--warp` / Release `--no-jobs` で比較 → 全て一致。OFF も Debug と Release で一致。
  - Profiler 窓を撮影して「culled by reason: frustum 274 / distance 0 / ...」の行が出ることを確認 (Editor.exe の撮影)。
  - 未確認: Rendering メニューと Project Settings のカリング欄の見た目・操作 (ビルドは通るが画面では確認していない)。`--selftest-full`・replay_verify・shot_verify は未実行 (このサブの検証コマンド外。sub-09 で実施予定)。
自己採点 (1-5):
  仕様適合: 4 — 項目は一通り実装。selftest 化 (3) は「可能なら」で未実装、UI の見た目は未確認。
  正しさ: 4 — ON/OFF 全画素一致を 4 つのシーンで、counts を 4 構成 + 着手前の exe と照合。局所影ありのシーンでの一致は未検証 (sub-03 の領域)。
  コード品質: 4 — 既存の流儀 (read-modify-write、CLI 表、ビュー別履歴) に沿わせた。RenderView / Sprite2DRenderer に 1 つずつ旗を足したのは最小。
  テスト: 4 — 設定・CLI・統計の selftest を追加。描画 ON/OFF の一致は CLI 撮影の手作業で、回帰テストとしては残っていない。
不安・質問:
  - 視錐台 ON/OFF 一致の回帰テストが無い。RenderSystem::Render を回す selftest を足すか (大きな作り)、sub-09 の通しで shot_verify 的な ON/OFF 撮影を手順に入れるか、planner に判断してほしい。
  - OFF で局所影アトラスに画面外キャスターが入る件 (上記)。sub-03 が ON 側を直せば揃う前提で、sub-02 ではアトラスを触っていない。
触ったファイル:
  src\Engine\Engine\Rendering\CullingSettings.h, src\Engine\Engine\Rendering\CullingSettings.cpp (新規),
  src\Engine\Engine\Rendering\RenderSystem.h, RenderSystem.cpp, RenderStatsDump.cpp, RenderStatsSelfTest.cpp,
  src\Engine\Engine\Rendering\Sprite2D\Sprite2DRenderer.h, Sprite2DRenderer.cpp,
  src\Engine\Engine\Particles\CpuParticleBackend.cpp, src\Engine\Renderer\Pipeline\RenderTypes.h,
  src\Engine\Core\Diagnostics\Profiler.h, Profiler.cpp, src\Engine\Core\Localization\LocalizationTable.inl,
  src\Engine\Engine\Loop\EngineLoop.h, EngineLoop.cpp, src\Engine\Engine\App\EngineCli.cpp, EngineCliSelfTest.cpp,
  src\Engine\Engine\Scene\TagSelfTest.cpp,
  src\Editor\App\EditorApp.cpp, src\Editor\Windows\Debug\ProfilerWindow.cpp, src\Editor\Windows\Project\ProjectSettingsWindow.cpp,
  build\Engine.vcxproj, build\Engine.vcxproj.filters (gen_project_files の結果。tracked)
申し送り:
  - sub-03: OFF のとき視錐台の外の物は queue_.opaque に入る。`ShadowAtlas::Render` は queue_ を描くので、局所影のあるシーンでは OFF だけ画面外キャスターが入る。sub-03 で「画面外キャスターをタイルの判定に足す」とき、ON 側が同じ集合を持てば OFF と一致する (OFF の queue_ には既に画面外の物が入っているので、二重に足さないこと。ON/OFF で同じ集合になるのは「画面内 + offscreenCasters_」)。
  - `cullTagDistance` / `cullDefaultMaxDrawDistance` / `enableDistanceCulling` / `enableSizeCulling` / `cullMinScreenSize` は RenderSystem に値を持つだけ (判定は sub-06 / 07)。
  - `ResolveOcclusionCulling` は EngineLoop からは呼ばれなくなった (EngineCliSelfTest だけが使う)。sub-09 で整理するか判断してほしい。
  - Editor.exe の `--screenshot` は ImGui 込み。シーンだけの比較は Runtime.exe で撮ること (img-diff は Editor.exe)。Editor.exe は `--frames` 無しだと撮影後も終了しない。
  - 計測や撮影の前に `cache\render_bench.scene.json` を消すこと (今回も消してある)。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-11)。受け入れ条件 1〜6 は SELF_EVAL の検証 (TagSelfTest / EngineCliSelfTest、既定ベンチ他 3 シーンの ON/OFF 撮影 img-diff tol 0、着手前 877dc523 との counts 比較、4 構成の counts 一致、check_rules 0 error) で確認。EngineLoop と EngineCliSelfTest が同じ `ResolveCullingSettings` を通ることも実コードで確認。差分 [追加] 3 件と [未実装] (selftest 化) は容認。ON/OFF の回帰は sub-09 の `tools\cull_verify.bat` で担保する (spec 変更)。nit: `cli.occlusion = config.occlusionCulling` の組み立てが EngineLoop と selftest の 2 か所にある (任意で 1 関数へ)。UI の見た目はユーザーの目視。
