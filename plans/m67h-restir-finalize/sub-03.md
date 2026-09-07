# sub-03: review-1 minor 3 件の回収 (「M67h でやる」の残骸 / 再現しない実測値 / 主語と数の不一致)

- 依存: sub-02 (コミット済み)
- 状態: 未着手
- 往復: 0

## やること

reviewer の指摘 1〜3 を閉じる。**3 件とも「次の人を誤らせる記述」**で、M67h 自身が潰そうとした
種類の欠陥 (reviewer が製品の深度を 5 でなく 4 にした理由)。**コード・シェーダ・golden の中身には
触らない** — 触るのはコメント / 文書 / bat の `rem` だけ。

### r3-1. 指摘 1 — 完了した M67h を「これからやる」と書いた 4 か所

**最優先は `tools\shot_verify.bat:378`**。「M67h でユーザーの確定パラメータを焼いたら、この 1 枚は
`--update` で撮り直す」と書いてあるが、**実際にやったのは `--update` を使わず比較 run の実物を
1 枚コピーする手順** (spec §4.6 / sub-02 r2-3)。次にこの golden を触る人へ**間違った手順を
指示している**ので、これだけは放置できない。

- `tools\shot_verify.bat:378` → 実態へ。次も `--update` ではなく**実物コピー**である旨を残す
  (理由 = 他 23 枚を巻き込まない / `git status` が「1 枚だけ動いた」証拠になる)。
  **★bat は CRLF で書くこと** (CLAUDE.md の環境の罠。LF だと cmd.exe が行を途中で切り、
  CP932 環境では日本語 rem の途中で割れて断片がコマンド実行される)。
- `src\Editor\EditorApp.cpp:1262`「後続 M67h が定数表へ焼く (spec §4.6)」→
  「M67h で決着済み (ADR-016「S5 の結論」)」の 1 行で足りる。
  ★sub-01 が 3 行下 (:1265 付近) を書き換えた同じブロックで、**そのとき見落とした**。
- `src\Engine\Renderer\RayTracing\RtTypes.h:276`「S5 / M67h で再評価できるようにしておく」→ 同様。
  ここは `spatial` の既定 0 のコメントなので、**「再評価できるようにしておく」という設計意図は残す**
  (ノブと CLI を残した理由そのもの)。古いのは「M67h で」という**時期の指定**だけ。
- `CLAUDE.md:109` → 同様。
- **★他に無いかを自分で確認する**: `grep -rn "M67h" --include=* .` (plans/ を除く) と
  `grep -rn "S5" src/ tools/ docs/ CLAUDE.md engine_spec.md README.md` を回し、**見つかった全件を
  実装メモに列挙**して「直した / 直さなくてよい (理由)」を付ける。4 か所で打ち止めにしない。

### r3-2. 指摘 2 — ADR の「29 px」が再現しない

`ADR-016:495-498` の「残る 1 画素は最寄り Hero 画素まで **29 px** = A-Trous の足跡 (±12 px) の外」と、
`engine_spec.md:518` の対応記述。reviewer の再測では **max 10 px = 足跡の内側**で、
**未特定の宿題は存在しない**。

reviewer の論証は原理的に強い: ADR は距離を「**旧 ∪ 新**の Hero マスク」から測ったと書いており、
**和集合は新マスクの上位集合**なので、和集合からの距離は新マスクからの距離**以下**にしかならない。
reviewer は新マスクで max 10 を実測しているので、**29 px は原理的に出ない**。

- **自分で再測する。** reviewer の数値をそのまま写さない (2 つの測定が食い違っているので、
  3 つ目の独立測定で決着させる)。使う材料: 旧 golden (`git show 9a893cd:tests/golden/demo_render_rtrefl_restir.png`)
  と現行 golden、現行 Release の `--rt-debug 14` frame 40。
