# sub-02: S5 の確定 (2 軸の計測 → 規則適用 → 既定値と文書)

- 依存: sub-01
- 状態: 未着手 (§7 Q1 の回答で**入力**が変わる。受け入れ条件と手順の骨格は変わらない)
- 往復: 0

## 入力が 2 通りある

| 入力 | やること |
|---|---|
| **(既定) ユーザーの確定値が無い** | 下の「手順」を最初から回す。**帰無仮説 = 現行の既定表**で、spec §4.4 の規則 R1〜R6 が要求したときだけ値を変える |
| **ユーザーが実機の確定値を持っている** (Q1 の回答が (i)) | 手順 3〜4 (本測定と規則適用) を**飛ばして**ユーザー値を焼く。手順 1 (A9 の観測) / 5 (golden) / 6 (文書) は**そのまま実施**する。ユーザー値は計測に優先する。値の出所を「ユーザーの実機判断 (日付)」として ADR に明記する |

司会から追加指示が来ない限り (既定) で進める。**「回答待ち」で止まらない。**

## やること

`spec.md` §4.4 の R1〜R8 が仕様。以下は「どの順に潰すか」だけを決めたもので、
**最もリスクの高い未知 (= 指標が立つのか) を最初に潰す**順序になっている。

### 手順 0 — 基準線

Release ビルド (`/p:MyeWarnAsError=true`) を作り、`shot_verify.bat` が 24 枚全緑であることを
**測定を始める前に**確認する。ここが赤いまま測ると、後で「既定値のせいか sub-01 のせいか」が切り分けられない。

### 手順 1 — 領域の決定と、review-1 の「未確認」を閉じる (A9)

`--render-demo --deferred --rt-refl --rt-restir` の frame 40 で、

- `--rt-debug 13` (一次ヒットのクラス) の **赤 = Hero** 画素 = **回転体 `rdemo_spin` 自身の表面**。
  これが「動いている受け面」= `P_prev ≠ P` = **J ≠ 1 の temporal 経路**。
- `--rt-debug 14` (反射像側のクラス) の **赤 = Hero** 画素 = **回転体が映っている受け面**。
  これが軸 B (追従率) の測定領域。

この 2 つのマスクから矩形を機械的に決め (M67 A7-a と同じ流儀)、実装メモに矩形の座標と決め方を残す。

**A9 の主張**: `--rt-debug 12` (reservoir の M) の frame 40 で、上の「回転体自身の表面」画素の M が
**1 より大きい** (履歴が動く受け面を跨いで生き残っている = 厳密 Jacobian と velocity の配線が
GPU で機能している) こと。あわせて同領域に孤立高輝度画素 (firefly) が無いこと
(M67 sub-05 と同じ「最大輝度の on/off 比較」で可)。

- 予測を先に書くこと: 「M は 1 より大きいが cap (Hero = 8) までは伸びない」か「cap まで伸びる」か。
  予測と違ったら、**測定を進める前に**何が違うのかを調べる (spec §7 のリスク)。
- M が 1 のまま = J が常に棄却されている = review-1 の心配が的中したケース。その場合は
  **測定を止めて planner へ「不安・質問」で上げる** (S5 より先に直すべきバグの可能性)。

### 手順 2 — 軸 B (追従率) の健全性検査

本測定に入る前に、**指標が mCap に反応するか**だけを 2 条件で確かめる。

```
軸 B = mean|test[41] - test[40]| / mean|ref[41] - ref[40]|   (手順 1 の矩形、--rt-debug 11 = 反射レーン)
  ref  = --rt-refl            (ReSTIR off、デノイズは既定のまま)
  test = --rt-refl --rt-restir --rt-class-override N
```

- 健全性 (1): **静止した反射像の領域**では ref の分母がノイズ主体になり比が意味を失う → 分母が
  十分大きい (= 本当に動いている) 領域を選べているかを数値で示す。
- 健全性 (2): `--rt-class-override 0` (mCap 8) と `3` (mCap 32) で軸 B が**目に見えて違う**こと。
  違わなければ「この条件では mCap は追従に効かない」= R1 の「維持」で閉じてよい (手順 3 を短縮できる)。
- ここで使う 4 枚 (ref 40/41 + test 40/41 ×2) は本測定の一部なので**無駄にならない**。

### 手順 3 — 本測定 (2 軸 × 4 条件)

**最小セット**: `{off, override 0 (mCap 8), override 1 (16), override 3 (32)}` の 4 条件 × frame 40/41。

