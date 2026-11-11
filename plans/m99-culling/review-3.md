# review-3: m99-culling

- 対象: `afdec5b5..30e7b7ac` (M99k: デバッグの線とナビの線を定数どおりの色で描く / M99l: 画面サイズ・距離カリングの撮影の run 間一致を cull_verify で確かめる)。round 1 / 2 の結果 (review-1.md / review-2.md) は引き継ぐ
- 日付: 2026-11-11

```
REVIEW: PASS
round: 3
軸 (1-5):
  製品の深度: 4 — 司会が未検証として挙げた 2 つの経路を撮影して確かめた。(a) PixelPerfectCamera2D の低解像度経路 (320x180 → 960x540): 固定した視錐台の白線は低解像度 RT に描かれ、ほかの絵と同じく 3 倍に拡大される。(b) エディタの Scene View / Game View: ナビの塗りの上にエディタのグリッドが乗り、壁の陰ではグリッドが深度で隠れる。ほかに、D3D の debug layer をオンにした Debug の実 GPU で、新しい重ね描き (Deferred + TAA + 枠表示 / 2D + 固定) を回した。ハザードや束縛のエラーは出なかった。見ていないもの: Play 中のエージェントの経路線をエディタで見ること、英語表示
  機能性: 5 — shot_verify は既知の flow_title 以外すべて PASS (更新した nav / perception / bt を含む)。cull_verify フル (2b の run 間一致を含む) は 27 checks で PASS。関連スイートも全部 PASS。距離の枠は定数の青の系統で出る
  ビジュアルデザイン: 5 — 枠が定数の色で出るようになり、明るい床の上でも読める (scratchpad\out\r3_dist_zoom.png)。新しい nav の golden は、塗りが暗く彩度の高い水色になり、経路線の緑がはっきり見える。崩れは無い (nav_cmp.png)
  コード品質: 4 — 変更は小さく、範囲は RenderSystem.{h,cpp} と cull_verify / cull_counts だけ。関数名と役割 (DrawParticlesAndVfx / DrawDebugOverlay) は分け方として妥当で、コメントは理由 (トーンマップで色が変わる、TAA 解決後なので非ジッタの射影) を書いている。check_rules は 0 error。気になる点はあるが、どれも指摘にするほどの根拠は無い (下の観察)
指摘: なし
観察 (指摘ではない。根拠が足りないもの、範囲外のもの):
  - 線は TAA 解決後の目標へ非ジッタの射影で描き、深度はジッタ付きで書かれたものを使う。深度テストのある線が面に沿う所では、半画素ぶん深度がずれる。コメント (RenderSystem.cpp の DrawDebugOverlay) は「手前へ寄せるので許容」としている。今回の撮影ではちらつきも欠けも見ていない (静止画だけなので、時間方向の揺れは未確認)
  - Debug + debug layer の nav (Deferred + TAA) で出た [d3d] の WARN は、46 件すべて「Pixel Shader が Slot 8/9 のサンプラーを期待しているのに未束縛」だった。線パスのものかは切り分けていない。重ね描きの移動で増えたかどうかは、基点との比較をしていないので不明
  - レビュー中に、round 2 で起動していたエディタ (--project scratch) が 19:47 ごろに自分で閉じた。.mye\imgui.ini と editor_settings.json が書かれていたので、通常の終了の形。クラッシュダンプも Application のイベントログも無い。その間 cull_verify の Runtime 窓が前面に出ていたので、私のクリックが別の窓へ当たった可能性がある。再起動後は問題なく動いた。製品の不具合の証拠にはしない
検証した手段:
  - `git log afdec5b5..HEAD` は 2 本 (422603ea / 30e7b7ac)。作業ツリーの変更は harness.md だけ。src (RenderSystem.{h,cpp}) と tools (cull_verify.bat / cull_counts.ps1) の diff を全部読み、sub-11 / sub-12 の実装メモ (原因の切り分けの表、α 混合で説明できる割合、golden の D ⊆ L_old ∪ L_new の表) を読んだ
  - 重ね描きの前提をコードで確かめた: view.dsv = target.dsv (RenderSystem.cpp:854)、HDR 中間は target と同じ寸法、UI (uiRenderer.Render、EngineLoop.cpp:3349) は Render の後なので線より上に乗る、PixelPerfect の低解像度経路では DrawDebugOverlay が lowTarget (低解像度の RT と DSV) へ描いてから拡大する (RenderSystem.cpp:818-835)
  - MSBuild で Release / Debug を再ビルドした (成功)
  - `tools\shot_verify.bat` → img-diff PASS 31、FAIL は flow_title だけ (maxDiff=62 / 944 px、既知)
  - `tools\cull_verify.bat` (フル) → `[PASS] culling verify (27 checks, ... run-to-run match, counts)`。2b の repeat: size / distance はどちらも diffPixels 0
  - `MYE_SELFTEST_ONLY=renderstats,occlusion,navagent,navsurface,perception,behaviortree,distancecull,enginecli` で Release `Editor.exe --selftest` → 8 suites, 0 failed
  - `pwsh tools\check_rules.ps1` → 0 error / 60 warning
  - 色: `--culling-distance-demo --cull-debug --cull-max-distance 40` (WARP、960x540、--no-fxaa) の枠の画素は (93,170,245) / (97,171,244) 前後 (定数 (51,153,255) を 1 px の AA で床に混ぜた系統)。修正前の round 1 の撮影では (175,214,229) だった。画像は scratchpad\out\r3_dist.png / r3_dist_zoom.png
  - golden: 旧 nav (afdec5b5) と新 nav を上下に並べた scratchpad\out\nav_cmp.png。変わったのは塗りと線だけに見え、形・物・影は同じ
  - PixelPerfect: リポジトリ直下 (GameLogic が読まれ主人公が動く) で `--2d-demo --cull-freeze-frame 2 --shot-frame 150` と、固定なしを撮った (scratchpad\out\pp_cmp.png)。固定した視錐台の白線が画面の上端と左端に出て、低解像度の画素 3 px 単位で太い。ログは `PixelPerfectCamera2D zoom 3, RT 320x180 -> 960x540` と `froze the culling camera for view 1`
  - debug layer: Debug Runtime (実 GPU、debug layer on) で `--nav-demo --deferred --taa --cull-debug` と `--2d-demo --cull-freeze-frame 2` を回した。どちらも exit 0。[d3d] の出力は上の観察のサンプラーの WARN だけで、ERROR / CORRUPTION / ハザードは無い
  - エディタ: Release Editor.exe --project <scratch> --nav-demo --cull-debug (実 GPU、2560x1440)。Scene View (scratchpad\out\r3_ed1.png / r3_ed1c.png) ではナビの塗り・輪郭・NavObstacle などの線が出ている。エディタのグリッドは塗りの上に描かれ、箱の陰では深度で隠れる。カメラのギズモの線も出ている。Game View (r3_ed5.png) ではナビの塗りと輪郭が出て、統計の文字が一番上に乗る。終了は自分の PID を Stop-Process で止めた
前回指摘の消込:
  - round 2 #1 (= round 1 #2、枠の色が淡い) — 解消。線パスとナビの塗りを ResolvePost の後に LDR 目標へ描くよう移した (422603ea)。枠は定数の色の系統で出る (上の色の検証)。DebugDrawLine とナビの線も同じ経路で直り、golden 3 枚の更新は sub-11 の表どおり線と塗りの画素に限られる
  - round 1 #1 / #3 — round 2 で解消済み (変化なし)
```
