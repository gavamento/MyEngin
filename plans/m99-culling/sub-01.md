# sub-01: 段別の CPU 時間とベンチの物量、BVH の採否

- 依存: なし
- 状態: OK (コミットは司会)
- 往復: 1

## やること
spec 4.1.8 / 受け入れ条件 11 (と 2 の `--render-bench-scale` 部分)。
- `--render-stats-dump` の `cpuMs` に `CollectDrawables` のステージ 1 (直列の候補収集) / 2 (並列の判定) / 3 (直列のキュー化) と全体の CPU 時間を足す (参考値。`counts` には入れない)。
  ステージ 2 は「視錐台判定の部分」を切り出して測れるようにする (LOD / URO / viewZ と分けた時間。分けると並列の粒度が変わるなら、判定だけの時間を別の計測モードで取ってよい。方法を実装メモに書く)。
- `--render-bench-scale K` (整数 1..32、既定 1): `--render-bench-demo` のグリッドを K 倍にする。K = 1 のシーンは今と 1 エンティティも変わらない。増やす物はグリッドの**後ろ**に足す (memory: デモの生成順は粒子 RNG のストリーム)。
- Release・実 GPU で K = 1 / 4 / 16 を `--shot-frame 300` で各 5 回測り、中央値の表を `docs\adr\ADR-038-culling.md` (新規、この時点では「計測」節だけ) に書く。
- 採用条件 (spec 4.1.8) を満たすかを SELF_EVAL に書く。BVH 自体は実装しない。

## やらないこと (このサブでは)
- BVH の実装、判定ロジックの変更 (`counts` が 1 つでも変わったら逸脱)

## 触る場所 (planner の見立て)
- `src\Engine\Engine\Rendering\RenderSystem.cpp` `CollectDrawables` (1103-1640) — 計測区間
- `src\Engine\Engine\Rendering\RenderStatsDump.{h,cpp}` — `cpuMs` 節 (`FormatRenderStatsJson` :102-154)
- `src\Engine\Engine\App\EngineCli.cpp` (:325 付近に `--render-bench-cut-frame`) / `EngineCliSelfTest.cpp`
- `src\Engine\Engine\Demo\DemoContentRender.cpp` `BuildRenderBenchScene` (:597)、`ShowcaseScenes.cpp:68-71`
- 時間の取り方は既存の `cpuMs.gbufferSubmit` (ADR-029 決定 1) に合わせる

## 受け入れ条件 (このサブ)
1. K = 1 の `--render-stats-dump` の `counts` が sub 着手前と完全一致 (Release で dump を 2 本取り diff)
2. `--render-bench-scale 0` / `33` / 文字列は解析エラー、`4` は通る (EngineCliSelfTest)
3. K = 16 で dump が書け、`cpuMs` に段別の時間が出る
4. ADR-038 の計測節に K = 1/4/16 の表 (中央値・ばらつき・機材) と採否の判定がある

## 検証コマンド
- Debug / Release ビルド
- `bin\x64\Release\Editor.exe --render-bench-demo --render-stats-dump <file> --shot-frame 300` (K = 1、着手前後)
- 同 `--render-bench-scale 4` / `16` を各 5 回
- `MYE_SELFTEST_ONLY=EngineCli,RenderStats` で `Editor.exe --selftest`
- `tools\check_rules.ps1`

## 実装メモ (coder が追記)

SELF_EVAL: sub-01 (round 1)
実装:
  - RenderSystem.{h,cpp}: CollectDrawables に steady_clock の計測 (total / ステージ 1 / 2 / 3)。`CullCpuTimes()` が直近 32 回の平均を返す。`cullTimingProbe` が true のときだけ、ステージ 2 の直後に視錐台判定だけの並列パスを捨て結果で回して `frustum` を測る (出力へ書かない)。
  - RenderStatsDump.{h,cpp}: `cpuMs` に collectTotal / collectStage1 / collectStage2 / collectFrustum / collectStage3 を追加 (`counts` は無変更)。
  - EngineLoop.cpp: `cullTimingProbe = !renderStatsDumpPath.empty()`。
  - EngineCli.{h,cpp}: `--render-bench-scale K` (整数 1..32、範囲外・綴り違いは Error)。値は EngineCliExtras.renderBenchScale。
  - EditorMain / RuntimeMain / ServerMain: extras の値を ShowcaseOptions.renderBenchScale へ写す。ShowcaseScenes.{h,cpp}: BuildRenderBench ラッパ。
  - DemoContentRender.cpp / DemoContent.h: `BuildRenderBenchScene(ctx, scale = 1)`。複製 (K-1) 枚は生成順の末尾。K = 1 は何も足さない。
  - EngineCliSelfTest.cpp: scale の 0 / 1 / 4 / 32 / 33 / abc / 4x と既定値。
  - docs\adr\ADR-038-culling.md (新規): 計測の節 (表・方法・採否)。
