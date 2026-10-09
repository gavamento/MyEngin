# sub-02: GPU オクルージョンの縦切り (max-Z HZB を viewKey ごと、2 フェーズ、Deferred の不透明)

- 依存: sub-01
- 状態: OK (commit は司会が記入)
- 往復: 1

## やること
spec §4.1.4、§2 #1 #2 #3。M90 で最もリスクの高い未知 (2 フェーズが既存の Deferred 経路と WARP で組めるか) を潰す縦切り。**Deferred の GBuffer 不透明だけ**に通す。
1. HZB の縮小を min / max で切り替えられるようにする。SSR 用の min-Z ピラミッドの結果は 1 ビットも変えない (既存の selftest がそのまま通ること)。max 版の selftest を足す (同じ `HzbReduceSpan` の分割規則、奇数段の 3 テクセル読みでも max が正しい)。
2. viewKey ごとのオクルージョン状態 (max-Z ピラミッド、エンティティごとの可視ビット = 安定スロット) を持つ。viewKey 0 と ProbeBaker は持たない (OFF)。リサイズ・デバイス消失 (M88 の復旧経路) で捨てる。
3. 2 フェーズ:
   - フェーズ 1: 前フレームに可視だった項目だけを GBuffer に描く。
   - その深度から max-Z HZB を作る。
   - compute: 全項目 (CPU 視錐台を通った物) の AABB をビューへ投影し、HZB の該当 mip で判定。フェーズ 1 で描いていない可視の物を run ごとに詰め、indirect 引数を書く。可視ビットを更新。
   - フェーズ 2: `DrawIndexedInstancedIndirect` で描く。インスタンシングできない項目は instanceCount 0/1 の個別 indirect。
4. AABB の判定は保守的: 投影がニア面をまたぐ物、画面の外へはみ出す物は可視。判定の CPU 版 (同じ式) を純関数で持ち、selftest で境界値を確かめる (シェーダと定数・式を一致させる。C++/HLSL の共有定数は規約 4.3)。
5. `RenderSystem::enableOcclusionCulling` (既定 true)、CLI `--no-occlusion`、エディタの描画設定メニューに ON/OFF (`Tr()`)。
6. 統計: フェーズ 1 / 2 の描画数、落とした数を sub-01 の欄へ (数フレーム遅れのステージング読み、統計専用)。GPU ms の欄も埋める。
7. 失敗の局所化: リソース作成失敗でオクルージョンだけ OFF + ログ 1 回。
8. (sub-01 VERDICT から移管) `render_bench` にカメラカットを足す。**sim には触れない**: 描画側のカメラ上書き (`RenderSystem::Render` の `CameraOverride`) を、撮影フレーム番号で切り替えるデバッグ用 CLI (例 `--render-bench-cut-frame N`。名前は coder が既存 CLI に合わせる) で入れる。カット後のカメラは、カット前に壁で隠れていた物が見える位置にする。`cache\render_bench.scene.json` は生成シーンのキャッシュなので、シーン生成を変えたら消してから撮る。
9. (sub-01 VERDICT から) ProfilerWindow の GPU 行の「フレーム」は `RenderSystem::Render` 1 回分 (複数ビューのエディタでは最後に描いたビュー) を計っている。表示を実態に合わせる (例: 「Render (最後のビュー)」)。ビュー別の ms は任意。

## やらないこと (このサブでは)
- Forward 経路、`--hzb-debug` の拡張 (sub-03)。影・半透明。
- サーフェスマテリアル・水面など別経路の項目を判定対象にすること (常に描く)。

## 触る場所 (planner の見立て)
- `src\Engine\Renderer\Passes\HzbPass.*`、`assets\shaders\hzb_reduce.cs.hlsl`
- 新しい compute シェーダ (判定 + 詰め込み)、新しいパス (`Renderer\Passes\OcclusionCullPass.*` 等。名前は coder が既存の命名に合わせる)
- `src\Engine\Renderer\Pipeline\DeferredPath.cpp` (GBuffer の描画を 2 回に分ける。DeferredPath.cpp:1421 の「深度プリパスを足さない」方針とは矛盾しない — フェーズ 1 は本描画そのもの)
- `src\Engine\Renderer\Mesh\MeshInstancing.h` (`BuildInstanceRuns`、t0 の StructuredBuffer。可視 index の間接参照を VS に足す必要があるか)
- `src\Engine\Engine\Rendering\RenderSystem.*` (viewKey 別の状態、prevRender と同じ流儀の安定スロット)
- 前例: `GpuParticleBackend.cpp` の Indirect
- 確認すべき既存の前提: velocity (prevWorld)、TAA のジッタ、ステンシルを使う経路、GBuffer のクリア位置

