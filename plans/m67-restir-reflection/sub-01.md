# sub-01: RT 反射 / GI の golden 2 枚 (ローカル限定) + ベースライン計測

- 依存: なし
- 状態: OK (commit 73a439e)
- 往復: 1

## やること

spec §2 S1 / S10 の穴埋め。**シェーダにも C++ にも触らない**。「ReSTIR off = 現行とビット一致」を
以降の全サブで機械証明するための土台を、変更前のバイナリで固定する。

1. `tools\shot_verify.bat` に 2 本足す (SSR の枠 = bat 152-166 行と同型。**CRLF**、`if defined ... goto` の囲い、
   `TOLNOW=0`、囲いの環境変数は 1 本 `MYE_SHOT_SKIP_RT`):
   ```
   call :shot demo_render_rtrefl --render-demo --deferred --rt-refl
   call :shot demo_render_rtgi   --render-demo --deferred --rt-gi
   ```
   置き場所は**一覧の末尾 (20/21 枚目)** — 既存の枠 (fog / particle / acoustic) も tol や frame と無関係に
   末尾へ append されてきた。中間に挿すと 13〜19 枚目の番号が bat / CLAUDE.md / README で全部ずれる
   (round 1 で coder の指摘により訂正。planner の当初指示「froxel の直後」は誤り)。bat 冒頭の「19 本」の数え書きと囲いの理由
   (「RT デモは WARP では重すぎる」の 112 行目コメントは `--rt-demo` の話で、render-demo の反射は 11 s /
   run-to-run 一致 = planner 実測、と書き換える) を更新する。
2. `.github\workflows\ci.yml` の env に `MYE_SHOT_SKIP_RT: 1` + 冒頭コメントの一覧に 1 段落
   (「BVH の hit/miss 分岐は SSR と同型で機種差が増幅する → ローカル限定」)。
3. `CLAUDE.md` 検証表の `shot_verify.bat` 行: 「19 枚」→「21 枚」、ローカル限定の列挙に「RT 反射 / RT GI」、
   `MYE_SHOT_SKIP_*` の一覧に `_RT`。
4. `tools\shot_verify.bat --update` で 2 枚を撮り、**中身を見てから** `tests\golden\demo_render_rtrefl.png` /
   `demo_render_rtgi.png` を追加 (`.gitattributes` の `*.png binary` は既にある)。
5. **S0 ベースライン**: 次の 2 コマンドの `[rt]` ログ行 (`EngineLoop.cpp:1826`、`trace / refl / denoise` の ms) を
   実装メモへ写す (WARP の値。実 GPU はユーザーが ProfilerWindow で見る):
   ```
   cmd /c bin\x64\Release\Runtime.exe --render-demo --deferred --rt-refl --warp --no-audio --font-embedded --width 960 --height 540 --frames 6 --shot-frame 3 --no-fxaa --screenshot <scratch>\base_render.png
   cmd /c bin\x64\Release\Runtime.exe --acoustic-demo --deferred --rt-refl --warp --no-audio --font-embedded --width 960 --height 540 --frames 121 --shot-frame 120 --no-fxaa --screenshot <scratch>\base_acoustic.png
   ```
   ログの置き場は `MYE_LOG_INFO` の出力先 (bin 配下の `.log` または標準出力) — 見つけた場所も実装メモに。

## やらないこと (このサブでは)

- シェーダ / C++ の変更。Material / RtTypes / RtPasses は次サブ以降。
- `--rt-demo` (コーネル箱) の golden。CI 判定 (tol=3) への昇格。
- `--acoustic-demo --rt-refl` の golden (frame 120 で RT はコストが読めない。ベースライン計測だけ)。

## 触る場所 (planner の見立て)

- `tools\shot_verify.bat` (152-166 行の SSR 枠が雛形。`:shot` サブルーチンは 352 行以降。**CRLF で書く**)
- `.github\workflows\ci.yml` (env 74-82 行、冒頭コメント 16-48 行)
- `CLAUDE.md` 39 行目の表、および CI 環境変数の段落 (44-52 行付近)
- `tests\golden\demo_render_rtrefl.png` / `demo_render_rtgi.png` (新規、`--update` の生成物)

