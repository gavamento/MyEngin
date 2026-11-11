# sub-09: 文書と通し検証

- 依存: sub-08、sub-04、sub-05 (BVH のサブを足した場合はそれも)
- 状態: 未着手
- 往復: 0

## やること
spec 受け入れ条件 8, 12, 13, 14。
- `engine_spec.md`: §6.14 に「切り替えとプロジェクト設定」を書き直し (4 種、キー、既定、CLI の優先)、距離・画面サイズ・追加経路・局所影 (「The local-light shadow atlas still sees on-screen casters only.」の文を直す)・デバッグを足し、CLI 表を更新。§6.6 の「No frustum culling」行を消す。機能一覧表 (:430-433 付近) と M99 の行を足す。契約番号 (TypeId / SimSnapshot) の記述を実コードに合わせる。
- `docs\adr\ADR-038-culling.md`: sub-01 の計測節に加え、決定 (spec 2. の D2〜D18 の要点、却下した案と理由、公式情報の URL) を書く。
- `docs\test_checklists.md`: カリングの手動確認項目 (各スイッチ、Scene View のトグル、固定、枠表示、Project Settings の保存)。
- `tools\cull_verify.bat` (新規): `Runtime.exe` で視錐台 ON と `--no-frustum-culling` を撮り、`Editor.exe` の img-diff `--tol 0` で照合する。対象は既定ベンチ・`--culling-demo`・`--culling-2d-demo`・`--particle-demo`・`--2d-demo`・`--render-demo`。撮影条件は `shot_verify.bat` に揃える (`--warp`・`--font-embedded`・`--no-fxaa`、`cache\` の保存済みシーンを先に消す)。`docs\test_checklists.md` と engine_spec の検証表に載せる。CI に載せるかは実行時間を測ってから決め、SELF_EVAL に書く。
- `ResolveOcclusionCulling` (`TagNames.cpp`) は sub-02 から本番経路で使われていない。EngineCliSelfTest の旧検査を `ResolveCullingSettings` 側へ寄せて削除するか、残す理由をコメントに書く。
- 通し検証: `Editor.exe --selftest-full`、`tools\replay_verify.bat`、`tools\shot_verify.bat`、`tools\check_rules.ps1`。既定ベンチの `counts` を M99 前 (基点 `d52e91f3`) と比べ、新しい欄以外の一致を確かめる (受け入れ条件 8)。
- Debug / Release / `--warp` の `counts` 一致 (既定ベンチ、`--culling-demo`、`--culling-2d-demo`)。

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

## フィードバック履歴