## 受け入れ条件 (このサブ)
1. `render_bench` (Deferred) で dump の occluded > 0、ON/OFF のスクショの画素差 0 (差があれば箇所と原因を示す。z-fight 以外は不具合)。 — `--screenshot` A/B + img-diff (Editor.exe)
2. カメラカットのフレームで欠けない (カット直後のフレームを ON/OFF で撮って差 0)。 — 同上
3. 既存の HZB (SSR) の selftest と golden が全部 PASS。max 縮小と保守的判定の selftest を足して PASS。 — `--selftest` Debug / Release
4. `--warp` で動き、WARP での `render_bench` 1 フレームの所要時間を ON/OFF で報告 (CI 時間のリスク確認。ゲートではない)。
5. リソース作成失敗の注入で、オクルージョンだけが OFF になり描画が続く。 — selftest
6. `tools\check_rules.ps1` PASS。

## 検証コマンド
- ビルド Debug / Release、`--selftest` 両構成、`tools\check_rules.ps1`
- `render_bench` の `--screenshot` を `--no-occlusion` と比較、`--render-stats-dump`
- 実機 GPU での ms を参考値として報告

## 実装メモ (coder が追記)

### SELF_EVAL: sub-02 (round 1)

実装:
- `HzbPass.h/.cpp`: `HzbReduceOp {Min, Max}` を足し `Init(device, shaders, op = Min)`。max 版は `hzb_reduce_max.cs.hlsl` (= `MYE_HZB_MAX` を立てて `hzb_reduce.cs.hlsl` を include)。`Texture()` を selftest 用に公開。min 版 (SSR) の既定経路は式もディスパッチも不変。
- `OcclusionMath.h` (新規): AABB 8 頂点の投影、保守的判定、深度の甘さ (24bit の 32 刻み)、矩形の余白 1px の純関数。HLSL と同じ式 (CPU 鏡)。
- `occlusion_cull.cs.hlsl` (新規): 1 グループ = 1 描画コマンド。項目を走査し、グループ内 prefix sum で元の並びのまま詰め、`DrawIndexedInstancedIndirect` 引数と remap (可視インスタンスの添字) を書く。モード SELECT (フェーズ 1) / TEST (フェーズ 2) / PASS (HZB が作れないとき全部可視)。可視ビットと統計は同じ CS が更新。
- `OcclusionCullPass.h/.cpp` (新規 `OcclusionCuller`): viewKey 別の max-Z ピラミッド・可視ビット (`entity.index` が安定スロット、世代 + 描画通番で「前フレームも描いたか」を判定、リサイズ・通番の飛びで履歴を捨てる)・統計ステージングリング (2 フレーム遅れ)。作業バッファ (項目 / コマンド / 引数 / remap) はビュー間で共有。リソース作成失敗・CS コンパイル失敗・ピラミッド失敗は 1 回ログして全ビュー OFF (`Shutdown` → `Init` で復帰 = M88 の復旧経路で捨てる)。
- `DeferredPath.cpp: RenderGeometry`: GBuffer の描画単位を `gbCmds_` (run か単発) に畳む計画段を切り出し、従来の描画 (`drawGbufferCmds(-1)`) と 2 フェーズ (0 / 1) が同じ列を使う。2 フェーズ = `SelectPhase1` → フェーズ 1 → 地形 → RTV/DSV と VS の SRV を外して `TestPhase2` (max-Z 構築 + 判定) → RT を張り直してフェーズ 2。地形が b4 と IA/VS/PS を替えるので、フェーズ 2 の頭で velocity CB (b4) とシェーダ・VB/IB の記憶を張り直す。`Render` で 2 フレーム遅れの GPU 統計を `prof::AddRenderStats` へ。`OcclusionGpuMs()`。
- `deferred_gbuffer_instanced.hlsl` + `MeshBind.h: PerObjectCB`: `remapPlus1` (0 = 従来の `gInstanceBase + SV_InstanceID`)。VS の t2 に remap。`instPad[2]` を `remapPlus1` + `instPad` に置換 (サイズ不変)。
- `RenderTypes.h`: `RenderItem` に `worldAabbMin/Max/hasWorldAabb`、`RenderView::occlusionEnabled`。`RenderSystem.cpp`: 不透明の非スキン項目へ world AABB (余白込み、CSM フィットと同じ値) を載せる。viewKey 1〜3 かつ `enableOcclusionCulling` で `view.occlusionEnabled=1`。`OcclusionGpuMs` を写す。`RenderSystem.h`: `enableOcclusionCulling` (既定 true)。
- CLI / 設定: `--no-occlusion`、`--render-bench-cut-frame N` (`EngineCli.cpp` / `EngineLoop.h` / `EngineLoop.cpp`)。カットは描画側 `CameraOverride` を指定フレーム以降に渡すだけで sim には触れない (壁 z=15 の裏のグリッドを (45,8,35) から (0,2,70) へ見る位置)。
- UI: エディタ「レンダリング」メニューに `Menu_Occlusion`、ProfilerWindow に `Prof_Occlusion` 行 (判定 + ピラミッドの ms と 2 フレーム遅れの件数)。`Prof_GpuStages` の「フレーム」を「Render (最後のビュー)」に改名 (sub-01 申し送り 9)。`RenderStatsDump` の `gpuMs` に `occlusion` を追加。`LocalizationTable.inl` は両言語。
- `tools/check_rules.ps1`: 規則 9 に `kThreadGroupSize/MYE_OCC_TG`、`kDepthBiasSteps/MYE_OCC_BIAS_STEPS`、`kRectMarginPx/MYE_OCC_MARGIN_PX` を追加。
- `OcclusionSelfTest.h/.cpp` (新規、`EditorMain.cpp` に登録): CPU = max ピラミッドの取りこぼしゼロ (奇数辺 15x7 等) / 2 のべきで 2x2 厳密 / AABB 判定の境界 (壁の裏・手前・輪郭外・縁またぎ・ニア面またぎ・背後・画面はみ出し・同深度の厚み無し箱・深度の甘さ ±)。GPU (WARP) = max と min の全段が CPU 参照とビット一致 (37x23 / 64x48 / 15x7 / 129x3)、Deferred の ON/OFF 全画素一致 (履歴なし → 定常 5 フレーム → カメラカット直後 → カット後 → 戻り → 通番が飛んだ直後)、失敗注入で OFF + 描画継続 + 再試行しない、GPU 統計 (壁の裏 3 個が occluded、定常のフェーズ 1 = 壁 1 個)。`EngineCliSelfTest` に 2 項目。

