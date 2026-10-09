# review-1: m90-render-optimization

- 対象: `3b30251..be31dbd` (M90a〜M90i の 9 コミット)
- 日付: 2026-10-10
- 作業ツリー: 台帳 `harness.md` / `sub-08.md` の未コミット差分は対象外 (触っていない)

```
REVIEW: FAIL
round: 1
軸 (1-5):
  製品の深度: 3 — 境界 (奇数・極小・縦長の解像度、デバイス消失 2 回、unique bench、地形・破壊・render demo) では ON/OFF の画素が全部一致し、2 フェーズの「欠けない」設計は実際に効いている。ただしオクルージョン ON の GBuffer 提出 CPU 時間が解像度に比例して伸びる (= GPU 待ちが入っている、#1)。カメラカットのフレームで LOD メッシュがあると ON/OFF が 11 画素違う (#2)。どちらも ADR-029 の結論 (CPU コストの原因、「画素差 0」) と食い違う
  機能性: 3 — 受け入れ 2 / 9 / 10 / 12 / 14 は再実行して PASS (dump の counts は Release / Release+WARP / Debug / Debug+WARP の 4 本で完全一致、shot_verify 30 枚、replay_verify 19 ジョブ (jobs A/B を含む)、check_rules 0 error、selftest は両構成で exit 0)。受け入れ 8 の「ON/OFF の画素差 0 (z-fight 由来なら原因を示す)」はカメラカットのフレームで満たしていない (#2)
  ビジュアルデザイン: 4 — render_bench の Deferred の絵 (画面外の Tower の影が床と壁に落ちる)、`--hzb-debug 1/3 --hzb-debug-max` の max-Z と赤い AABB を見て崩れはなかった。見た画像: scratchpad\shots\def_on.png、hzbmax_def.png、hzbmax_fwd.png、fwd_cut_on.png。ProfilerWindow とエディタのメニュー・ツールチップは GUI なので見ていない (目視待ち)
  コード品質: 3 — 描画の並列化 (確保は直列 → 評価は並列 → 結合は index 順) と URO・パレットキャッシュの持ち方は規約に沿い、check_rules の新規警告も無い (rule 7 の 10 件は全部 M90 前の行)。統計の読み戻しが `D3D11_MAP_READ` のフラグ 0 (待つ) で spec §4.1.1 の「読み戻しを待たない」に反する (#1)。小さい残りは #4〜#7
指摘:
  1. [major] 宛先: coder — オクルージョン ON の GBuffer 提出 CPU 時間 (`cpuMs.gbufferSubmit`) が解像度に比例して伸びる。OFF は解像度によらず約 0.2 ms なので、ON の分は CPU の仕事ではなく GPU の完了待ちである。spec §4.1.1 の「GPU で決まる数は読み戻しを待たない」に反する。ADR-029 の計測表 (CPU 提出 0.23 → 1.31 ms、unique 0.71 → 2.52 ms) は待ち時間を含んでいる可能性が高い。それを「フェーズ 2 が run ごとに状態設定・CB 更新・indirect を出し直すため」と説明しているので、OFF を勧める判断表の根拠も怪しくなる — 根拠: `Runtime.exe --render-bench-demo --deferred --no-audio --font-embedded --shot-frame 300 --render-stats-dump` (Release、実 GPU、各 3 回) の gbufferSubmit。ON / OFF の順に、320x180 で 0.24〜0.33 / 0.18〜0.26 ms、1920x1080 で 1.43〜1.62 / 0.17〜0.23 ms、3840x2160 で 2.77〜3.55 / 0.19〜0.33 ms。draw 数は解像度で変わらない。最有力の候補は `src\Engine\Renderer\Passes\OcclusionCullPass.cpp:449` の `Map(src, 0, D3D11_MAP_READ, 0, &mapped)` (フラグ 0 なので、2 フレーム前のコピーが終わっていなければ止まる)。`TestPhase2` → `ReadStats` は GBuffer の提出の中で毎フレーム呼ばれる。GpuTimer の読み出しも同じ形なら合わせて疑う — 期待: 統計の読み出しは `D3D11_MAP_FLAG_DO_NOT_WAIT` にし、`DXGI_ERROR_WAS_STILL_DRAWING` なら前の値を残す (ほかの読み戻しも同様)。そのうえで、上の解像度 3 段で ON の CPU 提出が OFF との差 + 解像度に依存しない定数に収まることを示す。ADR-029 の「計測」節と判断表は測り直して書き直す
  2. [major] 宛先: coder — カメラカットのフレームで、オクルージョンの ON と OFF が 11 画素違う (Forward は maxDiff=47、Deferred は maxDiff=49)。ADR-029:295 は「ON/OFF の画素差は 0 (z-fight 由来の差も出なかった)」と書き、受け入れ 8 も「差が出たら原因を示す」を求めているが、原因が示されていない — 根拠: `Runtime.exe --render-bench-demo --warp --no-audio --font-embedded --width 960 --height 540 --frames 32 --shot-frame 30 --render-bench-cut-frame 30 [--deferred] [--no-occlusion] --screenshot X` を `Editor.exe --img-diff A B --tol 0` で比べた。最悪の画素は (365,326)、11 画素はすべて x=363〜365 / y=325〜331。切り分けの観測: (a) カットの次のフレーム (shot 31)、カット後の定常 (cut 20 / 0、shot 30)、カットなしは、どれも差 0。(b) `--lod-force 0` と `--lod-bias 0.01` (全部が最も粗い段) では差 0、`--lod-force 1` では同じ 11 画素が違う。(c) ON の 11 画素は `--no-occlusion --lod-force 0` の絵と一致し、OFF は `--lod-force 1` の絵と一致する。(d) dump の counts (lodDraws [2509,1,0,7] など) は ON / OFF で同じ。場所は LOD 見本の球 (radius 3) と、隣のグリッドの球が交わる線の上 (render_bench は LOD 見本の球をグリッドの格子点に重ねて置いている)。仮説は 2 つあり、どちらかは未断定。① 交線上の同じ深度の画素で、カットのフレームだけ描く順序 (フェーズ 1 / 2 の分かれ方) が変わり、勝つ物が入れ替わる (= z-fight 由来)。② ON の経路で LOD 段の範囲か影が取り違えられている。画像: scratchpad\shots\fwd_cut_on.png / fwd_cut_off.png / ctx_on.png (差の位置に赤印) — 期待: 原因を断定する。① なら ADR-029:295 と 3-2 節 (「カット直後でも欠けない」) に、交差する物の同深度の画素は描画順で変わりうることを書き、OcclusionSelfTest の「カット直後」に LOD 付きの交差を含む形を足す。② なら直す
  3. [minor] 宛先: planner — 既定 ON の判断 (ユーザー判断 2026-10-09) は、sub-06 の「CPU 提出 +0.15 ms / unique 1.7 倍」を前提にしていた。sub-08 の実測では、実 GPU で ON が速かったのは 4 通り中 1 通り (既定 bench の Deferred) だけで、CPU 提出は常に約 1 ms 以上重い。ただし #1 によりこの CPU 値には GPU 待ちが混ざっている可能性が高い。私の実 GPU の再測定 (1080p、各 3 回) も GBuffer / Forward の ms が 0.4〜8.8 で揺れ、ON / OFF の優劣は読めなかった — 根拠: ADR-029:261-266、scratchpad\perf の dump、本レビュー #1 — 期待: #1 を直して測り直したあとも ON が実機で得にならないなら、既定 ON を続けるかをユーザーに確認する `[ユーザーに聞ける]`。得になるなら記録だけでよい
  4. [minor] 宛先: coder — Rendering メニューの表示が「GPU Occlusion Culling (Deferred)」/「GPU オクルージョンカリング (Deferred)」のまま。M90c から Forward にも効く — 根拠: `src\Engine\Core\Localization\LocalizationTable.inl:88` (sub-08 VERDICT の nit が未対応) — 期待: 「(Deferred)」を外す (両言語)
  5. [minor] 宛先: coder — CLI とファイルを合わせる式 (`Load && cli`) が本体と selftest に 1 つずつ書かれていて、本体の配線が変わっても selftest は気付かない — 根拠: `src\Engine\Engine\Loop\EngineLoop.cpp:387` と `src\Engine\Engine\App\EngineCliSelfTest.cpp:264-278` (sub-09 VERDICT の should が未対応) — 期待: `ResolveOcclusionCulling(assetsRoot, cliFlag)` のような関数に出し、両方から呼ぶ
  6. [minor] 宛先: coder — `TagNames.h` の冒頭の説明が「タグ名の表と RT のタグ設定」のままで、描画設定 (`rendering.occlusionCulling`) も置いていることが書かれていない — 根拠: `src\Engine\Engine\Scene\TagNames.h:4` (sub-09 の nit が未対応) — 期待: 冒頭の 1 行を直す
  7. [minor] 宛先: coder — 手動確認表に存在しない CLI の旗 `--forward` が書かれている (Forward は既定。旗は `--deferred` だけ) — 根拠: `docs\test_checklists.md:624`。`--forward` は `src\Runtime\RuntimeMain.cpp` にも `EngineCli.cpp` にも無い — 期待: 「`--deferred` あり / なし」に直す
検証した手段:
  - ビルド: MSBuild `MyEngine.sln` Release / Debug x64 → どちらも exit 0 (warning / error 0)
  - `bin\x64\Release\Editor.exe --selftest` → exit 0 (94 秒)。`bin\x64\Debug\Editor.exe --selftest` → exit 0 (502 秒)。ServerNetSelfTest の一過性 FAIL は今回出ず。SimParallel の参考時間: Release の知覚は jobs 0.137 / 直列 0.325 ms、Debug は jobs 8.900 / 直列 5.233 ms (既知の「Debug だけ遅い」を再現)
  - `pwsh tools\check_rules.ps1` → 0 error / 50 warning。M90 で触ったファイルの rule 7 警告 10 件は、git blame で全部 M90 前の行と確認
  - `tools\replay_verify.bat` → `[PASS]`、19 ジョブ (jobsab を含む) が 229.5 秒で全部 PASS
  - `tools\shot_verify.bat` → `[PASS]` 30 枚 (terrain の 1 枚だけ maxDiff=5 で tol=12 以内、残りは maxDiff=0)
  - 受け入れ 2: render_bench の `--render-stats-dump` (960x540、shot 30) を Release / Release+WARP / Debug / Debug+WARP で取り、counts が 4 本とも完全一致。tri の内訳は lodDraws [3364,2,2,10]、occluded 2793、shadowCascadeDraws [6,16,22]
  - 画素 A/B (WARP、tol=0、`Editor.exe --img-diff`): render_bench の Forward / Deferred で ON と OFF、ON と `--no-jobs`、ON と `--no-uro` → 全部 0。カメラカット → 11 画素 (#2)。境界: 333x177、17x9 (実際の出力は 120x9)、301x997、`--render-bench-unique-demo`、`--anim-demo` の ON / OFF と `--no-jobs`、`--render-demo --deferred`、`--terrain-demo --deferred`、`--fracture-demo`、`--simulate-device-lost 20,29` の ON / OFF とデバイス消失なしとの比較 → 全部 0
  - 実 GPU の ms: 上の 2 シーン × 2 経路 × ON / OFF × 3 回 (1080p、shot 300)。Deferred の CPU 提出は 3 解像度 × ON / OFF × 3 回 (#1)
  - 見た画像: scratchpad\shots\def_on.png、fwd_cut_on.png、fwd_cut_on_crop.png、fwd_cut_off_crop.png、ctx_on.png、hzbmax_def.png、hzbmax_fwd.png
  - 読んだ範囲: spec.md、harness.md、sub-08 / sub-09、ADR-029、test_checklists の M90 節、engine_spec の M90 該当行。diff は RenderSystem.cpp の全部、occlusion_cull.cs.hlsl、OcclusionMath.h、OcclusionCullPass.{h,cpp} の Begin / Dispatch / TestPhase2 / ReadStats、DeferredPath.cpp の GBuffer の 2 フェーズ部、ShadowPass.cpp、SkinPaletteCache.{h,cpp}、MeshLod.h、SkinBounds.h、TagNames、EngineCli / EngineCliSelfTest / EngineLoop の差分、インスタンス版シェーダ 2 本、JobSystem / TickRunner / Skeleton / RenderQueue / ProbeBaker の差分
  - 未実施: `project_settings.json` を false にした Runtime の dump (レビューではリポジトリのファイルを書き換えないため。coder の dump と TagSelfTest / EngineCliSelfTest の PASS で代用)。エディタの GUI (メニュー・ツールチップ・ProfilerWindow・Scene View と Game View の同時表示)。三校の実シーンでの計測
  - 仕様に無い変更 (黙った差分): 見つからなかった。JobSystem の Stats、Skeleton の revision、TickRunner の sceneLoadSerial、GpuTimer の ready_ ガードは、いずれも sub の実装メモか spec §8 に出所がある
前回指摘の消込: (round 1 のため無し)
```

## 目視待ち (ユーザー。指摘ではない)

`docs\test_checklists.md` の M90 節が正本。レビューで見られなかったものは次のとおり。

- ProfilerWindow の新しい欄 (GPU ms、オクルージョン、LOD の分布、ビュー別) の見た目
- Rendering メニューのオクルージョンの切り替えと保存 (再起動後も保たれるか、`scmhint::Changed`)、ツールチップの両言語
- Scene View と Game View を同時に出したときの、ビュー別の統計とキャラの姿勢の一致
- LOD の見た目 (ポップ、法線・UV の崩れ)、URO のカクつきの許容距離
- Inspector のモデル LOD 設定 UI (適用 → `.meta` を書く → 再クック)
