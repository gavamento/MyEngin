# sub-03: 仕上げ — ADR-017 / engine_spec §10.6 / README / test_checklists / CLAUDE.md / 進捗表 (M68c)

- 依存: sub-02
- 状態: OK (commit 8994b1d)
- 往復: 1

## やること

文書だけ。コードは触らない (A1〜A9 は「無風の再確認」として回す)。

1. `engine_spec.md` §10.6 末尾 (`:1655` 付近の `---` の前、`## 11.` の直前) に `**Audible output (M68).**` 段落 (既存の
   「太字リード + 散文」様式、小節は作らない)。内容: 判断 1〜5 の要旨 (3 本目の Dial 写し / 仮想発音位置 / 4 クラス /
   2 アンカー補間 / `PendingWaveShot`)、**sim 状態ゼロ**の主張、振幅 = √エネルギー (spec S2)、`--acoustic-audio-log`。
   `:1701` "all six existing replay pairs and all seventeen golden images" / `:1703` "six scene pairs" / `:1742` "six replay pairs" を
   現行 (seven / twenty-two) に直す。
2. `README.md` `## 主要機能` に M65 + M68 をまとめた 1 bullet (「波面の 4 役目」を 1 文で)。`:91-100` の「被覆は 6 シーン」→ 7
   (音響ショーケースを列挙に足す)、`:119-123` の「リプレイ照合 6 ペア」→ 7。
3. `docs\adr\ADR-017-acoustic-audio.md` (ADR-016 の様式: 冒頭メタ / `## 決定` / `## 理由` (`### なぜ…` に却下案) / `## 帰結`
   (決定論と後方互換 / コスト (実測) / 既知の制限))。M65 に ADR が無いので「波面の 3 役 (整数チャンファ・sim 状態は波表だけ)」の
   決定を 1 節で拾う。却下案は spec §2 から: 共通化 vs 写し (判断 1) / 振幅 vs エネルギー (S2) / 直付け vs 子 (S7) /
   到来方向 1 歩 vs 平均 (S16) / Play 時 1 回 vs 追従 (S17) / override の置き場 (判断 4) / push を tick 側にする理由 (判断 5)。
   実測値は sub-01 / sub-02 の実装メモ (probe ms、box cells、shots) から写す。
4. `docs\test_checklists.md` 末尾に `## M68: 音響 × オーディオ (耳で確認)` 節 (`- [ ] 操作 → 期待`):
   起動 (`Editor.exe --acoustic-demo` → Play、または `Runtime.exe --acoustic-demo`) / 部屋 A で hum がこもって小さい /
   横の廊下を東へ歩くと開いてくる / 縦の廊下で戸口側 (前方) に定位 / 戸口で素通し / 廊下と部屋 B で残響が段差なく変わる /
   Walker の足音が金属で遠くまで・カーペットで数歩で消える / 箱の落下音 / 敵の自発音 / Q 石・E 瓶 / 呼吸は鳴らない /
   ミキサー窓に「音響が上書き中」・combo は資産値のまま / `AcousticAudio.enabled` を切ると全部素通しに戻る。
5. `CLAUDE.md`: 検証表の selftest 行 (45 スイートは M68a で済み — 記述の整合を再確認)、CLI 一覧の `--acoustic-audio-log N` の説明を
   仕上げ、「横断的な変更のチェックリスト」に音響 × オーディオの 1 段落 (`AcousticField` を読むのは出力レーンだけ /
   `ShapeAcousticSpatial` の 1 本 / 波の spatial は rolloff 0)、TypeId 末尾 **50 = AcousticAudio** (Cloth/SoftBody 51/52) の記述。
6. `plan-original.md` の進捗表 (3 行) と申し送り。`harness.md` の申し送りに「メモリ (`myengine-project.md`) の現在地更新は司会」と書く
   (リポジトリ外なので coder は触らない)。
