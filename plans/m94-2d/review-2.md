# review-2: m94-2d

- 対象: `4664373..0b02303` (63d8661 review-1 の記録と spec 修正、cf5f298 sub-25 の修正本体、0b02303 台帳)。回帰の確認には `25fb355..0b02303` 全体の検証ログを使った
- 日付: 2026-10-22
- reviewer は読み取りと実行だけを行った。ソース・仕様・生成物 (cache\ 配下を含む) は変更・削除していない

```
REVIEW: PASS
round: 2
軸 (1-5):
  製品の深度: 4 — 上昇床の一方通行 (1 m/s と 4 m/s で CC と箱が 120 tick 床の上に留まる) が selftest で固定された。スプライト材質ありのデバイス消失は修正前後のログで復旧を確認した (修正前は外部参照 3 で打ち切り、修正後は 0 で復旧)。傾いた oneWay 床は v1 の対象外と決まり、限界欄に書かれた。減点はデバイス消失の材質経路がログ証跡だけで、自動テストになっていないこと (sub-25 の未実施に記録済み)
  機能性: 5 — チェックポイント 6 の結果: selftest Debug / Release とも PASS 8770・FAIL 0・d3d ERROR 0、replay_verify 22 ジョブ中 21 が個別 PASS (rules は既存 3 error = Q-9 だけ)、全体の d3d ERROR 0 件 (cp5 は 196)、shot_verify 32 枚 PASS (--update 不使用)、build_managed Release 成功。追加の受け入れ条件 41〜45 は 41・42・43・44 が証拠つきで満たされた。45 は条件の選択肢のうち「ログ 1 回」で満たした (zoom 1 でも「TAA / FXAA are turned off」が出る)
  ビジュアルデザイン: 4 — 状態復元の修正後も既存 golden と 2d / 2d_lights が tol 0 で不変 (shot_verify_cp6.log)。zoom 1 / zoom 3 の撮影ログ (cache\m94\s25\pp_zoom1 / pp_zoom3) は PixelPerfect の停止メッセージどおり。TAA を掛けたときの画素比較は未実施 (sub-25 の未実施に記録済み)
  コード品質: 4 — 修正は最小で、根本原因に当たっている。ReleaseGpu の 3 本の Reset、s1 の常時バインド、RS / Blend / DS の控えと復元 (早期 return は控える前の 683 行だけなので漏れない)、OneWayFloorShift で天面の時刻を tick 開始にそろえる (rb の無い静的床はシフト 0 = 従来どおり)。ABI と版は不変で、焼き値も不動。spec に残る小さな不整合 2 件は minor
指摘:
  1. [minor] 宛先: planner — #8 の書き換え漏れが 1 か所ある。spec.md:219 (4.1.12) に「RT の寸法が W/zoom/2*2 なので … 奇数の端数は RT 側を 1 px 大きく取り、blit で端を切る」が注記なしで残っている。2. #13 (:34) には「→ sub-22 で変更」と注記されたが、こちらは確定値 (:221 の「画面を覆う最小の偶数」) と矛盾したままになっている。round 1 の #8 で列挙した :34 と同じ内容が 4.1.12 にも重複していたのを、reviewer が round 1 で挙げ漏らした (検索語を 2. #13 の書き方に合わせたため) — 根拠: `sed -n 219p plans/m94-2d/spec.md` — 期待: 同じ「→ sub-22 で変更」の注記を足す
  2. [minor] 宛先: planner — spec.md:397 の 6. サブ分割表で、sub-24 の行の「コミット件名候補」列が消えている (4 セルしか無く、Markdown の表として 1 列欠ける)。63d8661 で sub-25 の行を足したときに失われた — 根拠: `git diff 4664373 0b02303 -- plans/m94-2d/spec.md` の該当行 (`| 1-5, 40 | \`M94x: …\`` が `| 1-5, 40 |` になっている) — 期待: 件名候補のセルを戻す