仕様との差分:
  - [追加] 視錐台判定だけの時間は「別の計測モード」で取った (spec が許す方法)。stage2 の直後の捨て結果パス。キャッシュが温まる分だけ stage2 内の判定よりやや速く出る (ADR に記載。結論には影響しない: stage2 全体でも 0.648 ms)。
  - [追加] ShowcaseOptions.renderBenchScale / EngineCliExtras.renderBenchScale と 3 Main への写し (Server 含む)。値の置き場が必要だったため。
  - [追加] 複製の配置 (4 列ずつ元グリッドの奥へ、間隔 184 m)。spec は「K 倍」としか言わないので最小の解釈。
検証:
  - Release|x64 / Debug|x64 の MyEngine.sln ビルド → 成功。
  - K=1 の counts: 着手前の Release exe の dump と新 exe の dump 2 本が完全一致 (JSON の counts を比較)。
  - K=1/4/16 各 5 回 (Release、Editor.exe、--shot-frame 300): 各 K で 5 回とも counts 一致。中央値は ADR-038 の表 (K=16: total 12.725 / stage1 7.862 / stage2 0.648 / frustum 0.477 / stage3 3.724 ms)。
  - Debug: K=1 と K=4 の counts が Release と一致。K=16 の Debug は 10 分超で終わらず打ち切り (未確認)。
  - MYE_SELFTEST_ONLY=enginecli,renderstats,render の `Editor.exe --selftest` (Debug) → 3 suites 0 failed。
  - tools\check_rules.ps1 → 0 error (60 warning は既存)。
  - 未実行: --warp、--no-jobs の counts 比較 (このサブの受け入れ条件外)。selftest-full、replay_verify、shot_verify (描画判定を変えていないため未実施)。
自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 1〜4 を検証済み。条件 2 の「`4` は通る」と 0 / 33 / 文字列のエラーは selftest。K=16 の Debug が未確認。
  正しさ: 4 — counts の不変を実 dump で確認。ただし計測パスの重複実行が dump run の CPU 時間にだけ上乗せされる点は意図どおり。
  コード品質: 4 — 既存の規約 (gbufferSubmit 相当の平均窓) に合わせた。窓が CpuTimeWindow と別実装 (チャンネル 5 本のため)。
  テスト: 3 — CLI の解析は selftest。段別時間・複製シーンの内容は dump の実走でしか見ていない (selftest なし)。
不安・質問:
  - Editor の撮影が使うカメラの far が遠く、K=16 でも culled は 2436 のみ (大半は視錐台の内側 → occluded)。視錐台で大半を落とす構図は未計測 (ADR に明記)。判定は「見送り」で変わらない見込みだが、planner が構図の追加計測を求めるか判断してほしい。
触ったファイル:
  - C:\HAL\MyEngin\src\Engine\Engine\Rendering\RenderSystem.h / RenderSystem.cpp / RenderStatsDump.h / RenderStatsDump.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Loop\EngineLoop.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\App\EngineCli.h / EngineCli.cpp / EngineCliSelfTest.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Demo\DemoContent.h / DemoContentRender.cpp / ShowcaseScenes.h / ShowcaseScenes.cpp
  - C:\HAL\MyEngin\src\Editor\App\EditorMain.cpp / C:\HAL\MyEngin\src\Runtime\RuntimeMain.cpp / C:\HAL\MyEngin\src\Server\ServerMain.cpp
  - C:\HAL\MyEngin\docs\adr\ADR-038-culling.md (新規)
  - C:\HAL\MyEngin\plans\m99-culling\sub-01.md (実装メモのみ)
申し送り:
  - BVH は見送り (ADR-038)。BVH のサブは足さない。後続サブは spec の sub-02〜09 のまま。
  - `cache\render_bench.scene.json` が残っていると --render-bench-scale が効かない (ADR に注意書き)。
  - Server は --render-bench-demo の解析を通る (scale を読む) が検証していない。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-11)。受け入れ条件 1〜4 を SELF_EVAL の根拠と実コード (RenderSystem.cpp の計測専用パスは捨て結果のみ・データ競合なし、ShowcaseScenes.cpp の cache\ は保存済みのときだけ読む既存の挙動) で確認。BVH は見送り (K=16 で視錐台判定 0.477 ms・全体の 3.7%)。不安への回答: 追加計測は不要 — BVH で削れるのは線形の判定時間が上限で、それが全件を判定して 0.477 ms。視錐台で大半を落とす構図では早期脱出で判定がさらに安くなるだけで、結論は変わらない。nit 2 件 (ADR の「判定費用は落ちる数によらない」を上限の論法へ言い換える = sub-09、scale != 1 で cache\ の保存済みシーンを読んだときに 1 行警告を出すかは任意) は申し送り。