7. **コードの唯一の例外 (spec S24 / 変更履歴 #15)**: `EngineLoop.cpp` の入力レーン確定 (`:1287` 付近、`--synth-input` の置換と
   同じ場所) で、`deterministicShot` のとき**生デバイス由来のレーン 0** の `mouseDeltaX` / `mouseDeltaY` を 0 にする。
   置く位置は `--synth-input` / .rep の置換より**前** (合成入力と記録入力のデルタは殺さない — A11 のレシピと replay 7 ペア目の
   記録側 `--synth-input` が視点角の被覆に使っている)。コメントに「frame == tick と同じ撮影モードの決定化。`WatcherFpsCamera` が
   デルタを yaw に積分して MeshRenderer 付きの箱を回すので、撮影中に机を触ると acoustic の golden が割れた (M68b で実測)」を書く。
   キーボード / マウス位置は触らない (`ui_probe` 等の golden がマウス位置に依存していないことは未確認なので広げない)。
   `CLAUDE.md` の「環境の罠」に 1 行 (「決定的撮影モードは生マウスデルタを 0 にする。acoustic の golden は M68c 以前は撮影中の
   マウスで割れた」)。`engine_spec.md` の決定的スクショの記述 (M52c の段落、`grep -n "deterministic" engine_spec.md`) に半文を足す。

## やらないこと (このサブでは)

- コードの変更 (**例外は 7 の 1 行だけ**)。調整値の焼き込み (ユーザーが耳で決めた後の別コミット)。golden の更新 (動いていたら
  それは M68a/b のバグ、または 7 が合成入力まで殺している = A26 (c) で検出)。

## 触る場所 (planner の見立て)

| ファイル | 場所 |
|---|---|
| `engine_spec.md` | `:1640-1655` (§10.6 末尾)、`:1701`、`:1703`、`:1742` |
| `README.md` | `## 主要機能` (`:91-100`、`:119-123`) |
| `docs\adr\ADR-017-acoustic-audio.md` | 新規 |
| `docs\test_checklists.md` | 末尾 (M46 節の後) |
| `CLAUDE.md` | 検証表 / CLI / チェックリスト / TypeId |
| `plans\m68-acoustic-audio\plan-original.md` | 進捗表 |
| `src\Engine\Engine\EngineLoop.cpp` | `:1287` 付近の入力レーン確定 (7 の 1 行 + コメント) |
| `CLAUDE.md` 環境の罠 / `engine_spec.md` の決定的スクショの段落 | 7 の 1 行ずつ |

## 受け入れ条件 (このサブ)

spec §5 の A1〜A9 (再確認) と A20〜A26。加えて `pwsh -File tools\check_rules.ps1` (文書は対象外だが習慣)。
A26 の内訳: (a) `shot_verify.bat` ×2 で 22 枚 maxDiff=0 (b) `acoustic_deferred` の撮影コマンド (bat の `SHOTBASE` + `--acoustic-demo
--deferred --frames 121 --shot-frame 120`) を、マウスを動かしながら回して golden と `--img-diff --tol 0` PASS — 自動化は PowerShell の
`SendInput` (`MOUSEEVENTF_MOVE`、`user32.dll` を `Add-Type` で呼ぶ) を撮影中ループさせる。`WM_INPUT` に乗らなければ手で 1 回
(方法と結果を実装メモに。**変更前のバイナリで同じ手順が割れること**も 1 回確かめて「故障点で検証した」形にする)
(c) A11 のレシピ (`--synth-input --screenshot`) の `[acaudio] t=` 行が sub-02 round 2 の run と一致 (合成入力のデルタが生きている証拠。
round 2 のログは coder の scratchpad に無ければ司会に問う。無ければ変更前バイナリで 1 回撮って比較) (d) `replay_verify` 7 ペア無風。

## 検証コマンド