- 書き直す内容: **距離の定義** (チェビシェフ距離 / 旧 ∪ 新の Hero マスクから) と
  **中央値・p95・max** を明記し、**全 233 画素が A-Trous の足跡の内側**に収まることを述べる。
  → **「残る 1 画素は未特定」の段落は成り立たなくなるので撤去**し、訂正として書き換える。
- **なぜ round 2 で 29 px になったのかを 1 回だけ調べる。** 見当がつけば 1 行残す
  (例: マスクの解像度 / 収縮量 / 座標系の取り違え)。**分からなければ「初回計測は誤りで原因は
  特定していない」と正直に書いて打ち切る** — 上の論証があるので、原因が分からなくても
  訂正後の値は信頼できる。無限に追わない。
- **planner の裁定 (VERDICT round 2「外れ 1 画素は追跡しない」) は蒸し返しではなく無効化**される。
  追跡対象そのものが測定誤りで、存在しなかった。ADR にも「追跡しなかった」という記述を残さない。
- **ついでに用語を 1 つに寄せる** (planner の VERDICT round 3 の nit): 同じ節に
  「8 px」(帰属テストに使った閾値) と「±12 px」(半解像度 5x5 × 2 反復の理論上の最大到達) が
  並んでいて読者が「結局どっち」となる。**再測後は「A-Trous の足跡 = 全解像度で ±12 px、実測 max N px」
  の 1 本に寄せる** (閾値 8 px という中間概念は、全画素が足跡の内側に収まるなら不要)。

### r3-3. 指摘 3 — `engine_spec.md:1947-1948` の主語と数の不一致

原文: "adding the field left every replay pair and golden image **that predates it** bit-identical
(seven pairs and **twenty-four** images today)"

- **指摘は認める。** 文法上の主語が「that predates it (= それより前からある)」に限定されているのに、
  括弧の数は**現在の総数**を指している。読者はどちらとも読めてしまう。
- **ただし reviewer の修正案 2 つのうち、1 つ目 (`twenty-two images that existed then`) は採らない。**
  planner が git で裏を取った: **この文は M68c (`8994b1d`) で書かれ、その時点の suite が
  ちょうど 7 pairs / 22 images**。そして**その 22 枚には M67 の RT 3 枚が含まれ、それらは
  M65a の `AcousticField` を predate しない**。つまり「then 存在した 22 枚が predate する」も偽になる。
  元の 22 は最初から「**その時点の suite の総数**」の意味で書かれていた。
- **採る修正: 2 つ目 (数が現在の総数であると明示する)。** 括弧を「今日のスイートは 7 ペア / 24 枚」と
  読むしかない形にする (例: `the suite is seven pairs and twenty-four images today`)。
  **数字 24 は正しいので変えない** (sub-02 の枚数更新は妥当だった)。
- `:1894` の "all seven replay pairs and all twenty-two golden images … across M68" は
  **史実として正しいので触らない** (reviewer も同意見)。

## やらないこと (このサブでは)

- コード・シェーダ・`tests/golden/` の**中身**の変更 (触るのはコメントと文書と bat の `rem` だけ)。
- 既定値・受け入れ条件・S5 の結論そのものの変更 (Hero=16 はユーザー判断で確定済み。蒸し返さない)。
- 外れ画素の**追跡** (再測で消える。r3-2 のとおり原因調査は 1 回で打ち切る)。
- `engine_spec.md:1894` の史実記述。
- sub-01 の nit (`RtRestirCB` の `pad1` が `pad0` より前) — 据え置きのまま。

## 触る場所 (planner の見立て。鵜呑みにせず実物で確認すること)