## 受け入れ条件 (このサブ)

1. `tools\shot_verify.bat` が 21 枚全緑 (A1)。
2. 新 2 枚は同一 Release バイナリで **2 回撮って `--img-diff --tol 0` PASS** (A2)。`--rt-gi` 側が割れたら
   GI 側を落として「不安・質問」に理由 (maxDiff / diffPixels / worst pixel) を書く。
3. 新 2 枚の所要時間 (SHOTBASE 条件) が各 ≤ 60 s。超えたら数値を書いて報告 (planner が 0.25 スケール等を判断)。
4. `MYE_SHOT_SKIP_RT=1` を立てて `shot_verify.bat` を回すと 2 枚が飛び 19 枚で緑 (CI の経路)。
5. S0 の `[rt]` ログ行 2 本が実装メモにある (A11 の前半)。
6. `pwsh -File tools\check_rules.ps1` 緑 (bat の CRLF 等は規則の対象外だが習慣として)。
7. 撮れた 2 枚を目視して「反射床パッチに柱と灯りが映っている」「GI で影側の面が持ち上がっている」ことを
   実装メモに 1 行ずつ (真っ黒 / 真っ白の golden は守るものが無い — bat:305 の教訓)。

## 検証コマンド

```
rem Release ビルド (shot_verify は Release 前提)
for /f "usebackq tokens=*" %i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set MSBUILD=%i
"%MSBUILD%" MyEngine.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
tools\shot_verify.bat --update      (2 枚を撮る → 目視 → 以降は通常モード)
tools\shot_verify.bat               (21 枚)
set MYE_SHOT_SKIP_RT=1 && tools\shot_verify.bat   (19 枚、CI 経路)
cmd /c bin\x64\Release\Editor.exe --img-diff A.png B.png --tol 0   (run-to-run)
pwsh -File tools\check_rules.ps1
```
exe は **PowerShell ツールから `cmd /c`** で (GUI サブシステム。Bash ツールの `cmd /c` は実行されない)。

## 実装メモ (coder が追記)

### round 1 (2026-09-04 着手 / 2026-09-05 完了)

