# sub-01: リスナー場 + 遮蔽・回折の整形 + `AcousticAudio` (TypeId 50) + selftest 45 本目 + hum (M68a)

- 依存: なし
- 状態: OK (round 1。commit は司会が記入)
- 往復: 1

## やること

spec §4.1.1 / §4.1.2 / §4.1.5 / §4.1.6 (M68a 分) / §4.2 (コンポーネント・`lpfCoefficient`・hum) を実装する。
**sim には 1 バイトも触らない** (`src\Engine\Engine\Acoustic\` に diff なし。このサブではコメント行も触らない)。

1. `src\Engine\Engine\Audio\AcousticAudio.h/.cpp` (新規): `AcousticProbe` (Dial の 3 本目の写し + 箱 + `kProbeCellBudget` +
   再構築契機 S18 + 開放度 S13 の閉形式)、`ClassifyPath` / `ShapeAcousticSpatial` (純関数、spec §4.1.2 の 8 段)、
   `AcousticShapeState` / `AcousticShapeInfo` / `AcousticAudioStats` / `PendingWaveShot` (POD。**M68a では定義だけ**、push/drain は M68b)。
2. `src\Engine\Engine\Audio\AcousticAudioSelfTest.h/.cpp` (新規): 下の T1〜T15。`pwsh -File tools\gen_project_files.ps1`。
3. `Components.h/.cpp`: `AcousticAudioComponent` を**末尾 (TypeId 50)** に `kComponentNoHash` で登録 (spec §4.2 の表、全 22 フィールド)。
   `Components.cpp:840` 付近の予約コメント / `CLAUDE.md:150-151` / `plans\supple-weaving-loom.md` (:36 / :120 / :214 / :246 / :290) の
   「50/51」を「51/52」へ。
4. `AudioSystem.h`: `AudioSpatial::lpfCoefficient = 1.0f` を**末尾**に。`AudioSystem.cpp:1146` の `applyLpf(dry, dsp.LPFDirectCoefficient)`
   を `applyLpf(dry, std::min(dsp.LPFDirectCoefficient, s.lpfCoefficient))` に。リバーブ送り側は触らない。
5. `AudioSourceSystem.h/.cpp`: `SetAcousticField(const AcousticField*)` / `SetAcousticAudioLog(int ticks)` /
   `const AcousticAudioStats& AcousticAudioStats() const`。`SourceState` に `AcousticShapeState shape` を足し、`st.vel = {}` の 3 箇所
   (`:258` / `:389` / `:404`) で一緒に `{}`。`Update`: `dTicks` を `:340` の上書き前に取る → 既存のリスナー確定の後で
   (1) 有効な `AcousticAudio` を探す (enabled かつ active、最小 `entity.index`) (2) probe を更新 (3) per-voice ループ (`:421-448`) で
   `ShapeAcousticSpatial(probe, comp, listener.position, pos, spatial, gain, &st.shape, logInfo)` → `desc.volume *= gain` →
   `spatial.lpfCoefficient = ...` → 既存の `SetVoiceVolume` / `ApplyVoiceSpatial`。log 出力。`Reset()` で probe 無効化。
   `--no-audio` は `:330` の return の**後**に probe を組む = ヘッドレスはゼロコスト。
6. `EngineLoop.h`: `EngineConfig::acousticAudioLogTicks = 0`、`EngineContext::audioSources = nullptr`。`EngineLoop.cpp`:
   `:287` の隣に `audioSources.SetAcousticField(&acoustic);` (**唯一の配線点**)、`:390` の隣に `ctx.audioSources = &audioSources;`、
   `SetAcousticAudioLog(config.acousticAudioLogTicks)`、`:1832` の `[rt]` 行の隣に summary 行 (spec §4.1.6)。
7. `EditorMain.cpp:382` / `RuntimeMain.cpp:370` の隣に `--acoustic-audio-log N`。`CLAUDE.md` の CLI 一覧に 1 項目、`:37` の「44 スイート」→ 45。
8. `ProfilerWindow.cpp:87-101` の隣に `acoustic-audio:` 行 (`ctx.audioSources` 経由、有効な probe があるときだけ)。
9. `EditorMain.cpp:625`: `&& mye::RunAcousticAudioSelfTest(); // M68a` を末尾に (`RunSourceControlSelfTest` の後)。
10. `src\Engine\Engine\DemoContent.cpp` `BuildAcousticShowcaseScene` の**末尾** (ランプ 3 個の `for` の後) に、この順で:
    `Watcher Ears` (Watcher の子、`AudioListenerComponent` のみ、`SetParent(player)`、ローカル (0,0,0)) →
    `Acoustic Audio` (`AcousticAudioComponent`、既定値のまま。`toneSound` は M68b) →
    `Hum` (`AudioSourceComponent`: `sound = AssetID{ ctx.sounds->ResolveKey(HashStr("hum")) }`、`playOnAwake 1`、他は既定。
    MeshRenderer 無し。位置 = `(AcousticMapToWorld(6), 1.0f, AcousticMapToWorld(9))`)。
    `Watcher` の `GameObject player` はブロックスコープなので、子を付けるにはスコープの外へ `EntityID` を持ち出す
    (Watcher ブロック内の追加は**しない** = 生成順を動かさない)。
11. `assets\audio\hum.wav` (SynthCore: Sine 110 Hz、2.0 s ちょうど、amp 0.5、attack/decay/release 0、sustain 1、44.1 kHz mono) +
    `hum.sound.json` (spec §4.2) + `hum.wav.meta` + `hum.sound.json.meta` (guid 一致、spec S11)。
    焼くのは一時プローブ (コミットしない)。**`SynthParams` を実装メモに書く**。
12. `plans/m68-acoustic-audio/` 一式をコミットに含める。

## やらないこと (このサブでは)

- 残響 override / `PendingWaveShot` の push・drain / 足音 4 本 / `Mixer_AcousticOverride` / `AcousticField.h:54` のコメント (M68b)。
- 文書 (engine_spec / README / ADR / test_checklists の本文。M68c)。`CLAUDE.md` は上記 3 箇所だけ。
- 到来方向の平均化 (S16) / 一発再生の追従 (S17) / 調整値の追い込み。

## 触る場所 (planner の見立て)

| ファイル | 場所 | 何を |
|---|---|---|
| `src\Engine\Engine\Audio\AcousticAudio.h/.cpp` | 新規 | probe / 整形 / POD 群。include は `Acoustic/AcousticField.h` + `Acoustic/AcousticGrid.h` + `Audio/AudioSystem.h` + `Audio/SpatialMath.h` + `Core/Components.h` |
| `src\Engine\Engine\Audio\AcousticAudioSelfTest.h/.cpp` | 新規 | `bool RunAcousticAudioSelfTest();` (`AcousticSelfTest.cpp` の `check` 流儀、`DebugSetGrid` + `MakeLMaze` の写し) |
| `src\Engine\Core\Components.h` | `AgentBrainComponent` (`:1276`) の後 | `AcousticAudioComponent` |
| `src\Engine\Core\Components.cpp` | `:840` のコメント、`AgentBrain` 登録の後 (`:952` の `}` の前) | 登録 + 予約コメント |
| `src\Engine\Engine\Audio\AudioSystem.h` | `:57` (`pitch` の後) | `lpfCoefficient` |
| `src\Engine\Engine\Audio\AudioSystem.cpp` | `:1146` | `min` |
| `src\Engine\Engine\Audio\AudioSourceSystem.h/.cpp` | `SourceState` (`:69-78`)、`Update` (`:324-453`)、`Reset` (`:200`) | 上記 5 |
| `src\Engine\Engine\EngineLoop.h/.cpp` | `EngineConfig` (`:165-169` の隣)、`EngineContext` (`:328` `sounds` の隣)、`:287`、`:390`、`:1832` | 配線 |
| `src\Editor\EditorMain.cpp` | `:382`、`:625` | CLI、連鎖 |
| `src\Runtime\RuntimeMain.cpp` | `:370` | CLI |
| `src\Editor\Windows\ProfilerWindow.cpp` | `:87-101` の隣 | 1 行 |
| `src\Engine\Engine\DemoContent.cpp` | `:3116` (関数末尾) | 3 エンティティ |
| `assets\audio\` | 新規 4 ファイル | hum |
| `CLAUDE.md` / `plans\supple-weaving-loom.md` | 上記 | 数字 |

## 受け入れ条件 (このサブ)

spec §5 の A1〜A14。selftest の内訳 (T1〜T15、すべてヘッドレス。土台は `DebugSetGrid` + `MakeLMaze` 24×1×24 (腕 1: x∈[2,20], z=2 /
腕 2: x=20, z∈[2,20]) と自由空間 24×4×24。`ShapeAcousticSpatial` は `AcousticAudioComponent` の既定値で呼ぶ):

| T | 内容 | 期待 |
|---|---|---|
| T1 | 登録 | `AcousticAudioComponent::sTypeId == 50`、レジストリの flags に `kComponentNoHash`、`kSimSnapshotVersion == 11` |
| T2 | 同一原点の一致 | L-maze で `Emit(S)` → `Advance` を maxRing まで回した波と、原点 S の probe: 波の `dist != kUnreached` の全セルで `dist` と `parentDir` が一致、波が未到達のセルは probe の `dist > wave.maxDist` または未到達 |
| T3 | 対称性 | 3 組の (L, S) で `probe(L).dist[S] == wave(S).DistanceAt(L)` |
| T4 | 再現性 | 同じ入力で 2 回組んで `dist` / `parentDir` が memcmp 一致 |
| T5 | 密閉 | 四方を閉じた孤立開セルの S → `Occluded`、`gainOut == occludedGain`、`lpf == occludedLpf`、`position` は S のまま |
| T6 | 上限超え / グリッド外 | `probeMaxRing = 8` で L-maze の遠端の S → `Occluded` (dPath = −1)。グリッド外の S → `Bypass` (`AudioSpatial` が memcmp 不変、gain 1)。リスナーがグリッド外 → probe 無効 → `Bypass` |
| T7 | 到来方向 | L = (5,0,2)、S = (20,0,18): `Detour`、`dir == (+1,0,0)`、`position == L + dir·dPath` (1e-4)、`dopplerScale == 0` |
| T8 | 直達 | 直線廊下 L (3,0,2) / S (15,0,2): `Direct`、`lpf == 1`、`position == S` (厳密)、gain 1、`dopplerScale` 不変。自由空間の対角 L (2,1,2) / S (20,1,20): `Direct` (チャンファの +2.9% を閾値が吸収) |
| T9 | **クラス列** | `probeMaxRing = 20`、S = (20,0,18) 固定、L を (2,0,2)→(20,0,2)→(20,0,16) と 1 セルずつ歩かせる: クラス列が `Occluded*` → `Detour*` → `Direct*` (各区間は連続、逆戻りなし、3 区間とも非空)、非 Occluded の全歩で `dPath ≥ 0.99·dLine`、dPath は単調非増加 |
| T10 | 単調 | L 固定、S を腕 2 の z=3..18 で動かす: detour 非減少 → lpf 非増加、常に ≥ `lpfFloor`。「床に着く」は `bendFullM = 4.0` の複製で検査 (12 m 四方の L 字では回り込みが 5 m 弱で、既定 8 では床 (6 m) に届かない。round 1 で訂正) |
| T11 | 開放度 | 自由空間 24×4×24 の中心で `openness ≥ 0.95`、L-maze の腕 1 中央で `< 0.3`。閉形式 chamfer == 自由空間 Dial (全セル、R 以内) |
| T12 | 平滑化 | 目標を段状に変えて `dTicks=1` ×3 と `dTicks=3` ×1 が 1e-5 以内。`valid=false` → スナップ厳密。`smoothTicks=0` → スナップ |
| T13 | 波→spatial の関係式 | `RolloffGain(0, 0.5, 10, d)² · 0.7 == EnergyAt(d/0.5·11, 220, 0.7, 0.5)` を d ∈ {1, 3, 7} m で 1e-6、d = 10 で両方 0 (M68b の `MakeWaveShotPlay` が前提にする式。ここで先に固定) |
| T14 | ソリッド内の S | 壁セル (開セルに隣接) を S にする → nudge されて `Occluded` にならない |
| T15 | ボリューム無し | `AcousticField` 既定 (HasVolume false) → probe 無効、`ShapeAcousticSpatial` が `Bypass`、`AudioSpatial` memcmp 不変、gain 1 |

## 検証コマンド

```
pwsh -File tools\gen_project_files.ps1
(8 ビルド。MYE_MSBUILD_ARGS=/p:MyeWarnAsError=true)
cmd /c bin\x64\Debug\Editor.exe --selftest                       # 45 スイート
tools\replay_verify.bat                                           # 7 ペア / 10 ジョブ
tools\shot_verify.bat                                             # 22 枚 maxDiff=0 (Release 先行)
pwsh -File tools\check_rules.ps1
tools\build_managed.bat Debug / Release
git diff 8e4272e -- src/Engine/Engine/Acoustic                    # 空
git diff 8e4272e -- src/Shared/EngineAPI.h src/Scripting          # 空
A11: cmd /c bin\x64\Release\Runtime.exe --acoustic-demo --synth-input --acoustic-audio-log 600 --screenshot <scratch>\acaudio.png --shot-frame 600 --frames 601 --font-embedded > <scratch>\acaudio_1.txt   (×2、--no-audio は付けない)
A12: 同じ + --no-audio → [acaudio] 行 0
```

A11 の判定は `[acaudio] t=` 行だけを抜いて (a)〜(e) を数える (PowerShell で十分)。**予測**: `Detour` ≥ 550 行 / `Direct` = `Occluded` = `Bypass` = 0 行 /
Hum の `dPath` は 28〜40 m / `rebuilds ≥ 2`。予測と違ったら分類か probe のバグなので、`--update` 的に予測を書き換えない (SELF_EVAL の「不安・質問」へ)。

## 実装メモ (coder が追記)

### round 1

```
SELF_EVAL: sub-01 (round 1)
実装:
  - src/Engine/Engine/Audio/AcousticAudio.h/.cpp (新規) — AcousticProbe / AcousticShapeState /
    AcousticShapeInfo / AcousticAudioStats / PendingWaveShot (定義のみ) / AcousticPathClass。
    BuildProbeField = Dial の 3 本目 (AdvanceWaveOneRing をリング分周なしで完走した写し)、
    UpdateAcousticProbe (再構築契機 + kProbeCellBudget)、ComputeOpenness (閉形式が分母)、
    ChamferClosedForm、ShapeAcousticSpatial (spec §4.1.2 の 8 段)
  - src/Engine/Engine/Audio/AcousticAudioSelfTest.h/.cpp (新規) — T1〜T15 (47 アサート)
  - src/Engine/Core/Components.h/.cpp — AcousticAudioComponent (TypeId 50 / kComponentNoHash /
    全 22 フィールド)。:840 の予約コメントを 51/52 へ
  - src/Engine/Engine/Audio/AudioSystem.h — AudioSpatial::lpfCoefficient (末尾追加、既定 1.0)
  - src/Engine/Engine/Audio/AudioSystem.cpp — applyLpf(dry, min(LPFDirectCoefficient, s.lpfCoefficient))
  - src/Engine/Engine/Audio/AudioSourceSystem.h/.cpp — SetAcousticField / SetAcousticAudioLog /
    AcousticStats、SourceState::shape (vel と同じ 3 箇所で {})、Update に dTicks 採取 +
    FindAcousticAudio + probe 更新 + per-voice 整形 + log、Reset で probe 無効化
  - src/Engine/Engine/EngineLoop.h/.cpp — EngineConfig::acousticAudioLogTicks、
    EngineContext::audioSources、:287 隣の配線 2 行、:390 隣の ctx 代入、[rt] 隣の summary、
    ★メインループ直前の audioSystem.Update(0.0f) (下記「仕様との差分」[追加] 1)
  - src/Editor/EditorMain.cpp / src/Runtime/RuntimeMain.cpp — --acoustic-audio-log N
  - src/Editor/EditorMain.cpp — 連鎖末尾に RunAcousticAudioSelfTest() (45 本目)
  - src/Editor/Windows/ProfilerWindow.cpp — acoustic-audio: 行 (stats.active のときだけ)
  - src/Engine/Engine/DemoContent.cpp — BuildAcousticShowcaseScene 末尾に Watcher Ears (子) /
    Acoustic Audio / Hum の 3 体。Watcher の EntityID をブロック外へ持ち出しただけで生成順は不変
  - assets/audio/hum.wav + hum.sound.json + .meta x2 (新規)
  - CLAUDE.md (44→45 スイート / CLI 1 項目 / TypeId 末尾 50)、plans/supple-weaving-loom.md (50/51→51/52)

hum の SynthParams (SynthCore で焼いた実値。再現手順):
  wave=Sine / freqStart=freqEnd=110.0 / durationSec=2.0 / amplitude=0.5 / duty=0.5 /
  attackSec=0 / decaySec=0 / sustainLevel=1.0 / releaseSec=0 / sampleRate=44100 / channels=1
  → 88200 frames / 176444 bytes / peak ±16383 / 220 周期ちょうど (先頭 0, 末尾 -257 で継ぎ目なし)
  .wav.meta guid = 3f8c5d1a9e264b70 (= .sound.json の clip 4579137290032794480)
  .sound.json.meta guid = c47b0e93a5d61f28

仕様との差分:
  - [追加] EngineLoop のメインループ直前に audioSystem.Update(0.0f) を 1 回。
    理由: 起動時 ApplyMixer (RegisterAssetLibraries) が保留したバスグラフ再構築がフレーム 0 の
    audioSystem.Update() で走り、その直前に audioSources.Update() が playOnAwake で鳴らした音を
    DestroyAllSourceVoices が全部殺していた (実測: hum が 2 tick で無音、shaped=2)。
    AudioSourceSystem は「voice が消えた = 鳴り終わった」と解釈して started を立てたままにするので
    ループ音が復活しない。**M45 からの既存不具合**で、--acoustic-demo に AudioSource が
    1 個も無かったため今まで露見していなかった。最小・可逆な修正としてグラフ構築を
    フレーム 0 より前へ倒した (dt=0 なのでフェードもメーターも進まない)。この 1 行が無いと A11 が成立しない。
  - [追加] ShapeAcousticSpatial の引数に const AcousticField& field と float dTicks を追加。
    field は §4.1.2 手順 2 の「壁の中の音源を 26 近傍へ寄せる」に IsSolid が要るため
    (probe は占有を持たない)。dTicks は §4.1.2 手順 7 の半減期が tick 基準だから。
    probe に AcousticField* を持たせる案は寿命の罠になるので採らなかった。
  - [追加] probe の再構築契機に「probeMaxRing が変わった」を足した (S18 の 4 条件 + 1)。
    Inspector で実行中に触れる値なので、これが無いと変更が次のセル移動まで効かない。
  - [逸脱] アクセサ名を AudioSourceSystem::AcousticAudioStats() ではなく AcousticStats() に。
    型名と同名のメンバ関数はクラス内で型名を隠す (以後 mye:: 修飾が必須になる) ため。
  - [追加] 終了時 summary を「acousticAudioLogTicks > 0 かつ stats.ticks > 0」で出す。
    A12 が「--no-audio で [acaudio] 行が 0」を要求するので、summary もゼロコスト側に倒した。
  - [追加] Bypass のとき AcousticShapeState を {} に落とす (次に有効化されたときスナップする)。
    残すとグリッドへ入り直した瞬間に前回の遮蔽値から数百 ms かけて戻ってくる。
  - [追加] T10 の「末尾で床に着く」だけ bendFullM=4.0 の複製コンポーネントで検査。
    24x24 セル (12m 四方) の L 字で稼げる回り込みは最大 5m 弱で、既定 bendFullM=8 では
    床 (detour >= 6m) に届かない = 既定値のままでは書けない主張だったため。
    単調性・下限の検査は既定値のまま。

検証:
  - pwsh -File tools\gen_project_files.ps1 → Engine 372 / Editor 122 ファイルで再生成
  - MSBuild Debug/Release x64 (/p:MyeWarnAsError=true) → 0 error / 0 warning
  - tools\build_managed.bat Debug / Release → 両方 0 warning / 0 error
  - cmd /c bin\x64\Debug\Editor.exe --selftest → exit 0、FAIL 0 件。45 本目
    "Acoustic audio self test: ALL PASS" (T1〜T15 の 47 アサート全 PASS)
  - tools\replay_verify.bat → [PASS] 10 ジョブ / 7 シーン / 103.8s、規則検査 0 error
  - tools\shot_verify.bat → exit 0、22 枚すべて img-diff PASS maxDiff=0
    ※最初の 1 回だけ「1 shot(s) differ」で落ちた (replay_verify 直後の実行)。どの 1 枚かは
      未捕捉。その後 3 回連続で 22/22 maxDiff=0 なので**取りこぼしのフレーク**と判断した。
      再現しなかったので原因は特定できていない (不安・質問 3)
  - pwsh -File tools\check_rules.ps1 → 0 error / 0 warning
  - git diff 8e4272e -- src/Engine/Engine/Acoustic → 空 (A6。M68a はコメント行も触っていない)
  - git diff 8e4272e -- src/Shared/EngineAPI.h src/Scripting → 空 (A7)
  - A11 (Release Runtime、同一コマンド ×2):
      summary: ticks=601 rebuilds=66 boxCells=16224 probeMsAvg=0.647 shaped=601
               classes D/T/O/B=0/601/0/0
      (a) class=Detour 599 行 (>= 550) / Direct=Occluded=Bypass=0 行 → **予測どおり**
      (b) dPath >= 0.99*dLine が全 599 行で成立。**ただし dPath の帯は 20.77 / 26.64〜30.73 m で、
          予測の「28〜40 m」から外れる** → 不安・質問 1
      (c) name=Hum の tick 間 max|Δgain|=0.000 / max|Δlpf|=0.060 (<= 0.12) → 予測どおり
      (d) 2 run の [acaudio] t= 行が**バイト一致** (599 行) → 予測どおり。
          さらに「修正前の run」とも一致 = 出力が実時間に依存していない
      (e) rebuilds=66 (>= 2) → 予測どおり
  - A12: 同じコマンド + --no-audio → [acaudio] 行 0 (summary 含めて 0)
  - A13: Profiler 行はコード読み + Editor --autoplay の実走 (shaped=60 / stats.active=true =
    行が出る条件が満たされる) で確認。**GUI の目視はしていない**
  - A14: 起動ログに [audio] loaded clip: hum (1 ch @ 44100 Hz, 2.00s)。
    unknown sound key は 0 件 (M68a はまだそのメッセージ自体を出さない)
  - 独立検算: Python で同じ 26 近傍チャンファ Dijkstra を組み、デモの壁配置から
    セル (7,2,8)→(28,1,40) を解いて **chamfer 662 / dPath 30.09 / dLine 19.14** を得た。
    エンジンのログ (t=2) と**小数第 2 位まで完全一致**。静止時 (Editor --autoplay、セル (8,2,8))
    も 651 / 29.59 で一致した = probe と分類は正しい

自己採点 (1-5):
  仕様適合: 4 — 受け入れ A1〜A14 のうち A11 (b) の「dPath 28〜40 m」だけ外れた。外れた理由は
    分類でも probe でもなく (独立検算が小数 2 位まで一致)、spec の 35.8 m という見積りが
    26 近傍の斜めショートカットを勘定していないため。それ以外は全部予測どおり。
    ShapeAcousticSpatial の引数 2 つ追加と EngineLoop の 1 行が spec 外 (上に明記)
  正しさ: 4 — 45 スイート / replay 7 ペア / golden 22 枚 / 規則検査がすべて緑で、実機ログの
    2 run バイト一致と外部 Dijkstra との数値一致まで取れている。5 にしないのは
    (i) shot_verify が 1 回だけフレークしたこと (ii) Profiler 行を GUI で目視していないこと
  コード品質: 4 — 3 本目の Dial は AdvanceWaveOneRing の写しで、写した理由と「同じ訪問順に
    なる根拠」をコメントに書いてある。写し自体は重複なので、共通化しなかったのは
    あちらが sim 状態 (WriteShell) を巻き込んでいるため。既存の命名・コメント密度に合わせた
  テスト: 4 — T1〜T15 を全部実装し 47 アサートが通る。「波の場と probe が memcmp 一致」
    「閉形式 == 自由空間 Dial」「3 × 1tick == 1 × 3tick」など不変条件を機械化した。
    5 にしないのは、実オーディオ (XAudio2 が実際に出す音) は誰も検査していない = 耳が残っていること

不安・質問:
  1. **A11 (b) の帯が実測と食い違う。** spec §4.2 / §5 A11 は「部屋 A の隅からの経路 ≈ 35.8 m」
     「dPath は 28〜40 m」としているが、実測は静止時 29.59 m / 移動中 26.64〜30.73 m。
     Python の独立実装 (同じ 26 近傍 <11,16,19> の Dijkstra) が engine と小数 2 位まで一致した
     ので、**35.8 m のほうが誤り** (斜め移動でコーナーを切る分を数えていない Manhattan 寄りの
     見積り) と判断した。指示どおり予測を書き換えていないので、spec 側の数値を
     「26.5〜31 m」等へ直すか A11 (b) の帯を広げるかを planner に裁定してほしい。
  2. **t=1 の 1 行だけ場が焼けていない** (dPath=20.77 / open=1.00 = 自由空間の値)。
     AcousticField::Sync の占有ベイクは静的コライダの WorldMatrix を見るので、
     行列が確定する前の最初の 1 tick は「壁が無い」状態で焼かれる (M65a からの既存挙動、
     tick 2 で正しい値に落ち着く)。整形側は平滑化で 1 tick ぶん引きずるだけ。
     v1 で許容してよいか (許容なら A11 の帯の下限もこの行を除外して書くべき)。
  3. shot_verify が **1 回だけ**「1 shot(s) differ」で落ちた (replay_verify 直後の実行)。
     直後から 3 回連続で 22/22 maxDiff=0。どの 1 枚かはログ末尾しか残っておらず未特定。
     M68a はピクセルに触っていない (新規 3 体に MeshRenderer 無し) ので実装起因とは考えにくいが、
     フレークとして台帳に残すか reviewer に再確認させるかを判断してほしい。
  4. Debug の probe 再構築が **9.3 ms/回** (Editor --autoplay の実測。Release は 0.65 ms)。
     デモは 16k セルで箱 = グリッド全体。歩行中 3〜7 回/秒なので Debug で 30〜65 ms/s。
     spec §4.4 の「Debug 数 ms」より重い。v1 で許容か、probeMaxRing の既定を下げるか。

触ったファイル:
  src/Engine/Engine/Audio/AcousticAudio.h
  src/Engine/Engine/Audio/AcousticAudio.cpp
  src/Engine/Engine/Audio/AcousticAudioSelfTest.h
  src/Engine/Engine/Audio/AcousticAudioSelfTest.cpp
  src/Engine/Engine/Audio/AudioSourceSystem.h
  src/Engine/Engine/Audio/AudioSourceSystem.cpp
  src/Engine/Engine/Audio/AudioSystem.h
  src/Engine/Engine/Audio/AudioSystem.cpp
  src/Engine/Core/Components.h
  src/Engine/Core/Components.cpp
  src/Engine/Engine/EngineLoop.h
  src/Engine/Engine/EngineLoop.cpp
  src/Engine/Engine/DemoContent.cpp
  src/Editor/EditorMain.cpp
  src/Editor/Windows/ProfilerWindow.cpp
  src/Runtime/RuntimeMain.cpp
  build/Engine.vcxproj
  build/Engine.vcxproj.filters
  assets/audio/hum.wav
  assets/audio/hum.wav.meta
  assets/audio/hum.sound.json
  assets/audio/hum.sound.json.meta
  CLAUDE.md
  plans/supple-weaving-loom.md
  plans/m68-acoustic-audio/sub-01.md

申し送り:
  - **M68b が乗る足場**: PendingWaveShot は定義済み・push/drain は未実装。
    ShapeAcousticSpatial は smooth=nullptr で一発再生から呼べる形にしてある。
    log 行の kind= は "voice" 固定なので shot 側を足すこと。summary / Profiler 行の
    末尾に shots / room を足す場所も素直に伸ばせる。
  - **AcousticAudioComponent は 22 フィールド全部確定済み**。M68b は toneSound0..3 /
    reverbSmall / reverbLarge / roomSmoothTicks / openSmall / openLarge / detourWet /
    waveVolume / minWaveVolume / waveReverbSend / waveRolloff を**読むだけ**で足りる。
  - AudioSpatial::reverbSend は M68a では触っていない (Detour/Occluded の +detourWet は M68b)。
  - 開放度 (probe.openness) は焼き直しのたびに計算済み。M68b の残響はこれを読むだけ。
    デモの実測は部屋 A 内で 0.33〜0.62 (roomProbeM=6)。openSmall=0.2 / openLarge=0.6 の
    既定だと t がほぼ上端に張り付くので、耳で詰めるときはここが最初の調整点。
  - reviewer 向け: A11 のレシピは spec のまま動く。ログの解析は
    `[acaudio] t=` 行を抜いて数えるだけでよい (2 run バイト一致も同じ抜き方で確認できる)。
  - 作業ツリーの改行: Edit が ProfilerWindow.cpp / AudioSourceSystem.h/.cpp / AudioSystem.h を
    LF で書き戻した (blob は元から LF なので git diff は差分行だけ。全域ノイズは出ていない)。
```

## フィードバック履歴

- round 1: **VERDICT OK** (planner、2026-09-06)。A1〜A14 を SELF_EVAL の検証欄と実コード (EngineLoop / AudioSourceSystem /
  AcousticAudio.cpp の 8 段 / Components / DemoContent 末尾 / hum.sound.json と .meta の guid 一致) で突き合わせ、全部成立。
  差分 7 件は全て仕様側の穴または見積り誤りとして spec §8 #1〜#9 に採用 (coder の誤りなし)。
  should 1 件: shot_verify の 1 回のフレーク (どの 1 枚か未捕捉) — 出力をファイルに残して 2 回追加実行、割れたら枚名と maxDiff を
  実装メモへ。nit 2 件は申し送りへ (AudioSourceSystem.h の冗長な前方宣言 / Debug の probe 9.3 ms)。
