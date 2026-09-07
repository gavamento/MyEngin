# review-1: m67h-restir-finalize

- 日付: 2026-09-08
- 対象コミット範囲: `9a893cdeb1095b113c99b03b1af925afe1e36e3c..HEAD`
  - `6c8a1b5` M67h: レビュー minor 5 件の回収 — 触れないチューニング UI と黙って落ちる reflectionClass
  - `30ce7ab` M67i: ReSTIR の既定表を 2 軸で測る — 4 行は規則で維持、Hero だけ M 上限を 16 へ

```
REVIEW: PASS
round: 1
軸 (1-5):
  製品の深度: 4 — 受け入れ条件の外側を 6 通り突いて破綻しなかった: (1) `.mat.json` の
    `null` / `[3]` / `{}` / `-0.0` / `3.0e10` / `true` の 9 入力が例外なしで 4 + 警告 1 行、
    欠損は無言 (実 run 24 本の全ログで `reflectionClass` の警告 0 行)。
    (2) クライアント 861 px (= 1400x900 窓) だけでなく **1280x720 でも** RT Debug →
    ReSTIR の調整 → クラス表の 3 段が全部収まる (g\21_720_tuning.png / g\23_720_classtable.png)。
    (3) 4 階層目のスライダを実際にドラッグして Default の M 上限 16 → 1 が**その場で絵に届き**
    (g\09_after_drag.png)、Prop の緑は残る = クラスの意味論が生きている。
    (4) `Reset to defaults` で 16 / 16 / 24 / 32 / 16 に戻る = 既定の出所が定数表のまま
    (g\11_after_reset_table.png)。(5) golden の差分と debug 12 / 14 の再測が coder の数値と一致。
    (6) Hero=16 は「却下した測定値ごと」`RtTypes.h:233-247` と ADR に残っており 8 へ戻せる。
    5 にしないのは、**このマイルストーンが潰そうとした「次の人を誤らせる記述」がマイルストーン
    自身の周辺に 4 か所残っている**ため (指摘 1。うち 1 か所は本件が意図的に避けた `--update` を
    次の人に指示している)。
  機能性: 5 — 受け入れ条件 A1〜A19 を自分で再実行して全て緑。golden 24 枚 maxDiff=0 /
    `--selftest` exit 0 (`heroIsMinCap` PASS を含む) / `check_rules` 0/0 / `replay_verify` 10 ジョブ /
    Debug・Release とも `/p:MyeWarnAsError=true` で警告 0。A17 / A18 は coder の数値を鵜呑みにせず
    別プロセスで撮り直して一致を確認した (下記)。回帰は 1 件も見つからなかった。
  ビジュアルデザイン: 5 — 実機 1400x861 で、ReSTIR トグル off + RT Debug 12 表示中でも
    チューニング UI が**灰色でない** (g\06_tuning_zoom.png: 全項目が通常色、意図的に
    TextDisabled なのは GPU 時間の 1 行だけ)、`Reset to defaults` と `restir 0.015 ms` が
    両方見える。クラス表は 5 行 × 3 スライダが 1 枚に収まり Hero の M 上限が 16 と読める
    (g\07_hero_zoom.png)。ドラッグ → 絵の変化 → Reset の往復も成立。
  コード品質: 4 — 受理規則が本当に 1 本 (`reflectionClass` を JSON から読む箇所は
    `GpuResources.cpp:1022` と `InspectorWindow.cpp:1481` の 2 つだけで、両方が
    `ParseReflectionClassJson` を呼ぶ)。CB の枠は「なぜ外したか」を旧名つきで残しつつ
    宣言・読み書き 0 件、`sizeof 240` / `offsetof 160` は静的検査で門番。コメントは日本語で
    「なぜ」中心。仕様との差分に出ていない変更は 1 つも無い (diff 全読)。
    5 にしないのは、ADR に**再現しない実測値が 1 つ**残っていること (指摘 2)。

指摘:
  1. [minor] 宛先: coder — **完了した M67h を「これからやる」と書いた記述が 4 か所残り、うち 1 か所は
     本マイルストーンが意図的に避けた手順を次の人に指示している。** —
     根拠: `tools\shot_verify.bat:378` = 「M67h でユーザーの確定パラメータを焼いたら、この 1 枚は
     **--update で撮り直す**」。実際には spec §4.6 / sub-02 r2-3 に従って `--update` を使わず
     比較 run の実物を 1 枚コピーしており (`git diff --stat 9a893cd..HEAD -- tests/` =
     `demo_render_rtrefl_restir.png` の 1 ファイルのみ)、この行だけが逆の作法を指している。
     coder は同ファイルの `:368` を確認済みと報告しているが 10 行下は見ていない。
     同種の残り 3 か所: `src\Editor\EditorApp.cpp:1262`「★ここで確定した値は後続 M67h が定数表へ
     焼く (spec §4.6)」(**coder が 3 行下の :1265 を書き換えた同じブロック**)、
     `src\Engine\Renderer\RayTracing\RtTypes.h:276`「S5 / M67h で再評価できるようにしておく」、
     `CLAUDE.md:109`「S5 / M67h で既定を on へ反転したときに」 —
     期待: 少なくとも `shot_verify.bat:378` を実態へ (「M67i で 1 枚差し替え済み。次も `--update`
     ではなく比較 run の実物をコピーする」)。残り 3 か所は「M67h で決着済み (ADR-016 S5)」の 1 行で足りる。

  2. [minor] 宛先: coder — **ADR-016 の「残る 1 画素は未特定 (最寄り Hero 画素まで 29 px = A-Trous の
     足跡 ±12 px の外)」が再現しない。実測では 10 px = 足跡の内側で、未特定の宿題は存在しない。** —
     根拠: `docs\adr\ADR-016-restir-reflection.md:495-498` と `engine_spec.md:518` の「99.6%」。
     再測手順 (すべて本レビューで実行):
     (a) `git show 9a893cd:tests/golden/demo_render_rtrefl_restir.png` と HEAD 版の画素差 =
         **maxDiff 5 / 233 px / 最悪画素 (367,372)** — ADR の数値と一致。
     (b) 現行 Release で
         `bin\x64\Release\Runtime.exe --render-demo --deferred --rt-refl --rt-restir --rt-debug 14
          --warp --no-audio --font-embedded --width 960 --height 540 --no-fxaa --frames 41
          --shot-frame 40 --screenshot tests\actual\rv_d14_f40.png`
         → Hero 色 (231,147,147) が **400 px** (ADR の「384 → 400」と一致)。
     (c) 233 画素から (b) の Hero マスクへのチェビシェフ距離 = **中央値 1 / p95 4 / max 10**。
         8 px を超えるのは `(621,225)` の 1 画素だけで、チャンネル差は **1** (R 44 → 45)。
     ADR は距離を「**旧 ∪ 新**の Hero マスク」で測ったと書いており、それは (b) の新マスクの
     上位集合なので**距離は私の値以下にしかならない** = 29 px は原理的に再現しない。
     結果として「±12 px の外」という結論も成立せず、**233 / 233 が足跡の内側**になる —
     期待: ADR:495-498 と engine_spec:518 の数値を再測値へ。planner が「原因は追跡しない」と
     裁定した点は蒸し返さない (むしろ再測すると追跡対象そのものが消える)。

  3. [minor] 宛先: planner — **`engine_spec.md:1947-1948` の枚数更新で、文の主語と数が食い違った。** —
     根拠: 該当文は "adding the field left every replay pair and golden image **that predates it**
     bit-identical (seven pairs and **twenty-four** images today)"。`ui_probe_720p` /
     `ui_probe_16x10` は M70b (`fcef43f`) の追加で**音響の場 (M65a) より後**なので predate して
     いない (predate するのは 22 枚)。同じ主張を書いた `:1894` は "all twenty-two golden images …
     across M68" のまま正しく史実として保護されており、この 1 行だけ主語と数がずれた。
     spec §2 S11 が `:1926` (現行 :1947) を「現在形」に分類したことから来ているので宛先は planner —
     期待: 「(seven pairs and twenty-two images that existed then)」のように主語と数を一致させるか、
     数が現在の総数であることを文面で明示する。どちらでも S11 の趣旨 (史実を機械置換しない) は保てる。

検証した手段:
  - 読んだ範囲: `spec.md` 全文 / `sub-01.md` 全文 / `sub-02.md` 全文 (844 行) / `harness.md` /
    `9a893cd..HEAD` の diff 全ファイル (src・assets・build・docs・engine_spec・plans) /
    `docs\adr\ADR-016-restir-reflection.md` 全文 / `engine_spec.md` §6.4 と §11.3 の該当節 /
    `tools\shot_verify.bat` の RT 節 / `README.md` の該当行 / `plans\m67-restir-reflection\review-1.md`
  - ビルド: MSBuild Debug / Release とも `/p:MyeWarnAsError=true` → exit 0、`warning` 0 行 (A15)
  - `bin\x64\Debug\Editor.exe --selftest` → **exit 0**。`PASS: heroIsMinCap` (A16) /
    `PASS: material: reflectionClass judges 'non-integer' by value, so 3.0 reads as 3` (A2) /
    落とした 9 入力に対する `[WARN ] material: reflectionClass=… is not a whole number in [0,5);
    using 4 (Default)` が**ちょうど 9 行** (A4)
  - `pwsh -File tools\check_rules.ps1` → 0 error / 0 warning
  - `tools\shot_verify.bat` → **[PASS] 24 shots**、`maxDiff=0` が 24 本 / 24 本、`git status` は
    `tests\golden\` に変更なし (A1)。差し替えた 1 枚も現行ビルドの実 run とビット一致 (A12 相当)
  - `tools\replay_verify.bat` → exit 0 / `[parallel] all 10 jobs passed in 181.9s` /
    `[PASS] replay consistency … + snapshot round-trip + time travel + rule check` (A14)
  - `pwsh -File tools\gen_project_files.ps1` → 再生成しても `git status` に差分が出ない
    (新規 2 ファイルの `.vcxproj` 登録が生成物と一致)
  - grep 検査: `gRsFrameIndex` / `RtRestirCB::frameIndex` の**宣言・読み書きが 0 件** (残るのは
    理由コメント 2 か所のみ = A7) / `reflectionClass` を JSON から読む箇所が 2 つだけで両方が
    共有関数を呼ぶ (A3) / `rtReflRestir` を読むのは `RenderSystem::Render` 1 か所で
    `RtRestirEffective()` 経由 (A5) / 誤読値 `15.65` / `13.02` はリポジトリに 1 か所だけ
    (ADR:539 の**罠の説明**) 残り、ADR / engine_spec の結論値からは消えている
  - 数値の再測 (Python + PIL、画像は `tests\actual\rv_*.png` = gitignore):
    旧 golden (9a893cd) vs 新 golden = maxDiff 5 / 233 px / 最悪画素 (367,372) →
    ADR の A18 と一致。`--rt-debug 14` の Hero = 400 px (ADR「384 → 400」と一致)。
    `--rt-debug 12` を 8bit 生値で読むと **Hero マスク = R/G とも 231 飽和 (= M 16)**、
    Default = 同じく 16、Prop = G 飽和 / R 中央値 198 (= M > 16) → **A17 の「Hero だけ動いて
    Default / Prop は動かない」を出荷構成で独立に再現**
  - 実機 (Release `Editor.exe --render-demo --deferred --rt-debug 12 --frames 100000000`、
    Win32 の自動操作。**`--rt-restir` を渡していない** = トグル off のまま debug 12 が
    ReSTIR を強制する条件で検査):
    `g\05_rtdebug.png` (RT Debug メニューに `ReSTIR Reflection` の**チェックが無い**ことを確認) /
    `g\06_tuning.png` + `g\06_tuning_zoom.png` (A5 / A6: 全項目が通常色、`Reset to defaults` と
    `restir 0.015 ms` が 1400x861 で可視) / `g\07_classtable.png` + `g\07_hero_zoom.png`
    (4 階層目のクラス表が全 20 行収まり Hero の M 上限が 16) / `g\09_after_drag.png`
    (4 階層目でドラッグが成立し Default の M 上限 16 → 1 が即座に絵へ = 黄 → 橙、Prop の緑は不変) /
    `g\10_after_reset_scene.png` + `g\11_after_reset_table.png` (Reset で表と絵が戻る) /
    `g\20_720_rtdebug.png` / `g\21_720_tuning.png` / `g\23_720_classtable.png`
    (**1280x720 でも 3 段とも収まる** = 受け入れ条件の外側)
    ※ 画像は `%TEMP%\claude\C--HAL-MyEngin\56078a7c-…\scratchpad\g\` (リポジトリ外)。
    検査後に Editor を終了し `git status` がレビュー前と同じ (台帳 1 ファイルのみ) であることを確認済み。
  - 見た画像: 上記 `g\*.png` 12 枚 / `tests\actual\rv_d12_f40.png` / `rv_d14_f40.png` /
    新旧 `demo_render_rtrefl_restir.png`
前回指摘の消込: (round 1 のため無し)
```