```
grep -n "six replay\|seventeen golden\|six scene" engine_spec.md      # 空
grep -n "6 シーン\|6 ペア" README.md                                    # 空
ls docs/adr/ADR-017-acoustic-audio.md
grep -n "^## M68" docs/test_checklists.md
cmd /c bin\x64\Debug\Editor.exe --selftest / tools\replay_verify.bat / tools\shot_verify.bat ×2 / check_rules (無風の再確認)
A26 (b): cmd /c bin\x64\Release\Runtime.exe --acoustic-demo --deferred --warp --no-audio --font-embedded --width 960 --height 540 --frames 121 --shot-frame 120 --no-fxaa --screenshot <scratch>\wiggle.png   (撮影中にマウスを動かす) → cmd /c bin\x64\Debug\Editor.exe --img-diff tests\golden\acoustic_deferred.png <scratch>\wiggle.png --tol 0
A26 (c): sub-01 の A11 コマンドをそのまま → `[acaudio] t=` 行を sub-02 round 2 のログと fc
```

## 実装メモ (coder が追記)

### round 1

```
SELF_EVAL: sub-03 (round 1)
実装:
  - engine_spec.md §10.6 末尾 — **Audible output (M68).** を 6 段落 (既存の「太字リード + 散文」様式、
    小節なし)。判断 1〜5 の要旨 (3 本目の Dial / 仮想発音位置と doppler 0 / 4 クラス /
    2 プリセット連続補間と ApplyReverbParams 1 箇所 / PendingWaveShot の push=tick・drain=frame)、
    振幅 = √エネルギー (S2) と「gain² · amplitude == EnergyAt を固定する」、
    到達範囲 = 波の到達範囲 (RolloffGain が maxD で厳密 0)、**sim 状態ゼロ**、`--acoustic-audio-log`
  - engine_spec.md — 古い数を訂正: `:1701` six/seventeen → 「predates it (seven pairs and
    twenty-two images today)」/ `:1703` six scene pairs → **seven** + 列挙に**音響ショーケース**を追加
    (記録側 --synth-input の理由つき) / `:1742` six replay pairs → seven
  - engine_spec.md 決定的スクショ (M52c) — 「two guarantees」→ **three** + 3 つ目の bullet
    「The device on the desk (M68c)」(生マウスデルタを 0 にする / 合成入力と .rep は後で置換されるので
    無傷 / キーとカーソル位置は触らない)
  - README.md `## 主要機能` — 音響 (M65 + M68) の bullet 1 本 (波面の 4 役、仮想発音位置、
    残響の連続補間、鳴る範囲 = 波の到達範囲、sim 状態は波表だけ、ADR-017 へのリンク)。
    「被覆は 6 シーン」→ **7** (音響ショーケースを列挙に追加)、CI の「リプレイ照合 6 ペア」→ **7**、
    ツール表の「(6 シーン被覆)」→ **7**、`:170` の「replay 6 ペア目」→「replay ペアの 6 本目」
    (A21 の grep を空にするための言い換え。序数の意味は同じ)
  - docs/adr/ADR-017-acoustic-audio.md (新規) — ADR-016 の様式 (冒頭メタ / ## 決定 / ## 理由 の
    `### なぜ…` / ## 帰結 の 決定論と後方互換・コスト (実測)・既知の制限)。M65 に ADR が無いので
    「決定 0」として整数チャンファと「sim 状態は波スロット表だけ」を先に置いた。却下案は
    共通化 vs 3 本目の写し / 逆二乗をそのまま volume に掛ける / 直付け vs 子エンティティ /
    到来方向の平均 (U5) / 追従整形 (U6) / override の置き場 / push を tick 側にする理由 /
    アンカーの値 (実測 9 点の表) / probeMaxRing 96 / minWaveVolume 0.10 /
    S24 の 3 案 (採用 a、却下 b 別コミット・c 放置)
  - docs/test_checklists.md — `## M68: 音響 × オーディオ (耳で確認)` (4 小節 30 項目、`- [ ] 操作 → 期待`)。
    遮蔽と回折 / 部屋の残響 / 鳴る波 / **壊れ方の切り分け**。申し送りどおり
    「耳で聞こえないのに playFailed=0 なら XAudio2 側を疑う」と
    「壁越しの hum は Detour であって Occluded ではない」を明記
  - CLAUDE.md — CLI の `--acoustic-audio-log` を M68b 後の実態に (kind=shot / summary の欄順 /
    2 run バイト一致)、「決定論の契約」の撮影モードの列に生マウスデルタ 0 を追記、
    「横断的な変更のチェックリスト」に **音響を耳に出すものを触る (M68)** の段落
    (AcousticField を読むのは出力レーンだけ / 規則は ShapeAcousticSpatial の 1 本 /
    波の spatial は rolloff 0・doppler 0・maxDistance = maxRing·cellSize /
    override は ApplyReverbParams の 1 箇所で reverbPreset_ に書き戻さない)、
    「環境の罠」に撮影中のマウスの罠 + **raw input は RIDEV_INPUTSINK 無し = 前面でないと届かない**
  - plans/m68-acoustic-audio/plan-original.md — 進捗表 3 行を「完了 + 計画外の事実」で更新し、
    「### 申し送り (M68 完了時点)」を新設 (調整値は未確定 / M65 追補候補 / probe の最適化余地 /
    耳で未確認の主張 / メモリ更新は司会)
  - src/Engine/Engine/EngineLoop.cpp — **コード 1 箇所 (2 行 + 日本語コメント)**: `deterministicShot` の
    とき `ctx.inputs[0].mouseDeltaX/Y = 0`。置いたのは**入力レーンのキャプチャ直後**
    (`input.CaptureSnapshot()` ループの直後、`netLiveInput` の退避より前)
