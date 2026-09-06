# review-1: M68 音響伝播 × 実オーディオ

- 対象: 8e4272e..HEAD (= c29f7b3 M68a / 01183b1 M68b / 8994b1d M68c)
- 日付: 2026-09-07
- 基点: master、作業ツリーは harness.md / sub-03.md (司会の更新) 以外 clean。レビュー中の実行で追跡ファイルは 1 つも動いていない (`git status --short` で再確認)

```
REVIEW: PASS
round: 1
軸 (1-5):
  製品の深度: 4 — 受け入れ条件の外側を 5 通り突いて全部持ちこたえた: (i) 既定デモ (AcousticAudio 無し) に
    `--acoustic-audio-log 300` → `[acaudio]` 0 行 = 既存シーンは 1 行も走らない (ii) 3000 tick の長回し →
    shots 262 / playFailed 0 / dropped 0 / WARN 0、dPath ≥ 0.99·dLine の違反 0 / 3259 行 (iii) `--replay-record`
    200 tick → 0 行、その .rep の `--replay-verify` → VERIFY PASS かつ 0 行 (記録/検証ゲート) (iv) Editor 経路
    (`--autoplay`、実オーディオ) でも summary が出て 1 shot が Direct で鳴った (v) Inspector に 22 フィールド全部が
    既定値どおり出る (en/ja)。4 に留めるのは、実行中の `enabled` off→on と `roomProbeM` 変更という「Inspector で
    触る」運用そのものに小さな穴が残る (指摘 1, 2) ことと、`kProbeCellBudget` の縮退経路が一度も実行されていない (指摘 3)
  機能性: 4 — A1〜A26 を全部自分で回して成立 (下記「検証した手段」)。A11 は 650 行が sub-02 round 2 の報告値と
    数値まで一致、2 run バイト一致、A16 (a)〜(f) 予測どおり (room 0.02、playFailed 0)。A26 (b) の物理マウス再現だけは
    ユーザーの前面アプリを奪わずには不可能なので未実施 (planner が採用済みの注入プローブに委ねる)。5 にしないのは
    指摘 1 が spec §4.1.2 の明文 (「有効な AcousticAudio が無い tick は全部 Bypass」+「Bypass のとき *smooth = {}」)
    からの逸脱であるため (影響は enabled 復帰直後の ~100 ms)
  ビジュアルデザイン: 4 — Inspector の AcousticAudio を en/ja で撮って見た (下記画像 3 枚)。22 フィールドが spec §4.2 の
    表と同じ並び・同じ既定値 (96 / 8.000 / 0.250 / 0.150 / 0.100 / 6 / 6.000 / 0.300 / 0.800 / 18 / 3 / 6 / 0.250 /
    1.000 / 0.100 / 0.350 / 0 / step_soft..metal)。ja の表示名は MYE_JP どおりだが 22 本中 15 本がラベル列で切れて
    読めない (指摘 5)。Profiler の `acoustic-audio:` 行と Mixer の「(音響が上書き中)」は**画面では未確認** —
    前者は既定レイアウトで Stats タブの裏 (profiler.png)、後者は Window メニューからしか開けず、入力注入はしない
    方針なので N/A。両方ともコード読み (ProfilerWindow.cpp:121-131 / AudioMixerWindow.cpp:398-405 = HasReverbBus
    分岐の内側、wet/dry の隣) と summary (`active` 経路が実走) で担保
  コード品質: 4 — 規約どおり (日本語で「なぜ」と踏んだ罠 / 層構造 = Engine/Audio → Engine/Acoustic の一方向 /
    規則は ShapeAcousticSpatial・MakeWaveShotPlay・ApplyReverbParams の各 1 本 / 末尾 append / NoHash)。
    `check_rules.ps1` 0/0、8 ビルド `/p:MyeWarnAsError=true` 0 警告、C# 両構成 `TreatWarningsAsErrors` 0 警告。
    「仕様との差分」に無い変更は diff 全読 (54 ファイル) で見つからなかった。selftest 45 本目は T1〜T21 (70 アサート)
    で不変条件を機械化している。4 に留めるのは指摘 3 (縮退経路のテスト無し) と、台帳に既にある nit
    (Update 200 行超 / 冗長な前方宣言)
指摘:
  1. [minor] 宛先: coder — 有効な AcousticAudio が無い tick (`acOn == false`) で voice の平滑化状態 `st.shape` を
     落としていない。spec §4.1.2 (spec.md:157) は「有効な AcousticAudio … が無い tick は全部 Bypass」、手順 7
     (spec.md:149-150、変更履歴 #4) は「Bypass のときは *smooth = {}」 — だが `*smooth = {}` を書くのは
     ShapeAcousticSpatial の bypass ラムダ (AcousticAudio.cpp:280-287) だけで、AudioSourceSystem.cpp:588 の
     `if (acOn) { ShapeAcousticSpatial(...) }` に else が無いので、acOn が偽の tick は関数自体が呼ばれず状態が残る
     (`st.shape = {}` は :319 / :532 / :550 の 3 箇所 = 非 usable / stream / 再生開始のみ)。
     — 根拠: 上記 file:line + spec の文言。再現は test_checklists M68 の「`AcousticAudio.enabled` を off → on」で、
     復帰直後の最大 ~100 ms (smoothTicks 6) が前回の遮蔽値からのランプになる (変更履歴 #4 が防ぎたかった現象そのもの)。
     機械検証はしていない (Inspector の切替が要る)
     — 期待: `if (acOn)` の else で `st.shape = {}` (または Bypass 経路を通す)。T12 に「acOn 偽の tick を挟むと次はスナップ」
     を 1 本足す
  2. [minor] 宛先: planner — `roomProbeM` を実行中に変えても openness が再計算されない。UpdateAcousticProbe の再構築判定
     (AcousticAudio.cpp:233-236) は 原点セル / requestRing / 署名 / grid だけを見て、openness は再構築時 1 回
     (:261) しか計算しない。spec §4.1.1 (spec.md:111、変更履歴 #3) は `probeMaxRing 変化` を「Inspector で実行中に
     触れるため」契機に足したが、同じ理由が当てはまる `roomProbeM` は挙げていない (仕様どおりの実装 = 仕様の穴)。
     ADR-017 / test_checklists / CLAUDE.md は「調整値は Inspector で実行中に触る」運用を勧めている
     — 根拠: 上記 file:line + spec §4.1.1「再構築時に 1 回計算し probe に保持」。リスナーが同じセルに立ったままだと
     残響の t が変わらず、次にセルをまたいだ瞬間に跳ぶ
     — 期待: spec §4.1.1 に「`roomProbeM` 変化 → openness だけ再計算 (場の焼き直しは不要)」を足し、coder が
     probe に要求値を持たせて比較する (1 行の契機 + T11 に 1 アサート)
  3. [minor] 宛先: coder — `kProbeCellBudget` の縮退経路 (AcousticAudio.cpp:241-250、ring を半分ずつ下げて 1 回だけ
     警告) を固定するテストが無い。T1〜T21 のグリッドは最大 24×4×24、デモは 16224 セルなので、この経路は
     セルフテストでもデモでも一度も実行されていない (Occluded へ倒す挙動と `budgetWarned` の 1 回限りも未検査)
     — 根拠: AcousticAudioSelfTest.cpp の MakeMazeGrid / MakeFreeGrid (24×1×24 / 24×4×24) と kProbeCellBudget = 262144
     — 期待: 自由空間 80×48×80 (307200 セル) 等で `probe.maxRing == 48 (要求 96)`、`budgetWarned`、遠い音源が Occluded
     になることを 1 本 (T22) で固定する
  4. [minor] 宛先: planner — (M65b からの既存挙動だが M68 の推奨経路に乗った) `Editor.exe --acoustic-demo` の
     `scenePath_` が `assets\scenes\main.scene.json` に落ちる。EditorApp.cpp:99-134 の分岐に acousticShowcase
     (と particleShowcase) が無く :134 の else へ行く。撮った inspector_tall.png のステータスバーにも
     「main.scene.json」と出ている。今は同ファイルが無い (Test-Path False) ので :170 の「存在すれば Load」に
     引っかからず組み立てに行くが、この session で Ctrl+S すると main.scene.json が生まれ、以後の `Editor.exe`
     (既定デモ = replay 1 ペア目) が BuildDemoScene ではなくそれを読む (:170-171)。test_checklists.md:118 は
     `Editor.exe --acoustic-demo → Play` を勧めており、:102 は同じ罠を `--rt-demo` については既に警戒している
     — 根拠: 上記 file:line + 画像 `...\scratchpad\inspector_tall.png` 右下
     — 期待: M65 追補候補として台帳へ (`cache\acoustic_showcase.scene.json` 等、他ショーケースと同じ振り分け)。
     M68 の範囲では test_checklists の M68 節に「Ctrl+S しない」の一言があればよい
  5. [minor] 宛先: coder — Inspector の日本語表示名が既定のラベル列幅で切れて読めない: 22 本中 15 本
     (「リスナー場の半」「回折が飽和す」「回折 LPF の下限」「狭い側のプリ」「広い側のプリ」「回り込みの残」
     「波の減衰カー」「音色 0 のサウ」…)。en でも `roomSmoothTick` が切れる。隣の「残響の半減期」「整形の半減期」
     (6 文字) は収まる
     — 根拠: 画像 `C:\Users\akita\AppData\Local\Temp\claude\C--HAL-MyEngin\13475dda-dfa1-41dd-8100-fba4a8496a59\scratchpad\inspector_ja.png`
     (`Editor.exe --acoustic-demo --select "Acoustic Audio" --lang ja --warp --no-audio --width 1600 --height 1500
     --frames 6 --shot-frame 4 --screenshot`)、同 `inspector_tall.png` (en)。ツールチップの有無は未確認
     — 期待: 表示名を 6〜7 文字に詰める (例: 「場の半径」「回折飽和長」「LPF 下限」「狭い側の響き」「回り込み送り」
     「減衰カーブ」「音色 0 の音」)。列幅を広げる案は Inspector 全体の変更なので planner 判断
検証した手段:
  - 読んだ範囲: spec.md / sub-01〜03.md / harness.md / plan-original.md 進捗表、`git diff 8e4272e..HEAD` の
    コード 2852 行 (src + build) と文書 931 行 (CLAUDE.md / README / engine_spec / ADR-017 / test_checklists /
    assets/audio の json・meta / supple-weaving-loom) を全読。周辺の既存コード: AudioSourceSystem.cpp:505-620、
    AudioSystem.cpp:1472-1500 (Update の null ガード)、AudioMixerWindow.cpp:360-411、AcousticField.cpp:117-124 (IsSolid の
    範囲外 = 壁)、EditorApp.cpp:60-215、EditorMain.cpp:150-290
  - 8 ビルド: `MYE_MSBUILD_ARGS=/p:MyeWarnAsError=true` を立てて `tools\replay_verify.bat` → ビルド通過 (警告があれば
    落ちる) + `[PASS] 10 jobs / 7 scenes / 119.5s` (A2 / A4 / A5 C++ / A26 (d))。bin の exe が HEAD より古かった
    (Debug Editor 23:53 < M68c 00:25) ので、以下は全部この再ビルド後のバイナリで回した
  - `tools\build_managed.bat Debug` / `Release` (`MYE_DOTNET_ARGS=/p:TreatWarningsAsErrors=true`) → 両方 exit 0、
    警告行 0 (A5 C#)
  - `cmd /c bin\x64\Debug\Editor.exe --selftest` → exit 0、45 本目 "Acoustic audio self test: ALL PASS"
    (`FAIL:` にマッチした 40 行は schema セルフテストの意図的な負例ログ) (A1 / A8 / A10 / A15 / A18 の Localization)
  - `pwsh -File tools\check_rules.ps1` → 0 error / 0 warning (単体 + replay_verify 内の 2 回)
  - `tools\shot_verify.bat` ×2 → 2 回とも `[PASS] 22 shots`、img-diff 22 行すべて maxDiff=0 diffPixels=0
    (A3 / A26 (a)。撮影中に私は入力を一切注入していない)
  - A11 / A16: `bin\x64\Release\Runtime.exe --acoustic-demo --synth-input --acoustic-audio-log 600 --screenshot
    <scratch>\acaudio_N.png --shot-frame 600 --frames 601 --font-embedded` ×2 → 650 行 (voice 599 / shot 51)。
    voice class Detour=599 / Direct=Occluded=Bypass=0、dPath ≥ 0.99·dLine 違反 0 / 649 行 (t ≥ 2)、Hum dPath 26.64〜30.73、
    Hum max|Δgain|=0 / max|Δlpf|=0.060、shot class Direct 23 / Detour 28、tone 7/33/4/7、src 88/96/97/100/101、
    max|Δroom|=0.03 (t ≥ 2)、summary `ticks=601 rebuilds=66 boxCells=16224 shaped=652 classes D/T/O/B=23/629/0/0
    shots=51 skipped=0 unknownKey=0 dropped=0 room=0.02 playFailed=0`、2 run の `[acaudio] t=` 650 行が
    Compare-Object で差分 0 = バイト一致 (A26 (c) も同値: sub-02 round 2 / sub-03 の報告値と全欄一致)、
    `[audio] unknown sound key` 0 件、`loaded clip: hum,step_hard,step_metal,step_soft,step_wood` (A14)
  - A12: 同コマンド + `--no-audio` → `[acaudio]` 0 行 (summary 含む)
  - 深度 (i): `Runtime.exe --synth-input --acoustic-audio-log 300 --screenshot ... --frames 301` (既定デモ) → 0 行
  - 深度 (ii): 同 A11 で `--acoustic-audio-log 3000 --shot-frame 3000 --frames 3001` → 3260 行、shots 262
    (Direct 232 / Detour 29)、summary `rebuilds=284 ... unknownKey=0 dropped=0 room=0.10 playFailed=0`、WARN/ERROR 0
  - 深度 (iii): `bin\x64\Release\Editor.exe --acoustic-demo --synth-input --replay-record <scratch>\ac.rep
    --replay-ticks 200 --replay-fast --acoustic-audio-log 200` → `recorded 200 ticks`、`[acaudio]` 0 行;
    `--replay-verify <scratch>\ac.rep --acoustic-audio-log 200` → `VERIFY PASS: 200 ticks hash-identical`、0 行 (A17)
  - 深度 (iv): `Editor.exe --acoustic-demo --autoplay --warp --font-embedded --width 1600 --height 1000 --frames 40
    --shot-frame 39 --acoustic-audio-log 40 --screenshot <scratch>\profiler.png` (実オーディオ) → summary
    `ticks=40 rebuilds=2 ... classes 1/40/0/0 shots=1 playFailed=0` (A13 の active 経路)
  - 画像: `<scratch>\inspector.png` (en 1600×1000) / `<scratch>\inspector_tall.png` (en 1600×1500、22 フィールド全部) /
    `<scratch>\inspector_ja.png` (ja) / `<scratch>\profiler.png` (Play 中、Stats タブが前面で Profiler 行は見えない)。
    `<scratch>` = `C:\Users\akita\AppData\Local\Temp\claude\C--HAL-MyEngin\13475dda-dfa1-41dd-8100-fba4a8496a59\scratchpad`
  - 機械照合: A6 `git diff 8e4272e --stat -- src/Engine/Engine/Acoustic` = `AcousticField.h | 2 +-` のみ / A7 空 /
    `MYE_API_VERSION 15u` / `kSimSnapshotVersion = 11` / A20・A21 の grep 空 / A22 存在 / A23 test_checklists.md:115 /
    A25 plan-original.md:346-354 / 5 音源の `.wav.meta` guid (hex) と `.sound.json` の clip (dec) が全部一致、
    assets\audio の 17 個の .meta に guid 重複なし (S11)
  - 未実施 (理由つき): A26 (b) 物理マウスでの再現 (前面アプリを奪わない方針、planner が注入プローブで採用済み) /
    耳の確認 (docs/test_checklists.md M68 節 = ユーザーの領分) / Profiler 行と Mixer 注記の目視 (入力注入をしないため N/A)
前回指摘の消込: (round 1 なので無し)
```
