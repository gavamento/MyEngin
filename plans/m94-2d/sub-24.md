# sub-24: 文書 (engine_spec / ADR-033 / test_checklists / README)

- 依存: sub-23
- 状態: 実装中
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む** (第 1 章の文書規約、第 8 章)。spec: 5. 40

## やること
1. `engine_spec.md`:
   - §6.15 (新設) 「2D rendering (M94)」: Sprite2D / 整数ソートキー / ソート層 (安定 id、上限 32) / SortingGroup / SpriteMask (ステンシル) / Sliced・Tiled / Light2D のライトバッファと層ごとの群 / ShadowCaster2D (押し出し) / 法線 / Parallax2D / Line2D / SpriteShape2D / PixelPerfectCamera2D (式、TAA・FXAA 無効、ポストは低解像度、UI はフル解像度) / サーフェスシェーダの枝。英語 (既存の節に合わせる)。
   - §10.13 (新設) 「2D assets (M94)」: `.meta` の sprites (サブアセット `#sprite<id>`)、`.spriteanim.json`、`.tilepalette.json` / `.tilemap.json` (セルのビット配置、チャンク、override 表 = sim 状態、presence gate)、`.shape2d.json`、`.spriteshape.json`。
   - §10.5 の末尾に「2D joints = plane2D + Hinge(Z) / Slider / Fixed / SpringJoint」の 1 段落、`Effector2D`。
   - 平面拘束 (`plane2D`) の節 (§10.4 か §10.5 の近く): 拘束点 A/B/D、不変量、z は初期値、異なる z は当たらない、`oneWay` の規則、CC の 2D モード (移動床は kinematic Rigidbody の velocity)。
   - §10.9 の近くに `GridAgent2D` (A* の辺とコスト、4096 展開上限、BT MoveTo の fallback)、Perception の `forwardAxis`。
   - §11.3 の replay ジョブ表に `2d`、§11 の golden 一覧に `2d` / `2d_lights`。
   - §12.2 の表に M94 の行、Scripting 節に ABI の版と 10 本。
2. `docs\adr\ADR-033-2d-lane.md`: 文脈 / 決定 (平面拘束の 3D ソルバ流用、整数ソートキー、アセット保存 + 差分 sim 状態、ライトバッファ乗算 + 押し出し影、ピクセルパーフェクトの RT、スプライト列を Animator のステートに、ソート層の安定 id、ABI 1 回の位置) / 却下案と理由 (spec 2. の表から) / 結果 (sub-01 の不変量、sub-11 の計測値、sub-23 の性能) / 参照 (Unity の URL)。`engine_spec.md` §13 の ADR 一覧に追加。
3. `docs\test_checklists.md` の M94 節 (sub-23 の下書きを仕上げる): 手動確認 (Scene View 2D モードの操作感、スプライトエディタ、タイルパレット、ピクセルパーフェクトの見え方、ライトの見え方)。
4. `README.md` の機能表に 2D レーンの 1 行。`plans\m75-ugui.md` の M75h の注記に「M94n が v31 を使った場合は v32」(実際の版で)。
5. `plans\m94-2d\harness.md` の申し送り欄に残り minor と未確認事項 (司会が仕上げるが、coder が下書き)。
5b. (各サブの申し送りの集約) engine_spec の「2D の v1 の限界」欄に、spec 7. R9〜R12 と spec 4.1.x の各「確定事項」に書いた限界 (親付き plane2D / 複合子の oneWay・多角形 / 回転床 / BoxCast の近似 / タイル絞り込みの余裕 / マスクの効き方 / 影・法線・Freeform / SpriteShape の height / GridAgent の非タイル足場 / Effector の領域内は眠らない / froxel は 2D 経路に効かない 等) を 1 か所にまとめる。`docs\adr\ADR-032-engine-mcp-server.md` の「固定 21 ジョブ」を 22 (2d 追加) に直す。engine_spec に `--2d-demo` / `--2d-night` / `--2d-stress` と性能値 (デモ 0.9〜1.4 ms/フレーム、stress 4.4〜4.6 ms、sim 0.21 ms/tick、Raycast 12.7 us / BoxCast 67.5 us) を書く。`docs\test_checklists.md` の M94 節は、各 VERDICT で「ユーザー目視」に回した GUI 操作 (sub-02〜22 の nit) を全部拾う。
6. 文書のみの変更: ビルドは原則不要 (AGENTS.md 7)。参照先の存在と行番号 / 名前の整合を確認する。