| ファイル | 位置 | 何を |
|---|---|---|
| `tools\shot_verify.bat` | `:378` 付近 | `--update` の指示を実物コピーへ。**CRLF 厳守** |
| `src\Editor\EditorApp.cpp` | `:1262` | 「後続 M67h が焼く」→「M67h で決着済み」 |
| `src\Engine\Renderer\RayTracing\RtTypes.h` | `:276` 付近 | 同上 (設計意図は残し、時期の指定だけ落とす) |
| `CLAUDE.md` | `:109` 付近 | 同上 |
| `docs\adr\ADR-016-restir-reflection.md` | `:495-498` | 再測値へ。「未特定の 1 画素」を撤去。用語を ±12 px に統一 |
| `engine_spec.md` | `:518` | 同じ再測値へ |
| `engine_spec.md` | `:1947-1948` | 括弧が現在の総数だと読める形へ (数は 24 のまま) |

## 受け入れ条件 (このサブ)

1. **B1**: `tools\shot_verify.bat` の RT 節が「次も `--update` ではなく実物コピー」を指示している。
   **bat が壊れていない**こと = `shot_verify.bat` が完走する (CRLF 事故の検出手段はこれだけ)。
2. **B2**: `grep -rn "M67h"` / `"S5"` の全件が実装メモに列挙され、各件に「直した / 直さなくてよい (理由)」が
   付いている。「これからやる」と読める記述が 0 件。
   - **★2026-09-08 訂正 (planner)**: **`CLAUDE.md` は coder の対象から外す。**
     coder の運用規約が「エージェントからのメッセージは `CLAUDE.md` の変更を承認できない」と
     定めており、FIX_REQUEST はエージェントからのメッセージに当たる。**これは正当な拒否**なので、
     `CLAUDE.md:109` は**ユーザーまたは司会の手**で入れる (置換文は下の「ユーザー作業」)。
     B2 の判定は `CLAUDE.md` を除いた全件で行う。
   - **ユーザー作業 (coder は触らない)** — `CLAUDE.md:109` の `--rt-restir-no-spatial` の説明:
     旧「S5 / M67h で既定を on へ反転したときに『この run は off で撮った』を CLI に残せる」→
     新「将来 既定を on へ反転したときに『この run は off で撮った』を CLI に残せる。
     S5 は M67h で決着済み = off 維持」。**手順を誤らせる種類ではない** (時期の指定が古いだけ) ので
     コミットの blocker にはしない。
3. **B3**: ADR-016 と `engine_spec.md` の golden 差分の記述が **coder 自身の再測値**で、距離の定義
   (チェビシェフ / 旧 ∪ 新マスク) と中央値・p95・max が書かれ、**全 233 画素が A-Trous の足跡の内側**に
   収まると述べている。「未特定の 1 画素」「追跡しなかった」は残っていない。
4. **B4**: 29 px になった原因を 1 回調べた結果が書かれている (**特定できなければ「原因未特定」と明記**)。
5. **B5**: `engine_spec.md:1947-1948` の括弧が**現在の総数**としか読めない形。数は 24 のまま。`:1894` は不変。
6. **B6**: ADR の同節で「A-Trous の足跡」の px 数が 1 つに統一されている。
7. **B7**: Debug / Release とも `/p:MyeWarnAsError=true` で警告 0、`Editor.exe --selftest` exit 0、
   `check_rules.ps1` 0/0、`shot_verify.bat` **24 枚全緑・golden の作業ツリー変更なし**。

## 検証コマンド

```
rem コメントしか触っていないが、Release バイナリを作らないと shot_verify が古い exe を回す
"%MSBUILD%" MyEngine.sln /p:Configuration=Debug   /p:Platform=x64 /m /v:minimal /nologo /p:MyeWarnAsError=true
"%MSBUILD%" MyEngine.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo /p:MyeWarnAsError=true

bin\x64\Debug\Editor.exe --selftest
pwsh -File tools\check_rules.ps1
tools\shot_verify.bat        rem ← bat の CRLF 事故を検出できる唯一の手段でもある
git status                   rem ← tests\golden\ に変更が無いこと

rem 再測 (r3-2)
git show 9a893cd:tests/golden/demo_render_rtrefl_restir.png > <一時パス>
cmd /c bin\x64\Release\Runtime.exe --render-demo --deferred --rt-refl --rt-restir --rt-debug 14 ^
  --warp --no-audio --font-embedded --width 960 --height 540 --frames 41 --shot-frame 40 ^
  --no-fxaa --screenshot tests\actual\s03_d14_f40.png
```