仕様との差分:
  - [逸脱] 置き場所: sub-03.md 7 は「`:1287` 付近、`--synth-input` の置換と同じ場所」と書いているが、
    実装は**フレーム頭のキャプチャ直後** (`:1163`) に置いた。理由 3 つ: (i) そこが「生デバイス由来」
    そのもの (CaptureSnapshot の戻り値をその場で潰す) (ii) tick ループの先頭に置くと**ネット経路で
    無効化される** — `net.InputsFor()` が全レーンを上書きするので、自分が送った生デルタが相手経由で
    戻って復活する。キャプチャ直後なら `netLiveInput` の退避より前なので送信値も 0 で揃う
    (iii) フレームに 1 回で済む。spec S24 の必須条件「`--synth-input` / .rep の置換より**前**」は
    満たしている (A26 (c) で機械確認)
  - [追加] engine_spec の決定的スクショは「半文」ではなく **bullet 1 つ**にし、リード文の
    「two guarantees」を **three** に直した。既存が箇条書きなので、半文を差し込むと数が合わなくなる
  - [追加] engine_spec の replay ペア列挙に音響ショーケースの説明を足した (数字だけ 7 に直すと
    列挙が 6 個のまま残る)
  - [追加] README `:170` の「replay 6 ペア目」→「replay ペアの 6 本目」。A21 の grep
    (`6 ペア`) を空にするための言い換えで、意味 (関節ショーケースが 6 本目) は変えていない
  - [追加] CLAUDE.md は「環境の罠」(sub-03 の指定) に加えて「決定論の契約」の撮影モードの列
    (`--warp` / `--font-embedded` / frame == tick / drain) にも 1 文足した。同じ列挙なので
    片方だけだと食い違う
  - [追加] test_checklists の M68 節に「壊れ方の切り分け」小節 (playFailed / unknownKey / dropped /
    Bypass / 2 run バイト一致)。sub-03 の項目表には無いが、申し送りの
    「playFailed=0 なら XAudio2 側」を単独で書くと読み手が使えないため
  - [追加] ADR-017 に「決定 0」(M65 の土台) を置いた — sub-03 の指示どおりだが、ADR-016 の様式には
    無い形なので明記しておく
  - [未実装] なし。sub-03 の 6 「`harness.md` の申し送りに『メモリ更新は司会』」は**既に
    planner が書いている** (`harness.md:41`) ので触っていない (harness.md は司会・planner の領分)