## やらないこと (このサブでは)
- コードの変更 (文書で見つかった不整合は「仕様との差分」に出す)

## 触る場所 (planner の見立て)
- `engine_spec.md` (§6, §10, §11, §12, §13)、新規 `docs\adr\ADR-033-2d-lane.md`、`docs\test_checklists.md`、`README.md`、`plans\m75-ugui.md`、`plans\m94-2d\harness.md`

## 受け入れ条件 (このサブ)
1. (spec 40) 上の全箇所が書かれ、engine_spec の節番号・ADR 番号・ABI 版・TypeId・snapshot 版が**実際のコード**と一致 (grep で確認したログ)。
2. `pwsh tools\check_rules.ps1` 0 (規則 14 の改行など)。
3. 文書内のリンク先が存在する。
4. **最終チェックポイント (spec 4-5)**: sub-23 の完了後にソースが 1 行も変わっていなければ (`git diff <sub-23 のコミット> -- src assets tools` が空)、sub-23 のチェックポイント結果をそのまま流用してよい (SELF_EVAL にその旨と diff の空を書く)。変わっていれば `Editor.exe --selftest` 全体 / `tools\replay_verify.bat` / `tools\shot_verify.bat` を回す。

## 検証コマンド
- `pwsh tools\check_rules.ps1`
- `git diff --stat` で文書だけが変わっていること (= 最終チェックポイントは sub-23 の結果を流用)
- ソースが変わっていた場合だけ: `Editor.exe --selftest` 全体 (Debug / Release)、`tools\replay_verify.bat`、`tools\shot_verify.bat`

## 実装メモ (coder が追記)

(round 1 の SELF_EVAL。coder がこの節へ写さなかったため司会が転記した)

SELF_EVAL: sub-24 (round 1)
実装:
- C:\HAL\MyEngin\engine_spec.md:
  - §6.15 新設「2D rendering (M94)」。英語。内容は、カメラと Scene View 2D、Sprite2D と整数ソートキー、ソート層(上限32)、SortingGroup、SpriteMask(ステンシル)、Sliced/Tiled、色の契約、Light2D(ライトバッファ、presence gate)、ShadowCaster2D、法線、Parallax2D、Line2D、SpriteShape2D、サーフェスシェーダ枝、PixelPerfectCamera2D(式、TAA/FXAA 無効、ポストは低解像度、UI はフル解像度)。
  - §10.5 末尾に「2D physics on the 3D solver (M94)」を追加。内容は、plane2D の A/B/D の3点拘束と不変量、z0 保持と異なる z は当たらない点、oneWay の規則、CC 2D モード(移動床は kinematic の velocity)、2D 形状、2D ジョイント1段落、Effector2D、クエリと BoxCast の近似。
  - §10.9 末尾に GridAgent2D(A* の辺とコスト、4096 展開上限と部分経路、BT MoveTo の fallback)を追加。§10.10 に Perception の forwardAxis を追加。
  - §10.13 新設。アセット表、.meta sprites、スプライトアニメ、タイルのビット配置、チャンク、override 表(sim 状態、TM2D 節、presence gate)、タイル衝突、タイルパレット窓、shape2d と spriteshape、Camera2DFollow、ABI v31=188 の10本、TypeId 81〜93、snapshot v65、デモと検証、性能値を書いた。
  - §10.13 末尾に「v1 limits of the 2D lane (consolidated)」を置き、R9〜R12 と各サブの限界を集約した。
  - §11.3 に `2d` ジョブの bullet と、golden `2d`/`2d_lights`(32枚)の記述を追加。
  - §12.2 に M94 の行を追加し、見出しを (M0-M91, M94) にした。§12.3 の採番行を M94 時点の値に更新。§13 の ADR 一覧に ADR-033 を追加。