```
SELF_EVAL: sub-01 (round 1)
実装:
  - tools\shot_verify.bat — 20/21 枚目として `demo_render_rtrefl` (--render-demo --deferred --rt-refl) /
    `demo_render_rtgi` (同 --rt-gi) を追加。`if defined MYE_SHOT_SKIP_RT goto :skip_rt` の囲い、
    TOLNOW=0、CRLF 維持 (417 CRLF / bare LF 0)。存在理由・実測値・「tol は上げない」を
    SSR / particle と同じ濃度でコメント化
  - tools\shot_verify.bat — 冒頭の数え書き 19 本 → 21 本、tol=0 の枚数 5 → 9 (fog / パーティクル 2 が
    既に漏れていたので併せて実態合わせ)、112 行目「RT デモは WARP では重すぎるので CI 対象外」を
    「--rt-demo (コーネル箱) の話であって --render-demo + --rt-refl/--rt-gi は 11 s」へ書き換え、
    PASS 行の tol=0 一覧に rt を追記
  - .github\workflows\ci.yml — env に `MYE_SHOT_SKIP_RT: 1`、冒頭コメントに MYE_SHOT_SKIP_RT の段落
    (BVH の hit/miss 分岐が SSR と同型)、「同じ役目の 6 本」→「7 本」(2 箇所)、
    step 名 `screenshot regression (12 shots, ... /rt)`
  - CLAUDE.md — 検証表の shot_verify 行を 19 枚 → 21 枚 / ローカル限定 7 枚 → 9 枚、
    CI 環境変数の列挙に `_RT` と 1 行の理由
  - tests\golden\demo_render_rtrefl.png / demo_render_rtgi.png — 新規 (--update の生成物、版管理対象)

仕様との差分:
  - [逸脱] 置き場所を「froxel (12 枚目) の直後」ではなく **一覧の末尾 (20/21 枚目)** にした。
    理由: 中間に挿すと 13〜19 枚目の番号が全部ずれ、bat の 6 ブロックに加えて
    **CLAUDE.md 5 箇所 (70-79 行) / README.md 4 箇所 (134-148 行)** の「N 枚目」が同時に嘘になる。
    sub-01 が CLAUDE.md で触ってよいと書いたのは 39 行目の表と CI 環境変数の段落だけで、
    README は sub-07 の担当。また bat の既存の並びは「追加時に末尾へ append」で、
    fog (M57追補) / particle (M63a) / acoustic (M65e) はどれも tol=0 や frame 120 の枠と
    無関係に末尾へ足されている = 「tol=0 をまとめる」という並びの規約は元々存在しない。
    末尾 append なら既存の番号が 1 つも動かず、追記は数え書きと枚数だけで閉じる。
    ★planner が中間挿入を維持したいなら、call 2 本と囲いを :skip_froxel の直後へ移すだけで済む
    (併せて bat / CLAUDE.md / README の番号を全部繰り下げる指示が要る)。
  - [追加] `tol=0 … ローカル限定の 5 枚 (fxaa / taa / ssr / froxel / fog)` を 9 枚へ実態合わせ。
    fog 追加時にパーティクル 2 枚が数え漏れていた既存の誤り。自分が同じ行に 2 枚足すので
    「5 枚」のまま残せないと判断した (1 行のコメント修正)。
  - [追加] ci.yml の「同じ役目の 6 本」→「7 本」(冒頭 1 箇所 + ACOUSTIC 段落 1 箇所) と
    step 名の `/rt`。SKIP 変数を 1 本足した直接の帰結。
  - [未実装] CLAUDE.md の CLI 一覧 (70 行付近) に `--rt-refl` / `--rt-gi` を載せていない。
    そもそも現状も載っていない (RT 系は `--rt-demo` だけ)。spec A12 が sub-07 の担当と
    書いているのでここでは触らない → 申し送りへ。

検証:
  - MSBuild Release x64 (MyEngine.sln) → 成功 (4 プロジェクト)
  - `cmd /c tools\shot_verify.bat --update` → exit 0 / 21 shots / **214.1 s**。直後の
    `git status --short tests/golden` が `?? demo_render_rtgi.png` と `?? demo_render_rtrefl.png`
    の 2 件のみ = **既存 19 枚は 1 バイトも動いていない** (bat 編集が既存の撮影条件を
    壊していないことの機械証明)
  - `cmd /c tools\shot_verify.bat` → **exit 0 / [PASS] 21 shots**。21 本すべて
    `maxDiff=0 diffPixels=0` (tol=3 / 12 / 0 の全枠)。216.3 s
  - `MYE_SHOT_SKIP_RT=1` + `cmd /c tools\shot_verify.bat` → **exit 0 / [PASS] 19 shots**。206.9 s
    (CI 経路。`=== shot:` の出現数を数えて 19 を確認)
  - run-to-run (A2、同一 Release バイナリ / 別プロセス):
    `--rt-refl` 2 回 → `--img-diff --tol 0` **PASS maxDiff=0**、`--rt-gi` 2 回 → **PASS maxDiff=0**。
    さらに bat 経由で撮った golden と手動撮影 (3 回目) を照合 → 2 枚とも **PASS maxDiff=0**。
    **`--rt-gi` は割れなかった** (spec §7 のリスク 1 件目は空振り = 良い方に外れた)
  - 所要時間 (A2 の ≤ 60 s、SHOTBASE 条件・単独実行): rtrefl 11.4 / 11.1 s、rtgi 10.9 / 11.0 s。
    RT 無しの同条件が約 7 s なので RT の増分は 4 s/枚。**どちらも 60 s の 1/5 以下**
  - 被覆 (「守るものが絵に出ている」の機械値): `demo_render_deferred.png` (RT 無し) との差が
    反射 **45114 画素 / maxDiff=221**、GI **221162 画素 / maxDiff=93**。
    2 枚どうしの差も 226805 画素 = 同じ絵の 2 枚目ではない
  - `pwsh -File tools\check_rules.ps1` → **0 error(s), 0 warning(s)**
  - 目視 (受け入れ条件 7、画像は tests\golden\ に入ったものと同一):
    - `demo_render_rtrefl.png`: 中央の反射床パッチ (rdemo_mirror、粗さ 0.10) に**青い柱と
      オレンジの回転十字、点光源の芯**が映り込んでいる。左下の縞デカール床にも鏡面の筋が乗る。
      差分ヒートマップは反射面 (鏡床 / 縞床 / 金属柱 / 回転体) だけが赤く、背景は黒 = 寄与の位置が正しい
    - `demo_render_rtgi.png`: 箱の**影側の面と床の影の中**が一段持ち上がり、赤い柱の陰面に
      色移りが出る。差分ヒートマップは**画面のライティング面全体に薄く広がる** (maxDiff=93 の
      うち大半が 1〜数レベル) = 間接光らしい形。真っ黒でも真っ白でもない
  - S0 ベースライン (A11 前半、WARP / Release / 開発機):
    ```
    --render-demo --deferred --rt-refl (frames 6 = sub 指定のコマンド):
      [rt] mode 0: 35 instances / 780 triangles / build 0.147 ms (CPU) /
           trace 0.000 / gi 0.000 / temporal 0.000 / svgf 0.000 /
           shadow 0.000 (+ filter 0.000) / refl 0.000 (+ denoise 0.000) (GPU, last frame)
    --render-demo --deferred --rt-refl (frames 20 = GPU タイマーが読める最小条件):
      build 0.346 ms (CPU) / refl 4.275 ms (+ denoise 22.646 ms)
    --render-demo --deferred --rt-gi   (frames 20、参考):
      build 0.199 ms (CPU) / gi 13.831 ms / temporal 1.454 ms / svgf 29.471 ms
    --acoustic-demo --deferred --rt-refl (frames 121 = sub 指定のコマンド、19.4 s):
      [rt] mode 0: 101 instances / 780 triangles / build 64.773 ms (CPU) /
           refl 7.992 ms (+ denoise 18.492 ms) (GPU, last frame)
    ```
    ★**`--frames 6` では GPU 時間が全部 0.000 ms になる**。`GpuTimer` は
    `kFrames = 6` のリングで、スロットの計測値は**そのスロットを再利用するとき**にしか
    回収しない (`GpuTimer.cpp:29-46`) ので、6 フレームでは 1 度も回収が起きず `lastMs_` が
    初期値 0 のまま出る。**7 フレーム以上**回して初めて数字が出る (frames 20 で確認)。
    frames 6 と frames 20 のスクショは `--img-diff --tol 0` で**ビット一致**なので、
    計測のためにフレーム数を伸ばしても golden の条件は変わらない。
    以降のサブで `restir` の ms を読むときは **frames を 20 以上にすること**。
    ログの置き場は **標準出力** (`Log.cpp` の `EmitUtf8` → WriteConsoleW / fputs。
    `.log` ファイルは作られない) なので `cmd /c ... > out.txt 2>&1` で取る。

自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 1〜7 を全て機械確認。置き場所だけ sub の指定と違う ([逸脱] に明記)
  正しさ: 5 — 21 枚 PASS / 19 枚 PASS / 既存 golden が git 上で 1 バイトも動かない /
           新 2 枚は 3 回の独立撮影で maxDiff=0。全部コマンドの出力で裏取り済み
  コード品質: 4 — bat は CRLF・囲い・コメント密度とも既存枠 (SSR / particle) と同型。
           既存の数え漏れ (tol=0 が 5 枚表記) も直したが、これは仕様外の実態合わせ
  テスト: 5 — このサブの成果物がテストそのもの。CI 経路 (SKIP) と通常経路の両方を実走

不安・質問:
  1. 置き場所の [逸脱] (末尾 20/21 枚目) を承認するか、中間 (13/14 枚目) へ戻して
     bat / CLAUDE.md / README の番号繰り下げまで指示するか。
  2. `--frames 6` で GPU タイマーが 0 を返す件 (上記)。A11 は「`[rt]` ログに GPU 時間が出る」を
     受け入れ条件にしているので、sub-04 以降で restir の ms を主張するときは撮影と別に
     frames 20 の計測 run を 1 本回す運用でよいか (golden はビット一致するので害は無い)。
     GpuTimer 側を触る (リングを深くする / 明示 Flush) 提案は**していない** — 描画の
     計測機構を M67 の都合で変えるのはスコープ外と判断した。
  3. spec §7 のリスク「`--rt-gi` が WARP で run-to-run 決定的でない可能性」は**空振り**
     (2 回撮って maxDiff=0)。GI 側を落とす必要は無かった。

触ったファイル:
  - tools\shot_verify.bat
  - .github\workflows\ci.yml
  - CLAUDE.md
  - tests\golden\demo_render_rtrefl.png (新規)
  - tests\golden\demo_render_rtgi.png (新規)
  - plans\m67-restir-reflection\sub-01.md (実装メモのみ)

申し送り:
  - CLAUDE.md の CLI 一覧に `--rt-refl` / `--rt-gi` が**元から載っていない**。sub-07 (A12) で
    `--rt-restir` 系を書くときに一緒に載せると読み手が困らない。
  - `engine_spec.md:1711` は「`shot_verify.bat` captures **fifteen** deterministic screenshots」
    のまま (19 の時点で既に古い)。sub-07 で 21 → 22 に直すときに拾うこと。
    `engine_spec.md:403` の「the RT demo is too slow under WARP, so the feature would carry
    permanently zero automated coverage」も、この 2 枚で前提が半分崩れた (RT 反射 / GI は
    被覆を得た。RT 影は依然ゼロ) — 文言の更新は sub-07 の判断で。
  - 以降のサブで golden が動いたら **--update で塗り潰さないこと**。この 2 枚は
    「ReSTIR off = 現行とビット一致」の唯一の証拠なので、動いた時点で報告が先
    (golden-diff-triage の 4 点計測)。
  - reviewer 向け: 一時画像 (差分ヒートマップ含む) はスクラッチに置いた。
    `%TEMP%\claude\C--HAL-MyEngin\60e02b8b-2cb8-4608-bcef-46fc4c1dd8d6\scratchpad\`
    (`rtrefl_1.png` / `rtgi_1.png` / `refl_diff.png` / `gi_diff.png`)。
    再現は `cmd /c bin\x64\Release\Editor.exe --img-diff tests\golden\demo_render_deferred.png
    tests\golden\demo_render_rtrefl.png --tol 0 --diff-out <出力>`
```

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-09-05)。受け入れ条件 1〜7 を SELF_EVAL の検証欄で確認 (21 枚 PASS /
  SKIP で 19 枚 / run-to-run 3 回 maxDiff=0 / 11 s / check_rules 0-0 / 目視 2 行 / S0 の数値)。planner 側でも
  git status・bat の囲いと CRLF (417/0)・`GpuTimer.cpp` のリング回収を読んで裏取り。
  [逸脱] 置き場所 = 末尾 20/21 → **仕様側の誤り**として sub-01 の「やること」を訂正 (coder の理由を採用)。
  [追加] 5 枚 → 9 枚の実態合わせ / ci.yml の 6 本 → 7 本 = 直接の帰結、承認。
  [未実装] CLAUDE.md の CLI 一覧 → sub-07 の「やること」に明記 (元から `--rt-refl` / `--rt-gi` が未掲載)。
  不安 2 (GpuTimer が 6 フレームでは回収しない) → **計測 run は `--frames 20`** を spec A11 / sub-04 / sub-06 に反映。
  GpuTimer 自体は触らない (M67 のスコープ外、coder の判断に同意)。不安 3 → spec §7 のリスクを「実測で否定」に更新。