検証:
  - cmd /c bin\x64\Debug\Editor.exe --selftest → exit 0 / FAIL 0。45 本目 "Acoustic audio self test:
    ALL PASS" (T1〜T21) まで緑 (A1)
  - tools\replay_verify.bat → [PASS] 7 シーン (demo + parts + flow + mp + physics + joints + acoustic)
    + snapshot 往復 + タイムトラベル ×2 + 規則検査、exit 0 (A2 / A26 (d))
  - tools\shot_verify.bat ×2 → 2 回とも exit 0 / [PASS] 22 shots。img-diff 44 行すべて
    **maxDiff=0** (A3 / A26 (a))
  - pwsh -File tools\check_rules.ps1 → 0 error / 0 warning (A4。ビルド前後で 2 回)
  - MSBuild Debug/Release x64 `/p:MyeWarnAsError=true` → 両方 exit 0 / 0 warning、
    tools\build_managed.bat Debug / Release → 両方 exit 0 (A5)
  - pwsh -File tools\gen_project_files.ps1 → `.vcxproj` に diff なし (新規ソースなし)
  - git diff 8e4272e --stat -- src/Engine/Engine/Acoustic → `AcousticField.h | 2 +-` の 1 行のみ (A6)
  - git diff 8e4272e -- src/Shared/EngineAPI.h src/Scripting → 空 (A7)
  - A8/A9: 今回フィールドを 1 本も足していない (コード差分は EngineLoop の 2 行だけ)。
    TypeId 50 / NoHash / snapshot 版 11 は A1 の T1 が緑
  - A20: `grep -n "six replay\|seventeen golden\|six scene" engine_spec.md` → **空**
  - A21: `grep -n "6 シーン\|6 ペア" README.md` → **空**
  - A22/A23/A25: `docs/adr/ADR-017-acoustic-audio.md` 存在 (決定 / 理由 / 帰結)、
    `grep -n "^## M68" docs/test_checklists.md` → 115 行目、plan-original の進捗表 3 行 + 申し送り節
  - **A26 (b) 故障点で検証 (2 段)**:
    (0) `SendInput` (MOUSEEVENTF_MOVE) の経路は**この環境では成立しなかった**。raw input は
        `RIDEV_INPUTSINK` 無しで登録されている (`Input.cpp:167`) ので、Runtime のウィンドウが
        **前面でないと WM_INPUT が 1 通も届かない**。実測: 撮影中の 608 サンプルすべてで
        `GetForegroundWindow() != Runtime` (前面はユーザーが実行中の全画面ゲーム)、
        `SendInput` を 1258 回 / 608 回撃った 2 回の撮影とも golden と maxDiff=0。
        **フォーカスの強奪はしなかった** (ユーザーのセッションを壊すため。
        `AttachThreadInput` + `SetForegroundWindow` は False を返した)。
        ★この 2 回のマウス移動は**前面のアプリに入ってしまった** — 前面化できない環境で
        この手を使ってはいけない、を CLAUDE.md の罠に書いた
    (1) 代わりに `Input::CaptureSnapshot` (= 修正点より**1 段上流**) で環境変数
        `MYE_FAKE_MOUSE_DELTA` のぶんだけ生デルタを合成する**使い捨てプローブ**を入れた。
        **修正前のコード + 注入 8 カウント/tick** で `acoustic_deferred` の撮影コマンド
        (bat と同一: `--warp --no-audio --font-embedded --width 960 --height 540 --frames 123
        --shot-frame 120 --no-fxaa --acoustic-demo --deferred`) を回すと
        **FAIL: maxDiff=127 diffPixels=1612 worst pixel (236,446)** = M68b が実測した
        フレーク (maxDiff 125 / 520 px / (236,444) = 部屋 A の隅の Watcher の箱) と同じ場所・同じ形。
        同じプローブ build で環境変数を外すと **maxDiff=0** (プローブ自体は無害)
    (2) **修正後 + 同じ注入 → PASS: maxDiff=0**。プローブは `git checkout` で消し、
        最終ビルドは素のコード (作業ツリーに 1 バイトも残っていない)
  - **A26 (c)**: sub-02 round 2 のログは私の scratchpad に無かったので、**変更前バイナリを
    もう一度焼いて**同じ A11 レシピを 1 回撮り、直接比較した:
      修正前 / 修正後の `[acaudio] t=` 行 **650 行が byte-identical** (`cmp` で確認)。
      修正後の 2 run 同士も byte-identical。
      summary は 3 run とも `ticks=601 rebuilds=66 boxCells=16224 shaped=652
      classes D/T/O/B=23/629/0/0 shots=51 skipped=0 unknownKey=0 dropped=0 room=0.02 playFailed=0`
      (probeMsAvg のみ 0.634〜0.742 で run 依存) = **sub-02 round 2 の報告値と一致**。
      合成入力のデルタが生きている証拠として決定的 (Watcher の yaw は移動方向を決めるので、
      殺されていれば経路も rebuilds も class 内訳も変わる)
  - 撮影・ログ実行はすべて PowerShell ツールから `cmd /c`。exe の exit code も確認済み