- C:\HAL\MyEngin\docs\adr\ADR-033-2d-lane.md 新規。構成は 文脈 / 決定10項目 / 却下案の表 / 結果 / 参照(Unity の URL)。
- C:\HAL\MyEngin\docs\adr\ADR-032-engine-mcp-server.md: 「固定 21 ジョブ」を 22 に修正した。
- C:\HAL\MyEngin\docs\test_checklists.md: M94 節を下書きから仕上げた。台帳の sub-02〜22 の「ユーザー目視」申し送りを拾って追加した (一部は要約で、各 VERDICT 全文との突き合わせはしていない)。
- C:\HAL\MyEngin\README.md: 物理・音響・AI の表に 2D の1行、ショーケース表に `--2d-demo`(`--2d-night`/`--2d-stress`)を追加。古かった ABI 記述(v29/174)を v31/188 に直した。
- C:\HAL\MyEngin\plans\m75-ugui.md: M75h の版注記を更新した(ABI は v32 以降、TypeId は着手時の末尾から、snapshot v65、ADR-033 まで使用済み)。
- C:\HAL\MyEngin\plans\m94-2d\harness.md: 申し送り欄に coder 下書きを追記した (→ 役の境界外。司会が戻し、司会の文で台帳に書き直した)。

仕様との差分:
- [追加] README の ABI 記述(v29/174 → v31/188)を直した。古い値のまま 2D の行を足すと矛盾するため。
- [追加] engine_spec §12.3 の採番行を M94 時点の値に更新した。
- [未実装] §12.2 に M92/M93 の行は無いまま。既存の欠落で M94 の範囲外。
- [逸脱] 平面拘束の節を §10.4 でなく §10.5 末尾の「2D physics (M94)」にまとめた。spec の「§10.4 か §10.5 の近く」の範囲内。
- [追加] §10.13 内の限界欄に「プレイ用の合成入力が左へ偏る」を書いた。
- 文書内の数値はコード側で grep 確認した (MYE_API_VERSION=31、kSimSnapshotVersion=65、kDocVersion=4、kCookVersion=6、スロット表 31=188、TypeId 81〜93、シェーダファイル、kOneWaySlop 等)。挙動の記述は spec.md と各サブの確定事項からの転記。

検証:
- `pwsh tools\check_rules.ps1` → 3 error / 50 warning (基準どおり)。
- `git diff --stat -- src assets tools` → 空。最終チェックポイントは sub-23 のチェックポイント 5 を流用 (ログ C:\HAL\MyEngin\cache\m94\ の cp5 一式)。
- 改行: 編集した全ファイルが LF。ADR-033 内のリンク 6 本の実在を確認。
- 未実行: エンジンのビルド、selftest、replay_verify、shot_verify (文書のみ、AGENTS.md 7 章)。

自己採点 (1-5):
  仕様適合: 4 — やること 1〜5b、受け入れ条件 1〜4 を全て書いた。
  正しさ: 3 — 版・TypeId・スロット数・ファイル名はコードで確認、挙動の記述は転記でコードとの再突き合わせは一部のみ。
  コード品質: 4 — 既存の節の文体に合わせ、限界は 1 か所に集約。
  テスト: 3 — check_rules と差分空の確認まで、ビルドや selftest は sub-23 流用。

不安・質問:
- engine_spec §10.13 などが spec.md 4.1.x と食い違わないかを reviewer に重点確認してほしい。
- harness.md の下書きの「cache\m94\ を掃除してよい」は削除の依頼ではない (未実施、消すならユーザー許可が要る)。
- check_rules の既存 3 error (Q-9) は未着手。

触ったファイル: engine_spec.md、docs\adr\ADR-033-2d-lane.md (新規)、docs\adr\ADR-032-engine-mcp-server.md、docs\test_checklists.md、README.md、plans\m75-ugui.md、plans\m94-2d\harness.md

申し送り: 最終チェックポイントは sub-23 流用 (受け入れ条件 4)。Q-9 は M94 の外の別件。コミット件名候補 `M94x: 2D レーンの ADR-033 と engine_spec / 検証表 / README を更新する`。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-22)。`git diff --stat -- src assets tools` が空 (文書のみ) を確認し、受け入れ条件 4 により最終チェックポイントは sub-23 のチェックポイント 5 を流用。engine_spec の抜き取り (PixelPerfect の RT = 画面を覆う最小の偶数、Effector の起床条件 4 種、SpriteMask の効き方、Camera2DFollow の境界) が spec 4.1.x と一致。check_rules 3 / 50 で増減なし。should: SELF_EVAL がこの節に写されておらず、coder が harness.md (司会の台帳) に書いた — 役の境界外なので司会が扱う。nit: 限界欄 §10.13 の Effector の行を一般化。