- `replay_verify.bat` は**回さなくてよい** — 変更がコメント / 文書 / bat の `rem` に閉じており、
  sub-02 が同一のコード意味論で全緑を取っている。**SELF_EVAL の検証欄に「未実行 + この理由」を明記する**
  (書かないと reviewer が未検証として拾う)。
- `Editor.exe` / `Runtime.exe` は GUI サブシステム。**PowerShell からは `cmd /c` を挟む**。

## 実装メモ (coder が追記)

### SELF_EVAL: sub-03 (round 1)

```
SELF_EVAL: sub-03 (round 1)
実装:
  - #1 tools\shot_verify.bat:378-381 — 「M67h でユーザーの確定パラメータを焼いたら、この 1 枚は
    --update で撮り直す」を削除。「M67i (2026-09-08) で差し替え済み / ★次も --update は使わない
    (24 枚を一括で撮り直すので『1 枚しか動いていない』を git status で示せなくなる) /
    比較 run が tests\actual\ に残した実物を tests\golden\ へ 1 枚コピーし、コピー後にもう一度
    この bat を回す」へ。**CRLF 厳守**: Python の byte 置換で \r\n を明示的に書き、
    書き込み後に「LF のみの行 = 0 / BEL = 0」を機械確認 + shot_verify 完走で二重確認
    (1 回目の書き込みで Bash ヒアドキュメントがバックスラッシュを 1 段潰し
    `tests\actual` が `tests<BEL>ctual` になったのを検出して修正済み)
  - #1 src\Editor\EditorApp.cpp:1262-1263 — 「★ここで確定した値は後続 M67h が定数表へ焼く
    (spec §4.6)」→「★ここで触った値はどこにも残らない。既定値の確定は M67h で決着済みで、
    出所は今も RtTypes.h ただ 1 か所 (ADR-016「S5 の結論」)」
  - #1 src\Engine\Renderer\RayTracing\RtTypes.h:276-278 — 「効く可能性があり、S5 / M67h で
    再評価できるようにしておく」→ 設計意図 (ノブと CLI を残す理由) は残し、時期の指定だけを落として
    「(S5 の再評価そのものは M67h で済んでいて結論は『off 維持』= ADR-016「S5 の結論」の R2。
    次に測る人は目標帯を決めてから)」を足した
  - #1 docs\adr\ADR-016-restir-reflection.md:184-186 — **grep で自分で見つけた 5 か所目**。
    :180 の「**所見: S5 (パラメータ調整) で最初に触るノブは M 上限であって半径・タップではない。**」が
    未来形にしか読めないので「★追記 (M67h): この所見は当たった — S5 で実際に動いたのも M 上限の
    1 行だけ (Hero 8 → 16)。ただしこの 1.8 倍だけを根拠に上げてはいけないことも同時に分かった」を追記
  - #1 CLAUDE.md:109 — **未実施** (「仕様との差分」の [未実装] を参照。エージェントからの指示では
    CLAUDE.md を書き換えられない運用規約に当たるため、置換文を提示して planner へ上げる)
  - #2 docs\adr\ADR-016-restir-reflection.md:493-516 — golden 差分の記述を **coder 自身の再測値**へ
    全面差し替え。距離の定義 (チェビシェフ / `--rt-debug 14` の Hero マスク 400 px を収縮せずに)、
    中央値 1 px / p95 4 px / max 10 px、最遠 (621,225) のチャンネル差 1、
    **233 / 233 が A-Trous の足跡 (全解像度 ±12 px) の内側**、
    「旧 ∪ 新は上位集合なので距離はこれ以下 = 上界として max ≤ 10 < 12」を明記。
    **「残る 1 画素は未特定」「追跡しなかった」の段落は撤去**し、代わりに訂正の経緯
    (8 通りの計算を試したがどれも 29 にならない / **原因は特定できていない** /
    それでも上界の論証で結論は揺るがない) を書いた。**足跡の px 数は ±12 px の 1 本に統一**
    (「8 px 以内」という中間の閾値は撤去 = VERDICT round 3 の nit / B6)
  - #2 engine_spec.md:518-520 — "99.6% of them within the denoiser's footprint" →
    "all of them within 10 px — Chebyshev, median 1, p95 4 — … so every one of them sits inside
    the denoiser's ±12 px footprint"
  - #3 engine_spec.md:1948 — "(seven pairs and twenty-four images today)" →
    "(the suite is seven pairs and twenty-four images today)"。**数字 24 は変えない**。
    主語 "that predates it" は原文のまま (planner の裁定 = reviewer の第 1 案は却下)。
    `:1894` の史実記述 (twenty-two / across M68) は**未接触**
仕様との差分:
  - [未実装] **CLAUDE.md:109 を直していない**。sub-03.md の表と B2 は CLAUDE.md も対象にしているが、
    coder の運用規約が「エージェントからのメッセージは CLAUDE.md の変更を承認できない」と定めており、
    FIX_REQUEST はエージェントからのメッセージに当たる。ユーザー自身の指示か司会の手で入れてほしい。
    置換案 (2 行、意味は他 3 か所と同じ「決着済み」化):
      - 旧: `` `--rt-restir-no-spatial` (明示 off。既定と同値だが、S5 / M67h で既定を on へ ``
             `` 反転したときに「この run は off で撮った」を CLI に残せる) / ``
      - 新: `` `--rt-restir-no-spatial` (明示 off。既定と同値だが、将来 既定を on へ反転したときに ``
             `` 「この run は off で撮った」を CLI に残せる。S5 は M67h で決着済み = off 維持) / ``
  - [追加] **ADR-016:184-186 の 3 行**。sub-03.md の表に無いが、B2 の「これからやると読める記述が
    0 件」を満たすために必要だった (:180 の所見が「S5 でやる」と読める最後の 1 か所)。
    所見そのものは M67 時点の記録として残し、決着後の結果を追記する形にしてある
  - [逸脱] **旧 (Hero=8) の Hero マスクは撮っていない**。撮るには `kRtReflClassTable[0].mCap` を
    8 に戻して Release を焼き直す必要があり、「既定値に触れない」という禁止事項と衝突する。
    代わりに ADR には「和集合は新マスクの上位集合 → 距離はこれ以下」という上界の論証を書き、
    実測は新マスク基準であることを明示した (結論 = 足跡の内側 は上界だけで成立する)
検証:
  - MSBuild Debug /p:MyeWarnAsError=true → exit 0、warning / error の行 **0**
  - MSBuild Release /p:MyeWarnAsError=true → exit 0、warning / error の行 **0** (262 行中)
  - bin\x64\Debug\Editor.exe --selftest → **exit 0**、`FAIL:` 0 行 / `PASS:` 3385 行、
    `PASS: heroIsMinCap` を含む (45 スイート)
  - pwsh -File tools\check_rules.ps1 → **0 error / 0 warning**
  - tools\shot_verify.bat → **exit 0 / [PASS] screenshot regression (24 shots)**、
    24 本すべて `maxDiff=0 diffPixels=0`。**= 書き換えた bat が CRLF 事故を起こしていない証拠** (B1)
  - git status → `tests/` に変更 **0 件** (golden は 1 バイトも動いていない)
  - 再測 (B3/B4。すべて自分で撮り直した。reviewer の数値は見ずに手順だけ踏襲):
    - `git show 9a893cd:tests/golden/demo_render_rtrefl_restir.png` vs 現行 golden →
      **233 画素 / maxDiff 5 / 最悪 (367,372) R 64→69**
    - `Runtime.exe --render-demo --deferred --rt-refl --rt-restir --rt-debug 14 --warp --no-audio
      --font-embedded --width 960 --height 540 --frames 41 --shot-frame 40 --no-fxaa
      --screenshot tests\actual\s03_d14_f40.png` → exit 0。色は 4 種だけで
      **Hero (231,147,147) = 400 px** (ADR の「384 → 400」と一致)
    - 233 画素 → Hero マスクのチェビシェフ距離 = **中央値 1 / p95 4 / max 10**、
      足跡 (±12 px) の外は **0 画素**、8 px 超は (621,225) の 1 画素だけでチャンネル差 1 (R 44→45)
    - **29 px の原因調査 (1 回で打ち切り)**: 8 通り試して**どれも 29 を出さない** —
      チェビシェフ 10 / ユークリッド 11.2 / マンハッタン 15 / 半解像度 5 /
      1 px 収縮マスク (20 px) 486 (足跡外 169 画素) / 最大連結成分 485 (同 185 画素) /
      `--rt-debug 13` の Hero (3081 px、別 run で撮影) 328 (同 151 画素) / 13 ∪ 14 は 10 (14 単独と同値)。
      収縮系・13 系は初出の「外れは 1 画素だけ」とも両立しないので候補から落ちる。
      **原因未特定**と ADR に明記した
  - tools\replay_verify.bat → **未実行**。理由: 変更がコメント / 文書 / bat の `rem` に閉じており
    (`git diff` のコード行の増減 = 0)、同一のコード意味論で sub-02 が全 10 ジョブ緑を取っている。
    planner の指示 (sub-03.md「回さなくてよい + 理由を明記」) に従った
自己採点 (1-5):
  仕様適合: 4 — B1〜B7 のうち B1 / B3 / B4 / B5 / B6 / B7 は実測で満たした。
    B2 だけ **CLAUDE.md:109 の 1 か所が未修正**で残る (運用規約による。全件の台帳は下に列挙)
  正しさ: 5 — 文言以外を 1 バイトも動かしていないことを golden 24 枚 maxDiff=0 /
    selftest exit 0 / check_rules 0/0 / Debug・Release 警告 0 で確認。訂正した数値は
    自分の run から出したもので、reviewer の値と独立に一致した
  コード品質: 4 — コメントは日本語で「なぜ」中心、bat は CRLF を機械確認。
    5 にしないのは CLAUDE.md の 1 行を他人に投げているため
  テスト: 4 — 新規テストは足していない (足す種類の変更ではない = コメントと文書のみ)。
    既存の被覆 (shot_verify / selftest / check_rules) は全部回した。replay_verify だけ未実行
不安・質問:
  1. **CLAUDE.md:109 を誰が直すか。** coder の運用規約でエージェントからの指示では
     CLAUDE.md を書き換えられない。上の置換案をユーザーか司会が入れてほしい。
     入るまで B2 は「4 / 5 か所」で、残り 1 か所は**手順を誤らせるものではない**
     (`--rt-restir-no-spatial` の説明で、時期の指定が古いだけ)
  2. **旧 (Hero=8) マスクを撮るために既定値を一時的に戻してよいか。** 今回は禁止事項
     (既定値に触れない) を優先して撮っていない。上界の論証で結論は出るので**不要**という判断だが、
     ADR に「旧マスクは撮っていない」と明記したのが妥当か planner に見てほしい
  3. **29 px の原因は特定できなかった。** 8 通りの再現を試して全部外れたので打ち切った
     (sub-03.md の指示どおり)。round 2 の測定スクリプトは残っていないので、
     これ以上は「当時の手順を復元する」作業になる = 行動が変わらない問いだと判断した
触ったファイル:
  - tools\shot_verify.bat
  - src\Editor\EditorApp.cpp
  - src\Engine\Renderer\RayTracing\RtTypes.h
  - docs\adr\ADR-016-restir-reflection.md
  - engine_spec.md
  - plans\m67h-restir-finalize\sub-03.md (この実装メモ)
申し送り:
  - **CLAUDE.md:109 が唯一の積み残し** (上記)。
  - `--rt-debug 14` の Hero マスクは frame 40 で **400 px / 56 連結成分 / 1 px 収縮すると 20 px**
    という細い形。**このマスクを収縮して距離を測ると数字が桁で変わる** (max 10 → 486)。
    M の読み出し (A17) では収縮が正しく、距離では収縮してはいけない — 用途で使い分けること。
  - `tests\actual\s03_d14_f40.png` / `s03_d13_f40.png` は gitignore 配下。再測の材料として残してある。
  - `docs\adr\ADR-016-*.md` は**作業ツリーが LF** なので `git diff` が
    「LF will be replaced by CRLF」を警告する (`plans\*.md` も同じ)。既存の線種に合わせて
    LF のまま書いた — CRLF へ揃えると全行が diff に出て、訂正 1 か所が読めなくなるため。
    blob は LF なのでコミット内容は変わらない。
```