自己採点 (1-5):
  仕様適合: 4 — sub-03 の 7 項目すべてに対応し、A20〜A26 は機械確認。5 にしないのは、
    コード 1 行の**置き場所を planner の見立て (`:1287` 付近) から動かした**こと (理由は上記。
    ネット経路で無効化される見立てだった) と、[追加] 6 件を自分の判断で入れたこと
  正しさ: 4 — A1〜A9 と A20〜A26 が全部緑。A26 (b) は「壊れる条件を作って壊し、直して直った」の
    2 点測定、A26 (c) は変更前バイナリとのバイト一致。5 にしないのは、**物理マウス (WM_INPUT) の
    経路そのものは検証できていない**ため (前面を取れない環境。注入点は WM_INPUT の 1 段下流の
    `CaptureSnapshot` の出口なので、修正が守る範囲は同じだが「実マウスで割れて実マウスで直った」は
    未実測)
  コード品質: 4 — コードは 2 行 + 「なぜ」を書いた日本語コメント。置き場所は生デバイスを読んだ
    直後の 1 箇所で、合成入力・記録入力・ネットの置換のどれとも交差しない。文書は既存様式に
    合わせた (ADR-016 の見出し構成 / §10.6 の太字リード + 散文 / test_checklists の `- [ ]` /
    README の bullet)。5 にしないのは engine_spec を英語で書いている以上、
    語り口の一致は主観判定でしかないため
  テスト: 4 — 自動検査は全部回した (selftest 45 / replay 7 ペア / golden 22 枚 ×2 / 規則検査 /
    8 ビルド 0 警告) うえで、A26 を故障点で 2 回測った。5 にしないのは、**この修正を守る恒久的な
    回帰テストが無い**こと: 「撮影モードでレーン 0 の生デルタが 0 になる」を固定できる場所は
    `EngineLoop::Run` の中で、ヘッドレスの selftest からは呼べない。将来同じ 2 行が消えても
    気づけるのは「golden がまた時々割れる」ときだけ