仕様との差分:
- [解釈] 「画面の外へはみ出す物は可視」は文字どおり実装 (余白込みの矩形が画面からはみ出せば丸ごと可視。切り詰めて判定はしない)。床など大きな物は常に描かれる。保守側の最小解釈で、切り詰め判定へは後から緩められる。
- [追加] スキン項目 (AABB が姿勢を包む保証が無い) は判定せずフェーズ 1 で常に描く。sub-04 で保守的 AABB が付いたら対象にできる。render_bench の 7 体は常に描かれる (統計の phase1 に含まれる)。
- [追加] 詰め込みは prefix sum (順序保存)。アトミック詰めだと同深度で重なるインスタンスの順序が実行ごとに揺れるため。仕様は「run ごとに詰め」のみで手段は未指定。
- [追加] `drawCalls` / `triangles` は従来どおり CPU が提出した論理数 (フェーズ 1 / 2 で 2 重に数えない、GPU が間引いた分も引かない)。ON/OFF と Debug/Release/WARP で同じ値になり基準値 (16 / 1322628) が動かない。間引いた効果は `occlusionPhase1Draws / occlusionPhase2Draws / occluded` (GPU カウント、2 フレーム遅れ) で見る。
- [追加] 地形はフェーズ 1 の後・HZB 構築の前に描く (遮蔽物にするため)。
- [追加] `hzb_reduce_max.cs.hlsl`、`check_rules.ps1` の規則 3 件、`RenderStatsDump` の `gpuMs.occlusion`、`RenderPath.h` の `OcclusionGpuMs()`、`DeferredPath` のテスト用口 (`InjectOcclusionFailureForTest` / `OcclusionDisabled` / `OcclusionStatsForTest`)。
- [追加] HZB が作れなかったフレームは判定せず全項目を可視として描き切り、次フレームから全ビュー OFF (ログ 1 回)。
- 一時的な実装方針: 可視ビットのスロットは `entity.index` (`PrevRenderWorldStore` と同じ流儀)。1 エンティティ 1 項目が前提 (`RenderSystem` は 1 エンティティから 1 項目しか作らない)。無効エンティティ・巨大な index は履歴を持たず常に描く。
- Unity / UE の方式照合 (spec §7): 公式ドキュメントは取得できなかった (Unity は該当ページの URL が 404 / 本文に該当語が無い、Epic のページは JS 描画で本文が取れない)。未確認のまま (記憶による 2 パスの前提)。