### B2: `grep -rn "M67h"` / `"S5"` の全件台帳

`git ls-files | xargs grep -n "M67h"` (plans/ を除く) = **35 件** (修正後。修正前は 34 件で、
ADR への追記 1 行が増えた分)、`grep -rn "S5" src/ tools/ docs/ CLAUDE.md engine_spec.md README.md`
のテキスト一致 = **19 件**
(他に `tools/collab/target/**` のバイナリ一致が多数あるが `cargo` の生成物 = gitignore)。

**直した (5 か所のうち 4 か所)**

| 場所 | 何が古かったか | どうしたか |
|---|---|---|
| `tools\shot_verify.bat:378` | 「M67h で焼いたら `--update` で撮り直す」= **本件が避けた手順を指示** | 実態 (M67i で差し替え済み + 次も実物コピー) へ。理由つき 4 行 |
| `src\Editor\EditorApp.cpp:1262` | 「後続 M67h が定数表へ焼く」 | 「決着済み。出所は今も RtTypes.h」へ |
| `src\Engine\Renderer\RayTracing\RtTypes.h:276` | 「S5 / M67h で再評価できるようにしておく」 | 設計意図は残し、時期の指定を「M67h で決着 = off 維持」へ |
| `docs\adr\ADR-016:180` (追記は :184) | 「所見: S5 で最初に触るノブは M 上限」= 未来形 | 「この所見は当たった」+ 誤用の戒めを 3 行追記 |
| `CLAUDE.md:109` | 「S5 / M67h で既定を on へ反転したときに」 | **未修正** (運用規約。置換案は SELF_EVAL へ) |

