# M78 プロジェクト側ポスト／コンピュート — review round 1

- slug: m78-project-shaders
- コミット範囲: `bbd11543770d2830552957ecf4122b0a55e5521d..HEAD` (27758be … 0bcc5dd)
- 日付: 2026-09-22

```
REVIEW: FAIL
round: 1
軸 (1-5):
  製品の深度: 3 — M78a–e の SelfTest と WARP 上の Compute ABI 縦切りは厚いが、エディタ Scene View (CameraOverride) と Play 停止後の Runner 寿命が仕様の「エディタ視界は不変」と矛盾しうる。リポジトリ内にサンプル `*.post.hlsl` / `*.fxstack.json` が無く、作者向けの即試行導線は薄い。ポスト本数ハード上限 (spec §4.4 例 8) はコンピュートのみ。
  機能性: 3 — 受け入れ 6–9・7 は機械検証で概ね充足。受け入れ 1–4 の手動 (Tint／マゼンタ／fill CS→ポスト) は本環境で GUI 未実施。CameraOverride 経路でユーザーポスト／BeforePost CS が残存実行しうる点は受け入れ 5 の「余計なパス」および §4.1 の Scene View 期待と衝突しうる major。
  ビジュアルデザイン: N/A — エディタ起動・スクショによるマゼンタ／Tint／CS 可視化は未実施 (ヘッドレス `--selftest` のみ)。マゼンタ代替シェーダと ERROR ログ経路はコード上存在 (`ProjectEffectRunner.cpp` 342–354, `assets/shaders/project_post_magenta.hlsl`)。
  コード品質: 4 — 層分離 (ComputeAbiRunner / ProjectComputeRunner / ProjectEffectRunner)、Properties 純関数、規則 11 v21=125、`EditorMain` の `ok &=` 集約は妥当。`RenderSystem.cpp` の CameraOverride 分岐は意図コメントと Resolve 注入が不整合。
指摘:
  1. [major] 宛先: coder — CameraOverride (エディタ Scene View 等) 中も `projectEffectRunner_` / `projectComputeRunner_` が `postFx_.Resolve` および BeforePost Dispatch に渡され続け、fxStack のロード更新は `!cameraOverride` 内だけのため、Play 後に停止すると Scene View でスタale なユーザーポスト／CS が走りうる (コメント「エディタ視界は不変」と逆)。 — 根拠: `src/Engine/Engine/RenderSystem.cpp` 1734–1737 (更新は `!cameraOverride` のみ) vs 1839–1851 (Override 有無に関わらず `HasPasses`→Dispatch と `&projectEffectRunner_` 注入) — 期待: Override 時は Runner を毎フレーム `ClearPasses` するか Resolve/Dispatch に `nullptr` を渡し、組込みポストのみの経路に戻す。SelfTest または最小再現手順を追加。
  2. [minor] 宛先: coder — spec §4.4 のポスト本数ハード上限 (例 8) が `ProjectComputeRunner` のみで `ProjectEffectRunner::SetPasses` には無い。 — 根拠: `ProjectComputeRunnerSelfTest` で 10→8 切捨て WARN を確認、`ProjectEffectRunner.cpp` `SetPasses` に同等上限なし — 期待: 仕様どおり定数化＋警告＋SelfTest 1 本。
  3. [minor] 宛先: coder — テクスチャ既定 `black` / `bump` (および `gray` も white 同等) が `RenderSystem` の Tex2D リゾルバで white にフォールバックのみ (TODO コメント)。 — 根拠: `src/Engine/Engine/RenderSystem.cpp` 1757–1762 — 期待: 組込み SRV を供給するか、未実装なら WARN と spec §4.1 エッジケースへの注記を docs/sub に同期。
  4. [minor] 宛先: planner — spec §4.1「Inspector で Properties をいじるとライブ反映」と sub-03 受理の save-on-apply＋ディスク再読込 (Save 後次フレーム反映) の文言が spec 本文に残存。 — 根拠: `spec.md` §4.1 83 行付近、`InspectorWindow.cpp` 2738–2740 Save ボタン、`RenderSystem.cpp` 毎フレーム `LoadFxStack` — 期待: §4.1/§4.3 を save-on-apply＋保存後自動反映に合わせて変更履歴に記録。
  5. [minor] 宛先: coder — 受け入れ 1–3 の手動検証用サンプル (`MyTint.post.hlsl` 等) がコミット範囲に含まれず、sub-03 手順のみ。 — 根拠: `git diff …HEAD --stat` に `assets/**/*.post.hlsl` / `*.fxstack.json` なし — 期待: 任意の `assets/` サンプル 1 セットまたは docs 専用 examples ディレクトリ (プロジェクト規約に沿って)。
検証した手段:
  - 読了: `plans/m78-project-shaders/spec.md`, `sub-01.md`–`sub-05.md`, `harness.md`
  - `git log bbd11543770d2830552957ecf4122b0a55e5521d..HEAD --oneline` (5 commits)
  - `git diff bbd11543770d2830552957ecf4122b0a55e5521d..HEAD --stat` (62 files; Material/GpuResources/Deferred 系なし)
  - `powershell -File tools\check_rules.ps1` → `0 error(s), 0 warning(s)`
  - `bin\x64\Debug\Editor.exe --selftest` → M78a–e ALL PASS、`[selftest] Compute ABI v21: PASS`; プロセス exit 1 (`Part self test: 8 FAILURE(S)` — CesiumMan/skinned アセット不足、台帳どおり本マイル外)
  - `tools\build_managed.bat` (Debug) / `tools\build_managed.bat Release` → 成功
  - コミット範囲 diff 重点: `RenderSystem.cpp`, `PostProcess.cpp`, `ProjectEffectRunner.cpp`, `ComputeAbiSelfTest.cpp`, `PartSelfTest.cpp` (MYE_API_VERSION 21u・7 スロット非 null)
  - 規則 11 変異テスト (Interop swap): 本ラウンドでは未再実行 (sub-05 SELF_EVAL 記載分を参照)
  - 手動 Tint／マゼンタ／fill CS→ポスト: 未実施 (GUI セッションなし)
前回指摘の消込: (round 1 のため該当なし)
```