## 補足 (指摘に落とさなかった確認事項)

- **司会が名指しした 4 点はすべて満たされている**:
  - Hero=16 は「却下された測定値ごと」`RtTypes.h:233-247` / `RtSelfTest.cpp:1180-1191` /
    ADR-016 の「行への割り当て」表 (規則の判定と最終値の 2 列) に残っており、8 へ戻す根拠が
    コメントだけで揃う。`heroIsMinCap` は `<=` なので 8 へ戻しても検査は通る。
  - round 1 の「1 行も変えない / 全 5 行維持」という記述は ADR・engine_spec・`RtTypes.h`・
    `RtSelfTest.cpp` のいずれにも残っていない (grep で確認)。残っているのは
    `plans\…\sub-02.md` の round 1 実装メモ = 記録として意図的に残す部分だけ。
  - 誤読値 15.65 / 13.02 は ADR の**罠の説明** 1 か所だけ (ADR:539)。結論値は 16.00 / 30.98 に
    訂正済み。
  - 枚数は `shot_verify.bat` の実測 (`call :shot` 24 本、`MYE_SHOT_SKIP_*` で 10 本除外 = CI 14 本) と
    `engine_spec.md:2020/2029/2031` / `CLAUDE.md` / `README.md:315` が一致。史実の `:1894`
    (twenty-two / across M68) と `:2063` (`that existed then` を補った側) は正しく保護されている。
    唯一の齟齬が指摘 3。
- planner が事前に裁定済みとされた 5 件 (sub-01 の未実行 2 件 / round 3 の自己採点 /
  `RtRestirCB` の `pad0`・`pad1` の並び / 外れ 1 画素の**追跡**の是非 / 3 クラス同挙動を
  機械固定しないこと) は、いずれも蒸し返していない。指摘 2 は「追跡すべきか」ではなく
  「記録された実測値が再現しないので数値を直す」という別の論点。
- `.mat.json` は cooked blob に入らない (`ModelCookData::AddMaterial` の呼び出しは
  `FbxLoader.cpp:400,523` / `ModelLoader.cpp:174` のモデル取り込みのみ) = `kCookVersion` 据え置きは正しい。
  `Material` はワールドハッシュに載らない (`WorldHasher` に material の語が 0 件) =
  `replay_verify` 全緑と整合。