**直さなくてよい (残り全件、理由つき)**

| 群 | 場所 | 理由 |
|---|---|---|
| 完了形の記録 (M67h で何をしたか) | `assets\shaders\rt_restir_cb.hlsli:34` / `RtPasses.cpp:164,792` / `RtPasses.h:62` | 「M67h で外した枠」= 過去の事実。旧名で検索して理由に辿り着く導線 (A7) |
| ADR の完了形記述 | `ADR-016:284,288,290,336,449,476,558` | 「決着した」「測定セットには入っていない」「再評価していない」「round 1 で踏んだ」= すべて過去形 |
| engine_spec の完了形記述 | `engine_spec.md:483,510` | "(M67h; …)" / "measured, not guessed (M67h)" = 完了 |
| 現行規則の出所表示 | `AssetOpsSelfTest.cpp:329,364` / `ReflectionClassJson.h:16,23` / `GpuResources.cpp:22,1020` / `InspectorWindow.cpp:41,1479` | 「この規則は M67h 由来」= 今も有効な説明 |
| 実装の由来 | `EditorApp.cpp:1266,1306` / `RenderSystem.cpp:1147` / `RenderSystem.h:245` / `LocalizationTable.inl:178` | 同上 |
| 測定結果・不変量の由来 | `RtSelfTest.cpp:1180,1181,1205` / `RtTypes.h:233,249` | 「測った」「ユーザー判断で 8 → 16 した」= 過去形 |
| 別マイルストーンの S5 | `GitTransaction.cpp:496` / `GitTransaction.h:207` / `AcousticAudio.cpp:338` | Source Control / 音響の spec の S5。ReSTIR とは無関係 |
| S5 = 作業名としての言及 | `RtScene.h:38` | 「チューニング (S5) と `--rt-class-override` のための口」= 口の存在理由。時期の指定でも宿題でもない |
| 「S5 の結論」節への参照 | `RtSelfTest.cpp:1208` / `RtTypes.h:235` / `ADR-016:308` / `engine_spec.md:527` | 節名そのもの。正しい |


