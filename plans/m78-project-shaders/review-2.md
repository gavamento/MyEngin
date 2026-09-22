# M78 プロジェクト側ポスト／コンピュート — review round 2

- slug: m78-project-shaders
- コミット範囲: `bbd11543770d2830552957ecf4122b0a55e5521d..HEAD` (27758be … 4595ccb)
- 前回: `review-1.md`
- 日付: 2026-09-22

```
REVIEW: PASS
round: 2
軸 (1-5):
  製品の深度: 4 — CameraOverride 時の Runner 無効化・ポスト上限 8・Tex2D 既定の WARN/docs・MyTint サンプルが揃い、作者の試行導線が改善。gray/black/bump の専用 SRV は docs どおり後続可。GUI 上の Tint/マゼンタ/CS→ポストの実画面確認は未実施。
  機能性: 4 — 受け入れ 6–9・7 を再実行で充足。review-1 major (#1) は ClearPasses＋`ShouldInjectProjectFxStack`＋SelfTest で解消。`Editor.exe --selftest` 全体 exit 1 は Part 8 FAIL (CesiumMan 等) のみ (台帳どおり本マイル外)。
  ビジュアルデザイン: N/A — エディタ起動・スクショによる Tint/マゼンタ/fill CS 可視化は本ラウンドも未実施。サンプル `assets/shaders/MyTint.post.hlsl` + `assets/MyTint.fxstack.json` はリポジトリに存在。
  コード品質: 4 — `ProjectFxStackPolicy.h` で注入方針を単体テスト可能に分離。ポスト上限は `ProjectEffectRunner::kMaxPostPasses`＋SelfTest。fix コミットは M78 スコープ内 (plans/docs/assets 含む)。黙った Material/Deferred 差分なし。
指摘:
  (blocker/major/minor 0 件 — round 1 指摘はすべて消込)
検証した手段:
  - 読了: `review-1.md`, `spec.md` (§4.1 save-on-apply / CameraOverride 追記), `harness.md`, fix 3 コミットの `git show --stat`
  - `git log bbd11543770d2830552957ecf4122b0a55e5521d..HEAD --oneline` (8 commits)
  - `powershell -File tools\check_rules.ps1` → `0 error(s), 0 warning(s)`
  - `bin\x64\Debug\Editor.exe --selftest` → M78a–e ALL PASS、`FxStack: CameraOverride injection policy` PASS、`Compute ABI v21: PASS`; exit 1 (`Part self test: 8 FAILURE(S)`)
  - コード確認: `RenderSystem.cpp` 1735–1740 (Override ClearPasses), 1860–1880 (`injectPostFx`/`injectComputeFx` nullable), `ProjectEffectRunner.cpp` 41–46 (上限 8), `docs/project-shaders-tex2d-defaults.md`
  - 手動 Tint/マゼンタ/fill CS→ポスト: 未実施 (GUI なし)
前回指摘の消込:
  1. [major] 宛先 coder — **解消** — Override 時 `ClearPasses` (`RenderSystem.cpp` 1737–1739) と `ShouldInjectProjectFxStack` により BeforePost Dispatch/Resolve へ `nullptr` (`1860–1880`)。`FxStackSelfTest.cpp` テスト 8 で policy を検証 (`Editor.exe --selftest` ログ)。
  2. [minor] 宛先 coder — **解消** — `ProjectEffectRunner::kMaxPostPasses`＋`SetPasses` 切捨て WARN (`ProjectEffectRunner.cpp` 41–46)。`ProjectEffectRunnerSelfTest.cpp` テスト 5 (c1bbe86)。
  3. [minor] 宛先 coder — **解消** — gray/black/bump 初回 WARN (`RenderSystem.cpp` 1765–1782) と `docs/project-shaders-tex2d-defaults.md` (a96f015)。
  4. [minor] 宛先 planner — **解消** — `spec.md` §4.1 83–84 行 save-on-apply と Scene View Runner 無効を明文化 (変更履歴 289 行、c1bbe86/a96f015)。
  5. [minor] 宛先 coder — **解消** — `assets/shaders/MyTint.post.hlsl`, `assets/MyTint.fxstack.json` (+ meta) をコミット (a96f015)。
```
