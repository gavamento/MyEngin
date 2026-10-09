# sub-09: GPU オクルージョンの ON/OFF をプロジェクト設定に保存する

- 依存: sub-06 (`EditorApp.cpp` の Rendering メニューと `EngineLoop.cpp` の設定の流し込みを同じ順で触るため)
- 状態: OK (commit 141885c)
- 往復: 1

## 背景
sub-06 の計測で、run がすべて別のシーン (`--render-bench-unique-demo`) ではオクルージョン ON の CPU 提出が OFF の約 1.7 倍になった。ユーザー判断 (2026-10-09、台帳): 「既定 ON で、設定で ON/OFF を切り替えられるように」。
現状の切り替え口 (エディタの Rendering メニュー `Menu_Occlusion`、`EditorApp.cpp:1224` / CLI `--no-occlusion`) は**どちらも保存されない**。エディタを再起動すると ON に戻り、ビルド後のゲーム (Runtime) には切り替える口が CLI しかない。ON / OFF のどちらが得かはシーンの作りで決まる「プロジェクトの決めごと」なので、RT のタグ規則 (`rayTracingTags`、`EditorApp.cpp:1251` のコメント、`TagNames.cpp:169-197`、`EngineLoop.cpp:398-405`) と同じく `assets\project_settings.json` に保存する。spec §4.1.4 / §4.3。

## やること
1. `assets\project_settings.json` に `"rendering": { "occlusionCulling": true|false }` を足す。読み書きは Engine 層 (`LoadRtTagRules` / `WriteSettingsKey` と同じ場所・同じ流儀。他のキーは保持、壊れたファイルは上書きしない)。キーが無い = true (既定 ON。既存のプロジェクトは 1 ビットも変わらない)。
2. `EngineLoop` の起動時に読み、`EngineConfig::occlusionCulling` の既定値にする。実効値 = ファイルの値 && `--no-occlusion` が無いこと。**CLI はファイルへ書き戻さない**。CLI で強制的に ON にする旗は足さない (ファイルが false のときにベンチで ON を見たければファイルを戻す) (`EngineCli.cpp:342` の RT タグ規則の前例と同じ)。Editor と Runtime の両方が同じ経路で読む。
3. エディタの Rendering メニューの `Menu_Occlusion` を切り替えたら `project_settings.json` へ保存し、`scmhint::Changed` を呼ぶ (RT タグ規則と同じ)。メニューの項目に「プロジェクトに保存される」ことが分かる注記を付ける (`Tr()` 両言語。既存の `Menu_RtSceneHint` 等の流儀)。
4. URO・lodBias・強制段は保存しない (このサブの範囲外。今のまま起動ごとの切り替え)。

## やらないこと (このサブでは)
- ゲーム内のオプション画面 (プレイヤーが切り替える UI)、GameLogic へ出す API (ABI を上げない、spec §2 #16)。
- URO / LOD の設定の保存。

## 触る場所 (planner の見立て)
- `src\Engine\Engine\Scene\TagNames.cpp` 付近の settings 読み書き (`LoadRtTagRules` / `WriteSettingsKey`)。置き場所が「タグ」のファイルに合わなければ、同じ層に描画設定の読み書きを小さく足す (多目的 Manager にしない)
- `src\Engine\Engine\Loop\EngineLoop.cpp` (`renderSystem.enableOcclusionCulling = config.occlusionCulling;` の前)、`EngineCli.cpp`
- `src\Editor\App\EditorApp.cpp:1224`、`LocalizationTable.inl`
- 前例の selftest: `TagSelfTest.cpp:280-298`、`RunProjectSettingsFileSelfTest`

## 受け入れ条件 (このサブ)
1. selftest: 保存して読み戻せる / キーが無いと true / 壊れたファイルは既定 true で読み、上書きしない / 他のキー (`rayTracingTags` 等) を保持する。
2. selftest または CLI の selftest: ファイルが false でも `--no-occlusion` 無しなら false、ファイルが true で `--no-occlusion` なら false、`--no-occlusion` はファイルを書き換えない。
3. `project_settings.json` に false を書いた状態で `Runtime.exe` の render_bench を `--render-stats-dump` すると、occlusion 系の数が 0 (OFF で走る)。キーを消すと従来どおり occluded > 0。 — dump 2 本
4. 既存 golden 全 PASS、`tools\check_rules.ps1` PASS、ローカライズ両言語。

## 検証コマンド
- ビルド Debug / Release、`--selftest` 両構成、`tools\check_rules.ps1`、`tools\shot_verify.bat`、render_bench の dump 2 本
- 一時的に書き換えた `assets\project_settings.json` は検証後に元へ戻し、差分が無いことを確かめる

## 実装メモ (coder が追記)
round 1: TagNames.cpp に Load/SaveOcclusionCullingSetting (rendering.occlusionCulling、キー無し/壊れ = true、保存は UpdateProjectSettingsFile)。EngineLoop は `Load && config.occlusionCulling`。EditorApp は切替で保存 + scmhint::Changed + ツールチップ (Menu_OcclusionHint)。selftest は TagSelfTest と EngineCliSelfTest。Debug/Release selftest exit 0、check_rules 0 error、shot_verify exit 0 (30 枚)、render_bench dump は既定 occluded 2867 / false 0。project_settings.json は元に戻し git diff 無し。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-09)。受け入れ 1〜4 の根拠と検証が揃っている。selftest 11 項目が両構成で PASS、Runtime の dump 2 本 (キー無しで occluded 2867 / false で occluded 0)、`git diff -- assets` は空、shot_verify 30 枚 PASS、check_rules は 0 error。
  - 不安・質問への回答: (a) `--no-occlusion` で起動したエディタでメニューを操作すると、メニューで選んだ値が保存される。この挙動は妥当とする。メニュー操作はユーザーの明示的な選択で、CLI は起動中の上書きにすぎず、保存しない。この優先順位を ADR-029 / engine_spec に書く (sub-08)。(b) 注記をツールチップにしたのは可。spec §4.3 の「注記を付ける」を満たしている。
  - should (申し送り): CLI と組み合わせる selftest は、EngineLoop の式 (`Load && cli`) をテストの中に書き写しているので、本体の配線が変わっても検出できない。`ResolveOcclusionCulling(assetsRoot, cliFlag)` のような関数に出して本体と selftest で共有するのが望ましい。今回は dump 2 本で配線を確かめたので差し戻さない。
  - nit (申し送り): `TagNames.h` の冒頭コメントが、描画設定 (rendering.occlusionCulling) も置いていることを反映していない。エディタのメニューからの保存経路 (`scmhint::Changed` を含む) とツールチップは実走・目視していない (ユーザーの目視待ち)。