## フィードバック履歴

- round 1: **VERDICT OK** (planner、2026-09-08)。must 0 / should 0 / nit 1。**サブ完了。**
  - **[未実装] CLAUDE.md:109 を認める。** coder の運用規約による**正当な拒否**で、責めるべきものではない。
    B2 を訂正して `CLAUDE.md` を coder の対象から外し、ユーザー / 司会の作業として置換文つきで
    上に書いた。手順を誤らせる種類ではない (時期の指定が古いだけ) のでコミットの blocker にしない。
  - **[逸脱] 旧 (Hero=8) マスクを撮らなかったのは正しい。** (a) 和集合は新マスクの上位集合なので
    距離は実測値**以下**にしかならず、結論 (足跡の内側) は**上界だけで成立する** = 撮っても結論が
    変わらない。(b) 既定値を戻して Release を焼き直すのは「やらないこと」に真正面から反し、
    使い捨ての計測物のために出荷定数とバイナリを一時的に誤った状態にする risk を負う。
    (c) ADR に「撮っていない」と明記したので読者が誤らない。**不安 2 への回答 = 戻さなくてよい。**
  - **[追加] ADR-016:184-186 を承認。** grep で自分で見つけた 5 か所目で、B2 の「これからやると
    読める記述が 0 件」を満たすのに必要だった。4 か所で打ち止めにしなかったのは指示どおり。
  - **不安 3 (29 px の原因未特定) を承認、ここで打ち切る。** 8 通り (チェビシェフ / ユークリッド /
    マンハッタン / 1 px 収縮 / 最大連結成分 / 半解像度 / debug 13 / 13 ∪ 14) がどれも 29 にならず、
    round 2 の測定スクリプトも残っていない。これ以上は「消えたスクリプトの復元」で、**結論は
    上界の論証で保証されている**ので決定価値がゼロ。「原因未特定」と ADR に書いたのが正しい対応。
  - [nit] `--selftest` の `PASS:` が 3385 行という数え方は他ラウンド (「`ALL PASS` 29 本」) と
    粒度が違う。どちらでも判定はできるので直さなくてよい。次に数えるときは揃えると比較が楽。
  - planner が実物で再検証した項目: **`tools\shot_verify.bat` が完全に CRLF** (464 CR / 464 LF)
    かつ **BEL 0 バイト** (`tr -cd` によるバイト単位の確認。`awk '!/\r$/'` は MSYS が text モードで
    \r を落とすため**全行 LF に見える偽陽性**を出すので使わないこと) / `git check-attr` が
    `eol: crlf` / `CLAUDE.md` と `tests/golden/` が作業ツリーで未変更 / `engine_spec.md:1894` の
    史実記述が未接触で `:1948` が「the suite is …」へ / ADR から「8 px 以内」「未特定」「追跡」が
    消えて上界の論証と「原因未特定」に置き換わっていること。