検証:
- Debug / Release ビルド → 警告 0 / エラー 0。
- `Editor.exe --selftest` Release → exit 0 (71 秒)。Debug → 1 回目は ServerNetSelfTest が 2 件 FAIL (`V1 LoadPersist / LoadGame in a session` の offline replay 一致と tick 280 の dump 一致。verified 0 of 540)、sub-01 申し送りの一過性と同種。修正を入れた後の再実行 (Debug exit 0 / 403 秒) は全 PASS。原因は未断定 (オクルージョンとは無関係の経路)。新規 `GPU occlusion culling self test` は Debug / Release ともに PASS。
- `tools\check_rules.ps1` → 0 error / 50 warning (全て既存の rule 7 の警告)。
- `tools\shot_verify.bat` (Release、WARP、オクルージョン既定 ON) → 30 枚全部 PASS。
- render_bench (Runtime.exe --render-bench-demo --deferred --no-audio --font-embedded --width 960 --height 540 --shot-frame 30) の ON と `--no-occlusion` の `--screenshot` を `Editor.exe --img-diff` で比較 (tol 0): Debug 実 GPU / Release 実 GPU / Debug `--warp` の 3 構成とも diffPixels=0 (maxDiff=0)。
- dump (ON): `occlusionPhase1Draws 589 / occlusionPhase2Draws 0 / occluded 2782`、`counts.total` は drawCalls 16 / triangles 1322628 / culled 307 / shadowDrawCalls 10092 で OFF と同じ。counts 節は Debug / Release / WARP で ON も OFF もバイト一致 (gpuMs 節を除いて比較)。
- カメラカット (`--render-bench-cut-frame 40`): `--shot-frame 40` (カット直後) と 41 の ON/OFF が diffPixels=0。`--shot-frame 42` の dump (= カットのフレームの統計) は `occlusionPhase1Draws 311 / occlusionPhase2Draws 2207`。壁の裏に隠れていた物がフェーズ 2 で描かれている。カット画像はカット前と別の絵 (カットが効いている)。
- WARP (Release、`--warp`、960x540、30 フレームのプロセス全体): ON 10.0 / 10.4 秒、OFF 10.7 / 10.8 秒。GPU 時間 (dump の gpuMs、WARP): frame 129.7 ms (ON) / 145.2 ms (OFF)、gbuffer 20.1 / 48.1 ms、occlusion 3.1 ms。CI 時間のリスクは見えない。
- 実機 GPU (参考値、Release、dump の gpuMs): gbuffer 1.78 ms (ON、うち occlusion 0.13 ms) / 2.01 ms (OFF)。このシーンは影 (csm) が支配的で、GBuffer は元々軽い (実機の GpuTimer は揺れが大きい)。
- 未実施: `tools\replay_verify.bat` (sim・snapshot・ABI に触れていない)。Forward 経路 (sub-03)。ProfilerWindow / エディタのメニュー・`--hzb-debug` の目視 (スクリーンショット未取得)。

自己採点 (1-5):
  仕様適合: 4 — 受け入れ 1〜6 を満たし検証済み。画面はみ出しを丸ごと可視にする最小解釈と、スキンを常に描く点は差分欄に明記。Unity / UE の一次資料は未確認。
  正しさ: 4 — render_bench の ON/OFF が 3 構成で全画素一致、カット直後も一致、selftest の ON/OFF 比較 (履歴なし・定常・カット・通番の飛び) と GPU ピラミッドのビット一致、golden 30 枚 PASS。実機でのスキン + 間接描画は bench の 7 体で一致を確認したが、ビュー複数 (SceneView + GameView) のエディタ実機は未確認。
  コード品質: 4 — 計画段の切り出しで従来経路とフェーズの重複を避け、CPU 鏡 + 規則 9 で定数を機械照合。`RenderGeometry` が長い (ラムダ + 計画段で増えた)。
  テスト: 4 — 純関数 (境界値)・GPU ビット一致・Deferred A/B・失敗注入・統計を足した。HLSL と CPU 鏡の式一致は GPU 側の A/B で間接的に確認 (GPU 判定の直接の単体比較は無い)。

不安・質問:
- フェーズ 2 は run / 単発ごとに状態設定と CB 更新をもう 1 回発行する (間引く側を CPU が知れないため)。ユニークなメッシュが多いシーンでは CPU 提出コストが約 2 倍になる。bench (16 コマンド) では見えない。sub-06 の描画側並列化 / コマンド数の多いシーンの計測で判断してほしい (現状は仕様どおり)。
- `drawCalls` / `triangles` を論理数のままにした (差分欄)。ProfilerWindow の「ドローコール」は間引き効果を表さない。間引いた実数を主表示にするなら sub-03 以降で統計の意味を決め直す必要がある。
- ServerNetSelfTest の一過性 FAIL が 2 回目 (今回の初回 Debug)。原因未断定のまま再発している。