- **軸 A (フリッカー)**: `--rt-anim-seed --rt-no-temporal --rt-no-svgf --rt-debug 11`
  (M67 A6/A7 と同一手法。`--rt-no-temporal` は SVGF 側だけを止め、ReSTIR の temporal は止まらない)。
- **軸 B (追従率)**: `--rt-anim-seed --rt-debug 11` (デノイズ既定のまま = 端から端までの応答)。

`override 2` (mCap 24) と「既定混在 (override 無し)」は、**4 点で結論が割れたときだけ**足す
(情報の価値 ÷ 取得コストで並べる)。

- 表は「条件 × (軸 A、軸 B)」の形で実装メモへ。M67 の既存数値 (音響デモの床: temporal 単独 0.181 /
  一様 Prop 0.100 / spatial on 0.255) との**比較可能性は主張しない** (被写体もフレームも違う) —
  比較するなら被写体を揃えて測り直す。
- **`--shot-every` を使わない** (spec §4.4 R8。`EngineLoop.cpp:704` で決定的撮影モードが外れる)。
  フレームは `--frames 41 --shot-frame 40` / `--frames 42 --shot-frame 41` の個別 run。
- **A12**: 代表 1 条件を 2 回撮って `Editor.exe --img-diff A B --tol 0` が PASS すること。
- 画像は `tests\actual\` (gitignore) へ。reviewer が再撮影できるようコマンドを実装メモに残す。

### 手順 4 — 規則の適用 (spec §4.4)

- **R1**: 変更条件 (軸 A が 20% 以上改善 **かつ** 軸 B の低下 0.05 未満) を満たしたクラスの `mCap` だけを
  `kRtReflClassTable` で変える。満たさなければ**現行維持**。
  どちらの結論でも「どの数値がどの規則をどう満たした / 満たさなかったか」を書く。
- **R5**: 軸 B が **0.8 未満**だったときに限り、`svgfHistory` 8 → 4 の 1 候補を再ビルドして軸 A/軸 B を測り直す。
  軸 B が上がり軸 A が悪化しないときだけ焼く (このとき `RtReflRestirParams::svgfHistory` の初期化子は
  新しい定数を `RtTypes.h` に作って指す — `RtSelfTest.cpp:1192` の
  `def.svgfHistory == kRtReflMaxHistory` も一緒に直す)。0.8 以上なら**やらない**。
- **R2 / R3 / R4 / R6**: 変えない。ADR に理由を書く (R4 は「spatial を on にしたときの初期値であって
  確定値ではない」と明記)。
- **Q1-a / Q1-b がユーザーから返ってきた場合**: `spatial` / `visRay` の既定を 1 行変え、
  `RtSelfTest.cpp:1197` の `TEST_CHECK(def.spatial == 0 && def.visRay == 0 ...)` と、その直前のコメント
  (「ここを 1 に戻すなら計測をやり直すこと」) を**ユーザー判断であると分かる形**に書き直す
  (計測結果を消さない — 「計測は temporal 単独が良かったが、実機の主観で on を選んだ」と両方残す)。

### 手順 5 — golden (値を変えたときだけ)

`demo_render_rtrefl_restir.png` を撮り直す。**`--update` で塗り潰さない** — M67g と同じく
比較 run の実物をコピーし、`git status` が「1 枚しか動いていない」ことの直接の証拠になる形にする。
3 回の独立 run が tol=0 で一致することを示す。値を変えなかったら golden は 1 枚も触らない。

### 手順 6 — 文書 (値を変えなくても必ずやる)

- **ADR-016** に「S5 の結論」節: 計測表 (条件 × 軸 A/B) / 変えた値と根拠 / **変えなかった値と理由** /
  **未検証のまま残る条件** (カメラが大きく動く条件の firefly と二重 temporal の主観品質。
  ヘッドレスに経路が無く、デモには触らないと決めたため)。A9 で閉じた「J ≠ 1 の経路」も書く。
- **engine_spec.md** §6.4 の該当箇所。加えて**現在形の golden 枚数だけ**を実測 (24) に合わせる
  (`:1926` の "and twenty-two images today" / `:1995` の "22 deterministic screenshots" /
  `:2041` の "all fourteen other" が候補)。**`:1872` のような史実の記述は触らない**
  (触るなら「M67 時点で」と主語を補う)。直した行を実装メモに列挙する。
- **CLAUDE.md**: 既定値を変えたときだけ (golden の frame や既定の説明が変わる場合)。
- `RtTypes.h` の変えた定数のコメントに「いつ・何を測って決めたか」を 1〜2 行
  (次にここを触る人が、また同じ測定をやり直さずに済むように)。

## やらないこと (このサブでは)

- **デモシーン / デモ材質 / カメラに触る** (spec §4.4 R7)。golden 24 枚の被写体そのもの。
- `spatial` の既定 on/off の再評価 (U7 で決着済み。ユーザーが指示した場合のみ 1 行)。
- `--img-diff` の矩形オプション追加、新しいデバッグ CLI の追加 (spec §2 S12)。
- `radiusPx` / `taps` / `radiusAlphaRef` を焼くこと (spec §2 S4)。
- 再ビルドを伴う総当たり sweep (R5 の 1 候補だけが例外)。
- `kRtRestirWMax` の予防的追加 (spec §2 S8)。

## 触る場所 (planner の見立て)

| ファイル | 何を |
|---|---|
| `src/Engine/Renderer/RayTracing/RtTypes.h` | `kRtReflClassTable` (:233-239) / `RtReflRestirParams` (:246-262)。**規則が要求したときだけ**。触ったら根拠コメント |
| `src/Engine/Engine/RayTracing/RtSelfTest.cpp` | `:1180-1198` の既定値固定 (`tableOk` / `svgfHistory` :1192 / `spatial` :1197)。定数を変えたら一緒に |
| `docs/adr/ADR-016-restir-reflection.md` | 「S5 の結論」節を追加 |
| `engine_spec.md` | §6.4 / 現在形の golden 枚数 |
| `tests/golden/demo_render_rtrefl_restir.png` | 値を変えたときだけ |
| `CLAUDE.md` | 値を変えたときだけ |
| 一時スクリプト | `scratchpad` 配下 (リポジトリに残さない)。画像は `tests\actual\` |

## 受け入れ条件 (このサブ)

1. **A9**: 回転体表面の M > 1 と firefly なしを数値で示し、review-1 の「J ≠ 1 は GPU で未観測」を閉じた
   (または閉じられない理由を報告した)。
2. **A10**: 軸 B の健全性を**本測定より先に**確認した記録がある。2 軸 4 条件 (以上) の表がある。
3. **A11**: 変えた値・変えなかった値の**全部**に「どの規則がどの数値で要求した / 要求しなかったか」が付いている。
   憶測で変えた値が 1 つも無い。
4. **A12**: 代表 1 条件の 2 run が `--img-diff --tol 0` PASS。
5. **A1**: `shot_verify.bat` 24 枚全緑。値を変えたなら `demo_render_rtrefl_restir` の 1 枚だけが差し替わり、
   `git status` が他 23 枚の不変を示す。3 run tol=0。
6. **A13**: ADR-016 / engine_spec が更新され、**未検証のまま残る条件が明記**されている。
7. **A14**: `tools\replay_verify.bat` 全緑 (M67h 全体の sim 非接触の担保)。
8. **A15**: Debug / Release とも `/p:MyeWarnAsError=true` で警告 0、`check_rules.ps1` 0/0。

## 検証コマンド

```
rem 基準線 / 最終確認
tools\shot_verify.bat
tools\replay_verify.bat
bin\x64\Debug\Editor.exe --selftest
pwsh -File tools\check_rules.ps1

rem 測定 (Release / WARP。1 run = 1 フレーム。--shot-every は使わない)
bin\x64\Release\Runtime.exe --render-demo --deferred --rt-refl [--rt-restir] [--rt-class-override N] ^
  --rt-debug 11 --rt-anim-seed [--rt-no-temporal --rt-no-svgf] ^
  --warp --no-audio --font-embedded --width 960 --height 540 --frames 41 --shot-frame 40 ^
  --no-fxaa --screenshot tests\actual\<name>.png

rem GPU 時間を見るときは撮影とは別 run (--frames 20。GpuTimer は 7 フレーム目からしか回収しない)
```

- `Runtime.exe` / `Editor.exe` は GUI サブシステム。**PowerShell からは `cmd /c` を挟む**
  (挟まないと待たずに戻り、古いログを読んで誤診する)。
- 画像の数値化は一時 Python (scratchpad)。M67 の A6/A7 と同じ道具立て。
- 長い run は背景実行し、途中経過を実装メモに積む。想定と桁が違ったら planner へ相談する。

## 実装メモ (coder が追記)

## フィードバック履歴