不安・質問:
  1. **A26 (b) の再現手段を替えたことの是非**。planner が「物理マウス / SendInput でなければ
     故障点と認めない」と判断するなら、ユーザーがマシンを空けたときに再実行が要る
     (手順: `scratchpad\wiggle_shot.ps1` = Runtime を起動 → 前面化 → SendInput ループ。
     前面化さえ通れば 1 コマンドで済む)。私は**ユーザーの全画面ゲームからフォーカスを奪う**
     選択を取らなかった
  2. **`--screenshot` を付けた対話実行では視点マウスが効かなくなる** (撮影モードの意味論としては
     正しいが、知らずに使うと「マウスが壊れた」に見える)。`[shot] deterministic capture:` の
     ログ行に「+ raw mouse delta zeroed」を足すか、`--shot-realtime` で解除できることを
     書き添えるか。今回は**黙って**いる (ログ文言の変更は指示に無いため)
  3. ADR-017 に「M65 の決定 0」を含めた結果、ADR の題が M68 単独より広くなっている
     (「音響の波面を耳へ出す」)。M65 の ADR を後から独立に立てるなら、決定 0 はそちらへ移すべき
触ったファイル:
  engine_spec.md
  README.md
  CLAUDE.md
  docs/adr/ADR-017-acoustic-audio.md   (新規)
  docs/test_checklists.md
  plans/m68-acoustic-audio/plan-original.md
  plans/m68-acoustic-audio/sub-03.md
  src/Engine/Engine/EngineLoop.cpp
  (`plans/m68-acoustic-audio/harness.md` と `sub-02.md` の差分は私ではなく司会のもの)
申し送り:
  - **README の CLI ブロックに `--acoustic-demo` / `--acoustic-audio-log` の行が無い**
    (M65 からの漏れ。今回は「主要機能」の bullet だけ足した)。足すなら
    `Editor.exe --joint-demo` の隣、「= replay ペアの 7 本目 + スクショ 16/17 枚目」の形で
  - engine_spec の M52c 段落は「frame の決定性に要る保証」が **3 つ**になった。
    4 つ目を足す人はリード文の数も直すこと (同じ段落に別勘定の
    「Three machine-dependent inputs are pinned」があるので混同しやすい)
  - **撮影モードの 2 行を守るテストは無い** (上記テスト欄)。golden がまた時々割れたら、
    まずここが消えていないかを見る
  - 一時プローブ (`MYE_FAKE_MOUSE_DELTA` を `Input::CaptureSnapshot` で足す 12 行) は
    リポジトリに 1 バイトも残していない。再現が要るなら実装メモの手順で復元できる
  - ユーザーの耳による確認 (`docs/test_checklists.md` の M68 節) と調整値の焼き込みは未実施 =
    M68 の残りはそこだけ
```

## フィードバック履歴

- round 1: **VERDICT OK** (planner、2026-09-07)。A1〜A9 / A20〜A26 を検証欄と実ファイルで確認 (A20 / A21 の grep が空、
  `engine_spec.md:1655` の段落と `:1908` の 3 つ目の bullet、README の bullet + ADR-017 リンク、ADR-017 の節構成、test_checklists の
  M68 節 4 小節、`EngineLoop.cpp:1163` の 2 行がキャプチャ直後 = `netLiveInput` 退避 (`:1182`) より前)。
  [逸脱] の置き場所は coder が正しい (planner の見立て `:1287` は tick ループ内で、ネット経路の `net.InputsFor()` に上書きされる。
  spec S24 の必須条件「置換より前」は満たす) → spec §8 #16 で採用。A26 (b) は物理マウスでなく `CaptureSnapshot` 出口への注入で
  故障点を再現 (修正前 FAIL maxDiff 127 / (236,446) = M68b の実測と同じ場所、修正後 PASS) — 修正が守る範囲は同じで、WM_INPUT →
  CaptureSnapshot の上流は M68b の実マウスの割れで既に証明されているので採用。フォーカスを奪わなかった判断も正しい。
  nit 3 件 (質問 2 の `[shot]` ログ文言 / 質問 3 の ADR 決定 0 / README の CLI ブロック) は申し送りへ。
