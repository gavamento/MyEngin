# review-1: m99-culling

- 対象: `d52e91f3103eacf6e5e07e4f44b8572063e1a166..3f97eb03` (M99a〜M99i、9 コミット)
- 日付: 2026-11-11
- reviewer の作業場所: 撮影・ダンプ・画面操作は scratchpad (`C:\Users\akita\AppData\Local\Temp\claude\58c0e4c841a0685f\f2580926-5c7d-48c2-9842-9b1a8a51de55\scratchpad\`)。リポジトリの `assets\project_settings.json` には触れていない (書き込みの確認は、assets をジャンクションにして `project_settings.json` だけ複製した scratch のプロジェクト `scratchpad\proj` に対して行った)

```
REVIEW: PASS
round: 1
軸 (1-5):
  製品の深度: 4 — 受け入れ条件の外側を 6 通り突いて破綻なし: (a) タグ別の距離を本物の描画経路で (scratch プロジェクトに tagCullDistances tag 5 = 50 m → culledDistance 2→3、Tagged_Far だけ追加で落ちる)、(b) 壊れた設定 (bool に文字列・数、minScreenSize "abc"、距離 -3、tag 64 / 負の距離 / 文字列の tag) → 7 行の [culling] WARN で既定値に落ち、ファイルは 1 バイトも変わらない、(c) CLI の範囲外 8 通り (0 / 1.5 / -1 / nan / -2 / "3x" / 33 / 0) が全部 exit 1 + 説明 1 行、(d) GPU 粒子が育った 180 フレーム目で --culling-demo の視錐台 ON/OFF が全画素一致 (cull_verify は 3 フレーム目だけ)、(e) 距離 80 m + 画面サイズ 0.02 を重ねた既定ベンチで --no-jobs と counts 42 欄一致、(f) エディタで Scene View トグル・枠表示・Project Settings の即時保存を実操作。残りは minor 3 件
  機能性: 5 — 受け入れ条件 1〜14 を実行で確認 (下の検証欄)。既知の例外 2 件 (flow_title 944 px / maxDiff 62、disturbancelong) は申告どおりの数字でだけ落ち、それ以外は全部 PASS
  ビジュアルデザイン: 4 — メニュー・Project Settings・Profiler の行・理由別の枠・固定した視錐台の白線はどれも出ている (scratchpad\out\ed4c.png / ed9.png / ed7c.png / bench_dbg.png / bench_dbg2.png)。ただし枠の色が淡く化け、明るい床の上の距離の青が読みにくい (minor 2)
  コード品質: 4 — 層・ABI・決定性の規約を守っている (check_rules 0 error、M99 で触ったファイルに新しい警告なし、ABI v35 のまま、NoHash コンポーネント)。仕様・engine_spec に書いた「サイズ変更で履歴を捨てる」が実装に無い (minor 1)
指摘:
  1. [minor] 宛先: planner — 画面サイズカリングの履歴を「サイズ変更で捨てる」は実装されていないのに、spec 4.1.4・sub-07・engine_spec §6.14 の 3 か所が実装済みのように書いている。sub-07 の SELF_EVAL の「差分」にも出ていない — 根拠: `sizeCullHistory_` を消すのは `RenderSystem::ResetRenderHistory` (src\Engine\Engine\Rendering\RenderSystem.cpp:2003-2010) だけ。呼び出しは RenderSystem.cpp:1229 (tick の巻き戻し)・:3112 (ReleaseGpu)・EngineLoop.cpp:1214 / :3293 (シーン切り替え) で、リサイズの経路は無い (`Grep ResetRenderHistory\(\)` の全件)。engine_spec.md:1082 「is cleared by `ResetRenderHistory()` and by a size change」。手本にした `lodHistory_` もリサイズでは消えていない。物差しの BoxScreenSize は画面の高さに対する比 (proj._22) で解像度に依らないので、消さなくても挙動の害は見当たらない — 期待: 「サイズ変更で捨てる」が要るかを決める。要らないなら spec 4.1.4 / sub-07 / engine_spec §6.14 から消す (推奨。比なのでリサイズで値が変わらない、と理由を添える)。要るなら coder のサブにする
  2. [minor] 宛先: coder — デバッグ枠の色が名前付き定数の色で出ず、淡く化ける。明るい床の上の距離の青は見分けにくい (sub-08 の nit、harness 申し送りの「距離の青が薄い」と同じもの。数字で裏を取った) — 根拠: `--culling-distance-demo --cull-debug` (scratch プロジェクト、タグ 5 = 50 m) の撮影 scratchpad\out\tag_dist_img.png の枠の画素は (175,214,229) 前後で、`kCullBoxDistanceRgba = 0x3399FFFF` (51,153,255) より床 (218,218,215) にずっと近い。オクルージョンの赤 (0xFF3333) も scratchpad\out\bench_dbg2.png ではピンクに見える (赤は M90 の既存の線と同じ描き方なので M99 で悪化したわけではない)。黄 (視錐台) は bench_dbg.png で濃く出ている。線は HDR の中間に描いていて (RenderSystem.cpp の DrawParticlesAndDebug、ポスプロ解決前)、トーンマップ・露出を通って色が変わると読める (推定。原因の切り分けはしていない) — 期待: ユーザーの目視 (test_checklists の M99 の項目) で足りなければ、線をトーンマップ後に描くか、色を濃くする。凡例 (ツールチップ) の色名と見た目が食い違わないこと
  3. [minor] 宛先: planner — 作業ツリーに増えた sub-10.md (このレビューの範囲外、未コミット) が「cull_verify の撮影条件には `--no-audio` が無い」としているが、既にある — 根拠: tools\cull_verify.bat:40 `set COMMON=--no-audio --font-embedded --width 960 --height 540 --no-fxaa` (Runtime の撮影は全部 %COMMON% を通る。:pair / :dump / :config_counts)。基点比較の行 (:95 付近) も %COMMON% を使う。Editor.exe の起動は --img-diff だけ — 期待: sub-10 の「`--no-audio` を足す」を「付いていることを確かめる」に直す (coder に既にある引数を足させない)
検証した手段:
  - ビルド: MSBuild で Release|x64 と Debug|x64 を作り直し → どちらも成功 (bin の exe は 16:32 / 16:37 で、最後のコミット 17:25 の EngineCliSelfTest / TagNames の変更より古かったため)
  - diff を全部読んだ: RenderSystem.{h,cpp} (760 行)、CullingSettings.{h,cpp}、DistanceCull.h、GpuParticleBackend.{h,cpp} と ParticleCurves.h の上界、CpuParticleBackend、VfxGeometry.h / VfxRenderer、Sprite2DRenderer.{h,cpp}、EditorApp (メニュー)、ProjectSettingsWindow、ProfilerWindow、Components (RenderCulling)、SimSnapshot.h、EngineCli、RenderStatsDump、EngineLoop、Runtime / Server / Editor の main、TagNames、DistanceCullSelfTest、Sprite2DAssetSelfTest、TagSelfTest、tools\cull_verify.bat。「仕様との差分」に無い変更は、指摘 1 の「やっていないこと」以外は見つからなかった (TagNames の ResolveOcclusionCulling 削除はコミット本文と sub-09 に書いてある)
  - 粒子の上界 (D12) をシェーダと突き合わせた: particle_sim.cs.hlsl:170-191 (加速度 = 重力 + 風 + 乱流 + 場)、GpuParticleBackend.cpp:569-630 (方向は ParticleBasisRotateDir で正規化 = ワールドのスケールで速さが伸びない、形状のずれは基底を掛ける = フロベニウスノルムで上から押さえて足りる)、particle_render_gpu.hlsl:172-214 (大きさはワールド行列を掛けない)。式に外れは見つからなかった
  - `pwsh tools\check_rules.ps1` → 0 error / 60 warning。M99 で触ったファイルの警告は docs\test_checklists.md:932 の 1 件だけで、git blame で 91886b51 (M96、2026-11-09) の行 = 既存
  - `tools\cull_verify.bat` (MYE_CULL_SKIP_CONFIGS なし = 構成間の比較込み) → `[PASS] culling verify (23 checks ...)`、354 秒。視錐台 ON/OFF の全画素一致 9 組 (diffPixels 0)、既定ベンチの OFF で culled 0・drawCalls 23→25、固定の一致と空振り検査、counts 5 シーン x (Release GPU / Debug GPU) が Release --warp と 42 欄一致。(コミット本文の「24 checks」との差 1 は、MYE_CULL_BASE_ROOT を渡していない分)
  - 受け入れ 3 の影の数: tests\actual\cull\bench_on.json と bench_off.json を欄ごとに比較 → 違うのは drawCalls / triangles / culled / lodDraws / lodTriangles / paletteEvaluated (6→7、カメラ背後のスキンを描く分) / occlusionPhase1Draws だけ。shadowCascadeCasters [6,43,833] と shadowDrawCalls 44 は一致
  - `bin\x64\Debug\Editor.exe --selftest-full` → `total 121 suites, 1 failed` (821 秒)。落ちたのは disturbancelong の「final hash at tick 600 equals the baked value」だけ (spec 7. の既知)。distancecull / enginecli / simsnapshot / renderstats / particle / vfx / sprite2dasset / tag / occlusion は PASS
  - `tools\shot_verify.bat` → 31 枚 PASS、flow_title だけ FAIL (maxDiff=62 diffPixels=944、spec 7. の数字と同じ)
  - `tools\replay_verify.bat` (Debug / Release を再ビルド) → `[parallel] all 24 jobs passed in 323.3s` / `[PASS] replay consistency (Debug/Release, 17 scenes ...) + snapshot round-trip + jobs A/B + compute A/B + time travel + rule check`。1 回目は Debug の selftest-full が Editor.exe を使っていて nethost.dll のコピーで MSBuild が失敗したので、selftest の終了後に回し直した
  - Release の関連スイート: `MYE_SELFTEST_ONLY=distancecull,enginecli,simsnapshot,renderstats,particle,vfx,sprite2dasset,tag` で `bin\x64\Release\Editor.exe --selftest` → 8 suites, 0 failed (Release の --selftest-full は未実施)
  - 深度の検査 (scratch プロジェクト、Release Runtime、--warp、960x540、--no-fxaa):
    - タグ別の距離の実経路: tagCullDistances [{tag 5, 50}] で `--culling-distance-demo` の culledDistance 3 (`--no-distance-culling` で 0)。枠の撮影 scratchpad\out\tag_dist_img.png に青い枠 3 つ (指摘 2 の画像)
    - 壊れた設定: WARN 7 行、ファイルのハッシュ不変、counts は既定値どおり (scratchpad\out\badset.stderr)
    - CLI 範囲外 8 通り: すべて exit 1 と stderr 1 行
    - `--culling-demo --particle-backend gpu` の 180 フレーム目: ON/OFF を img-diff --tol 0 → PASS (diffPixels 0)。ON は culledNonMesh 3 (画面外の GPU エミッタ・スプライト・デカール)、unculledGpuEmitters 2 (乱流 2)、OFF は両方 0。リポジトリ直下 (GameLogic の Rotator が動く) の 40 フレーム目は culledNonMesh 4 (トレイル込み)・unculledGpuEmitters 3 (動くエミッタ込み) で、sub-04 の記録と一致。(scratch プロジェクトでは GameLogic が読まれず回転台が止まるので、トレイルは描かれない)
    - 既定ベンチ + `--cull-max-distance 80 --cull-min-screen-size 0.02`: `--no-jobs` と counts 42 欄一致 (culledDistance 2903)
    - 固定 + 枠: `--render-bench-demo --cull-debug --cull-freeze-frame 10 --render-bench-cut-frame 20` の 30 フレーム目 (scratchpad\out\bench_dbg.png) で、固定した視錐台の白線と、今の視界に入っているが固定の視錐台の外にある物の黄色い枠が出る
  - エディタの画面 (Release Editor.exe --project scratchpad\proj --culling-distance-demo、実 GPU、2560x1440。マウスを Win32 で動かして撮影。終了は自分の PID を Stop-Process):
    - 表示 > レンダリング > カリング に 視錐台 / GPU オクルージョン / 距離 / 画面サイズ の 4 スイッチ (既存のオクルージョンはここへ移っている)、区切りの下に Scene View / 固定 / 落とした物を表示 (scratchpad\out\ed4c.png)。「落とした物を表示」のツールチップが色の凡例 (ed6tipc.png)
    - 既定では Scene View で遠い 3 つが残り (ed1.png)、「Scene View にもゲームのカリングを掛ける」で 3 つ消える (ed5.png)。枠表示で淡い青い枠 3 つ (ed6.png)
    - Profiler に「カリング理由別: 視錐台 0 / 距離 3 / 画面サイズ 0 / ...」とビュー 2 の理由別の行 (ed7c.png)
    - Project Settings > レンダリング > カリング: チェック 4 つ、最小画面サイズ (0.0050)、既定の最大描画距離、タグ別の距離 (Tag 5 = 50.0)、Runtime の注記 (ed9.png)。画面サイズカリングのチェックを入れた瞬間に scratch の project_settings.json へ `"sizeCulling": true` が書かれ、他のキー (particle*、tagCullDistances) は残った
    - 見ていないもの: 英語表示、Game View 側の固定、数値欄の編集終了時の保存、4096 個を超えたときのメニュー表示
  - 作業ツリー: レビュー中に `plans\m99-culling\spec.md` の変更と `sub-10.md` が増えたが、どちらも reviewer が書いたものではない (司会・planner の作業と読める)
前回指摘の消込: (round 1 のため無し)
```