検証した手段:
  - 読んだ範囲: cf5f298 の全 diff (Sprite2DPass.cpp / ShaderManager.cpp / PhysicsSystem.cpp / RenderSystem.cpp / PhysicsSelfTest.cpp / engine_spec.md / test_checklists.md)、63d8661 で変わった spec.md、plans\m94-2d\sub-25.md。Sprite2DPass::Render の return は 683 (状態を控える前) と 898 (復元の後) の 2 か所だけと確認した
  - チェックポイント 6 のログ: cache\m94\replay_verify_cp6.log (22 ジョブの done 行、`[ERROR] [d3d]` 0 件、rules は EditorMain.cpp の #ifdef _DEBUG と engine_dist の .bat 2 本 = 既存 3 error)、selftest_full_cp6_{Debug,Release}.log (PASS 8770 / FAIL 0、rising oneWay の 6 項目が両構成で PASS し、位置ログも同値)、shot_verify_cp6.log (32 shots PASS)、build_managed_cp6.log
  - 再現ログ: cache\m94\s25\dl_before (外部参照 3 で fatal)、dl_after (0 で復旧、world hash unchanged=1)、pp_zoom1 (zoom 1 でも TAA / FXAA 停止のログ)、pp_zoom3。修正前の再現に使った DemoContent への一時的な材質付けは、コミットにも作業ツリーにも残っていない (git status clean、cf5f298 に DemoContent は含まれない)
  - 文書: engine_spec.md の制御文字は 0 件 (3026 行は正しいパス)、2930 行の限界欄は一般化済み、test_checklists の 4 行を追加済み
  - 今回は自分で exe を実行していない。依頼に従い、生成物 (cache\2d_showcase.scene.json など) を消す操作を避けたため。チェックポイント 6 のログと s25 の再現ログを証拠として採用した
  - 未検証: TAA を掛けた画素比較、スプライト材質ありのデバイス消失の自動テスト化、GUI の実操作、実 GPU
前回指摘の消込:
  1. 解消 — s1 を bindSpriteState で常に張るようにした (Sprite2DPass.cpp:775-776)。replay_verify_cp6.log の d3d ERROR は 0 件 (cp5 は 196)
  2. 解消 — ReleaseGpu に spriteVS / spritePS / spriteInputLayout の Reset を追加 (ShaderManager.cpp:547-549)。s25\dl_before は外部参照 3 で打ち切り、dl_after は 0 で復旧した。3 = VS + PS + レイアウトの 3 本で、原因と数が合う
  3. 解消 — engine_spec.md:3026 が `tools\mcp\src\verify.rs` に直り、制御文字は 0 件。sub-20.md の制御文字も合わせて直されている
  4. 解消 — engine_spec.md:2930 が「a plane2D body in an active region never sleeps」になった
  5. 解消 — OneWayFloorShift で天面を床の tick 開始時の姿勢へ戻した (PhysicsSystem.cpp:342-347, :372, :1400、startPy は :3200)。PhysicsSelfTest の CC2D (10) は 4 m/s で修正前 FAIL → 修正後 Debug / Release で PASS。既存の焼き値は不動
  6. 解消 — v1 は水平な oneWay 床だけと決めた (Q-19)。spec 2. #9 を 4.1.5 にそろえ、engine_spec の限界欄に「傾いた・回転した oneWay 床は対象外」と書いた
  7. 解消 — test_checklists.md に 4 行 (Light2D の Inspector、forwardAxis の XY の扇形、SpriteShape2D の透視カメラ / 親、タイルパレットのキー U) を追加した
  8. 一部解消 — :34 / :180 / :219 の pixelSnapping / 4.2.3 / 4.3 / 5. 条件 5 は注記・書き換え済み。同じ 4.1.12 の段落に RT 式の古い記述が 1 つ残っている → 今回の指摘 1 (minor)
  9. 解消 — 開始時に RS / Blend (factor・sample mask 込み) / DS (stencil ref 込み) を控え、終了時に必ず戻す。golden 32 枚が tol 0 で不変
  10. 解消 — spec 4.1.12 を「PixelPerfectCamera2D がある間は常に TAA / FXAA を止める」に決め、実装も f.pixelPerfect = true (ppPlan.active) にそろえた。zoom 1 のログで確認
```