触ったファイル:
- assets/shaders/deferred_gbuffer_instanced.hlsl, assets/shaders/hzb_reduce.cs.hlsl, assets/shaders/hzb_reduce_max.cs.hlsl (新規), assets/shaders/hzb_reduce_max.cs.hlsl.meta (新規), assets/shaders/occlusion_cull.cs.hlsl (新規), assets/shaders/occlusion_cull.cs.hlsl.meta (新規)
- src/Editor/App/EditorApp.cpp, src/Editor/App/EditorMain.cpp, src/Editor/Windows/Debug/ProfilerWindow.cpp
- src/Engine/Core/Localization/LocalizationTable.inl
- src/Engine/Engine/App/EngineCli.cpp, src/Engine/Engine/App/EngineCliSelfTest.cpp
- src/Engine/Engine/Loop/EngineLoop.cpp, src/Engine/Engine/Loop/EngineLoop.h
- src/Engine/Engine/Rendering/RenderStatsDump.cpp, src/Engine/Engine/Rendering/RenderStatsDump.h, src/Engine/Engine/Rendering/RenderSystem.cpp, src/Engine/Engine/Rendering/RenderSystem.h
- src/Engine/Renderer/Mesh/MeshBind.h
- src/Engine/Renderer/Passes/HzbPass.cpp, src/Engine/Renderer/Passes/HzbPass.h
- src/Engine/Renderer/Passes/OcclusionCullPass.cpp (新規), src/Engine/Renderer/Passes/OcclusionCullPass.h (新規), src/Engine/Renderer/Passes/OcclusionMath.h (新規), src/Engine/Renderer/Passes/OcclusionSelfTest.cpp (新規), src/Engine/Renderer/Passes/OcclusionSelfTest.h (新規)
- src/Engine/Renderer/Pipeline/DeferredPath.cpp, src/Engine/Renderer/Pipeline/DeferredPath.h, src/Engine/Renderer/Pipeline/RenderPath.h, src/Engine/Renderer/Pipeline/RenderTypes.h
- tools/check_rules.ps1
- plans/m90-render-optimization/sub-02.md (実装メモ)

申し送り:
- `tools\gen_project_files.ps1` を回した結果 `build/Engine.vcxproj` / `build/Engine.vcxproj.filters` が更新されている (新規ファイルの登録。生成物なので一覧には入れていない。新規ファイルを使うビルドにはコミットが要る)。
- sub-03 (Forward): `OcclusionCuller` は DeferredPath 内に居るので、Forward へ広げるときは共有できる形に切り出すか Forward 側に持たせる。`RenderItem::hasWorldAabb` と `RenderView::occlusionEnabled` は経路に依存しない。ForwardPath のシェーダ (`forward_lit_instanced`) にも remap が要る。`--hzb-debug` に max-Z を出す口は `OcclusionCuller::ViewState::pyramid` (private) — 公開が要る。
- sub-04 (スキン AABB): `RenderSystem::CollectDrawables` の `if (c.skinned == 0)` で `hasWorldAabb` を立てているので、保守的 AABB が出来たらそこを外せばスキンも判定対象になる。
- Shader キャッシュ (`cache\shaders`) は新しい .hlsl の追加で自動再コンパイル。`cache\render_bench.scene.json` はシーン生成を変えていないので消していない。
- 一時ファイル (PNG / dump) は scratchpad にのみ置いた。リポジトリには増やしていない。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-09)。受け入れ条件 1〜6 の根拠を確認した: 3 構成の ON/OFF とカメラカット直後がすべて diffPixels=0、カットのフレームで phase2 2207、selftest の失敗注入、WARP では ON の方が速い、golden 30 枚 PASS。差分欄の解釈と追加はすべて仕様側で受け入れた (画面からはみ出す物は丸ごと可視 / スキンは sub-04 まで常にフェーズ 1 で描く / 順序を保つ prefix sum / 地形をフェーズ 1 の後に描いて遮蔽物にする / HZB を作れないフレームは全部可視)。移管: Forward への切り出し・統計の意味の固定・エディタの複数ビューの確認は sub-03 へ。フェーズ 2 の CPU 提出コストの計測は sub-06 へ。スキンを判定対象にするのは sub-04 (`if (c.skinned == 0)` を外す)。Unity / UE の一次資料が未確認なのは spec §7 の既知のリスクで、今回は問わない。
