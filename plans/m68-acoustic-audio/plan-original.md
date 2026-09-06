# M68: 音響伝播 × 実オーディオ — 波面の 4 役目 (遮蔽・回折ローパス・部屋の残響・鳴る波)

## Context

### なぜやるか

コンテスト向けの追加機能として「他エンジンに無く、30 秒の動画で耳と目に同時に伝わるもの」を選んだ
(2026-09-06 の案出しで案 1)。M65 の音響伝播は **1 本の波面が描画・AI・残光の 3 役**を担うが、
`plans\hushed-rippling-beacon.md` の「実装しない / できないもの (v1 の境界)」が
**「実際に鳴る音との統合」を明示的に外している**:

> `AudioSourceSystem` は決定論レーンの外。`Emit` と同時に `PlaySound` を鳴らすのは
> **出力レーン (tick 末ハッシュの後)** の別作業。`SpatialMath.h::RolloffGain` を共有すれば減衰は一致する

実測 (調査 3 本、2026-09-06): `--acoustic-demo` には `AudioSource` も `AudioListener` も **0 個**、
`Emit` の 3 呼び出し点 (`AcousticField.cpp:668` / `:755` / `AgentSystem.cpp:298`) はどれも音を鳴らさず、
`src\Engine\Engine\Audio\` に occlusion / obstruction の語は **1 件も無い**。ローパスは X3DAudio の
既定距離カーブだけ (`AudioSystem.cpp:1083`)。つまり今のデモは「波は見える・敵は聞く」が
**プレイヤーの耳には何も届かず**、鳴らしたとしても壁の向こうの音が素通しで聞こえる。

### 決定論の分水嶺 (M65 で確定済み、本計画の前提)

- `AudioListener` / `AudioSource` は `kComponentNoHash`、`AudioSourceSystem` は専用 `Pcg32` (`rng_`) を持ち、
  更新は `EngineLoop.cpp:1544` = tick ループの**後** (出力レーン)。スクリプトの一発再生も
  `TickRunner.cpp:570-576` の `if (!ts.resim)` ブロック = **ハッシュ (`:494-562`) の後**。
- 音響の sim 状態は**波スロット表だけ** (`AcousticField::Wave` ×16)。占有・距離場・流れ場・残光は導出値。
- → 本計画は **sim 状態を 1 バイトも増やさない**。音響場を「読む」だけの出力レーンの仕事で、
  **既存 7 ペアの replay と 22 枚の golden は全サブで無風**であるべき (それが健全性の指標)。
- 出力レーンなので**キャッシュしてよい** (流れ場と違い、リスナー場は「場が間に合ったか」が
  ハッシュ対象に影響しない。`AcousticNav.cpp:51` の `StaticSignature()` 判定が前例)。

### ユーザー決定 (2026-09-06)

1. **音源は WAV を焼いてコミット** (SynthCore で生成 → `assets\audio\`。手で追い込める形)。
2. **残響は 2 プリセット間の連続補間** (段階切替は不採用)。
3. **3 サブ** (a: 場 + 遮蔽 + LPF / b: 残響 + 鳴る波 + デモ / c: 仕上げ)。
4. **`/harness` (3 役) で回す** (M66 / M67 と同じ)。

### 出口の姿

- 隣の部屋で鳴っている持続音が、**壁越しにはこもって小さく**、廊下の角を曲がると**戸口の方向から**聞こえる。
- 波が見える場所と聞こえる音の減衰が**同じ距離場**から出る (絵・AI・耳の一致が構造的に成立)。
- 狭い廊下と広い部屋で残響が変わる (連続に、段差なく)。
- 足音・衝撃・投擲・敵の自発音の波が**実際に鳴る** (今は無音)。鳴る範囲 = 波の到達範囲。
- **既存 22 枚の golden は maxDiff=0、7 ペアの replay は無風、ABI v15 据え置き。**

---

## 全体設計 (7 つの判断と根拠)

### 判断 1 — リスナー場は出力レーンの「3 本目の Dial コピー」。sim ループには触らない

リスナーのセルを原点に、**同じ 26 近傍表 `acoustic::kNeighbors` と同じ重み `<11,16,19>`** で
バケット Dijkstra を**一気に完走**させ、`dist` / `parentDir` を得る (`Audio\AcousticAudio.h/.cpp` 新設)。

- `AdvanceWaveOneRing` (`AcousticField.cpp:423-487`) は非 const で slot 参照、残光 `WriteShell` が
  ループ内に食い込んでいる (設計上そこ以外にない)。共通化のためのパラメタ化は、`Rebuild()` の
  memcmp 不変条件 (`:503-513`) と replay 7 ペア目が守っている**唯一の関数**を触ることになる。
  sim 側の利益ゼロで危険だけがある。
- 前例: `AcousticNav::BuildDistance` (`AcousticNav.cpp:115-180`) が既に同じループの 2 本目の写しで、
  共有コードではなく**テストで結んで**いる。リスナー場は 3 本目。読むのは公開 API
  (`Grid()` / `IsSolid()` / `Occupancy()` / `StaticSignature()`) だけ。
- 写しのずれは**共有より強い保証**で塞ぐ: 無向グラフのチャンファ距離は対称なので
  「`probe(L).dist[c] == wave(S) の DistanceAt(c)` が箱の交差の**全セル**で等しい」を selftest で固定する。
- 格納は `AcousticField::WaveField` (公開 struct) をそのまま流用 (`LocalIndex` と箱の証明を引き継ぐ)。
  箱は `±probeMaxRing` をグリッドで clip、さらに**セル予算** (`kProbeCellBudget` ≈ 256k) を超えたら
  `probeMaxRing` を下げて 1 回だけ警告 (`kMaxCells` 4M で完走すると 0.5 秒級 — 箱と予算の両方が要る)。
- 再構築の契機は「リスナーのセルが変わった」or「`StaticSignature()` が変わった」だけ。
  デモ (52×6×52 = 16k セル) で 1〜3 ms (Release、推定) × 歩行時 3〜4 回/秒 = 無視できる。
- **リスナー場を持つのは `AudioSourceSystem`** (`AudioSourceSystem.h:35` の不変条件
  「ECS を読むのはここだけ / AudioSystem は World を知らない」を守る)。`AudioSystem` は
  `AcousticField` を一切 include しない。include の向きは Engine/Audio → Engine/Acoustic
  (`AcousticGrid.cpp:11` が `SpatialMath.h` を読む既存の逆向きはファイル単位で非循環)。
  **`AcousticField` から `AcousticAudio` を include しない** (逆流禁止)。

### 判断 2 — 仮想発音位置: X3DAudio に「経路距離の減衰」と「戸口からの定位」を両方やらせる

音源 S のセルの `dist` から `dPath = ChamferToMeters(dist)`、`parentDir` を S から L へ辿った
**最後の一歩**が到来方向 (`parentDir[ni] = OppositeNeighbor(i)` = 「親へ向かう向き」、
`AcousticField.cpp:474-475`。ループは `TraceToOrigin` `:762-790` と同じ上限 `maxRing*2+8`)。

```cpp
// 到来方向 = L から「経路が入ってきたセル」への向き = kNeighbors[OppositeNeighbor(lastDir)]
// lastDir == kNoParent なら S == L = 直達 (実位置のまま)
// 辿り着いた先の dist が 0 でなければ鎖が壊れている → Occluded 扱い
```

`AudioSpatial::position` を **`L + dir · dPath`** に置き換えるだけで、X3DAudio が
減衰を経路距離で、パンを到来方向で計算する。`AudioSystem` の変更なし。
到来方向は 26 方向に量子化されるので、最後の 2〜3 歩の平均で和らげる (任意)。

- **ドップラーは仮想化中は 0** (`io.dopplerScale = 0`)。X3DAudio は相対速度を発音→リスナーの
  向きへ射影するので、向きが仮想だと音源・リスナー双方の速度で狂う。`velocity` だけ 0 にしても
  `listener_.velocity` が残るので不十分。`UpdateVelocitySample` の呼び出しは触らない (再有効化時に跳ねない)。
- 回折ローパス: `detour = dPath − dLine` (直線距離との差) から
  `lpf = clamp(1 − detour / bendFullM, lpfFloor, 1)`。
- 分類は **4 値**: `Direct` (detour ≈ 0) / `Detour` / `Occluded` (箱内で未到達 = 密閉、
  **または箱の外** — 経路 32m 超は逆二乗で 0.001 なので同じ扱いが音響的にも正しく、箱の縁で
  段差を作らない) / `Bypass` (**`!HasVolume()` か `WorldToCell` 失敗のときだけ**。何も変えない)。
  `Occluded` は gain ×= `occludedGain` (≈0.15)、lpf = `occludedLpf` (≈0.1)、位置は実位置。
- 音源がソリッドセル内 (コライダ付きスピーカー) は `Emit` と同じ表順の 26 近傍 nudge
  (`AcousticField.cpp:527-544`)。やらないと壁掛け音源が全部「密閉」に読める。
- **`spatialBlend > 0` の音源だけ**整形し、gain も `Lerp(1, gain, blend)` で混ぜる
  (`applyLpf` の `Lerp(1, c, blend)` `AudioSystem.cpp:1134` と同じ意味論。2D 音を遮蔽しない)。
- スムージング: セル境界で値が跳ぶ (ジッパー音) ので、gain / lpf / 仮想位置を
  **tick 数ベースの指数平滑** (`alpha = 1 − pow(k, dTicks)`、`dTicks = tickIndex − lastTick_` を
  `:340` の上書き前に取る。`SpatialMath.h:156` と同型)。状態は `SourceState` に `AcousticShapeState`
  を足し、`st.vel = {}` をリセットしている 2 箇所 (`:258` / `:389`) で一緒にリセット。
  `valid=false → 最初の目標へスナップ` (密閉音源が大きく鳴ってから消えるのを防ぐ)。
  **一発再生は `Play` 時に 1 回整形してスナップ、状態なし** (`Play` は開始時にしか spatial を当てない
  `AudioSystem.cpp:1209` = 既存の一貫性)。

### 判断 3 — `AudioSystem` への変更は 2 点だけ。規則は純関数 1 本

1. `AudioSpatial` の**末尾**に `float lpfCoefficient = 1.0f` (開)。`applyLpf` (`AudioSystem.cpp:1133-1147`)
   の直達側だけ `min(dsp.LPFDirectCoefficient, s.lpfCoefficient)`。リバーブ送り側の LPF 行は触らない。
   `AudioSpatial` の生成箇所は全部 default 構築 (`AudioSourceSystem.cpp:174/250/423`、
   `TickRunner.cpp:71`、`AudioSelfTest.cpp:560`、`SceneViewWindow.cpp:677`) なので末尾追加は安全。
2. リバーブの override (判断 5)。

整形は **純関数 1 本** `ShapeAcousticSpatial(const AcousticProbe&, const AcousticAudioComponent&,
listenerPos, AudioSpatial& io, float& gainOut, AcousticShapeState* smooth)` に集約し、
`AudioSourceSystem::Update` の `:421-448` と一発再生の drain の**両方から同じ関数を呼ぶ**
(`MakeSourcePlay` の「規則は必ずこの 1 本だけ」`AudioSourceSystem.h:16-29` と同型)。

### 判断 4 — 開放度 → 残響は 2 アンカー間の I3DL2 連続補間 (ユーザー決定 2)

- 開放度 = リスナー場で「チャンファ半径 R (`roomProbeM` ≈ 6m) 以内の到達セル数 / 同半径の球の総セル数」。
  廊下 (< 0.25) と部屋 (> 0.6) が分かれる。**リスナー場の副産物**なので追加の走査は無い。
- `t = smoothstep(openSmall, openLarge, openness)` を ≈300 ms で平滑化し、
  `XAUDIO2FX_REVERB_I3DL2_PARAMETERS` を `reverbSmall` (既定 SmallRoom = 3) と `reverbLarge`
  (既定 Hall = 6、`AudioMixer.cpp:23-26` の並び) の間で**フィールドごとに lerp**
  (mB 単位の int32 は対数域なので線形補間でよい)。`|Δt| > 0.01` のときだけ `SetEffectParameters`。
- override は `AudioSystem` に `SetReverbOverride(const I3DL2&)` / `ClearReverbOverride()` /
  `ReverbOverrideActive()` を足し、**`ApplyReverbParams()` (`AudioSystem.cpp:589-600`) の 1 箇所**で
  「override があればそれ、無ければ `kReverbPresets[reverbPreset_]`」を選ぶ。
  `ApplyMixer` → `RebuildBusGraphNow` → `BuildBusGraph` 末尾 (`:374`) が `ApplyReverbParams` を呼ぶので
  `.mixer.json` のホットリロード (`ReloadHub.cpp:362`) 後も**自動で再適用**される。
  `reverbPreset_` / `CurrentMixer()` / ミキサー窓の combo (`AudioMixerWindow.cpp:380`) は資産値を保つ。
- Clear は `AudioSourceSystem::Reset()` (シーン遷移) と「有効な `AcousticAudio` が見つからない tick」で呼ぶ。
- 音源側: `Detour` / `Occluded` の音は `detourWet` ぶん送りを足す (壁越しの音は残響成分が主)。
- ミキサー窓: override 中は preset combo の横に `Tr(StrId::Mixer_AcousticOverride)`
  (「音響が上書き中」) を出す。combo は見た目上効かないことを文で示す。

### 判断 5 — 鳴る波は POD キューで渡す。spatial は波から作る (= `EnergyAt` と同じ式)

- `TickRunner.cpp:570-575` の `if (!ts.resim)` ブロックで `ts.acoustic->Waves()` を舐め、
  `bornTick == ctx.tickIndex` の波を **`PendingWaveShot{ox,oy,oz,tone,amplitude,maxRing,bornTick}`** として
  `ts.audioSources->PushWaveShot(...)` へ積む。gate は
  `audioSystem.IsReady() && !audioSystem.IsSuspended() && ts.audioSources != nullptr`。
  (`bornTick == tickIndex` が正しいのは `++ctx.tickIndex` が最後 `:675` だから。ハッシュは既に
  `bornTick` を畳んでいる `WorldHasher.cpp:318` = 読んでよい。`AcousticField.h:54` の「診断用」を書き換える)。
- **なぜ RunOneTick 側で push か**: `kMaxTicksPerFrame = 5` (`EngineLoop.cpp:83`) で、gain の小さい衝撃波
  (`maxRing=1, ticksPerRing=2`) は **4 tick で消えてスロットが再利用される** = フレーム単位の走査では取りこぼす。
- **なぜ AudioSourceSystem 側で drain か**: `Update` (`EngineLoop.cpp:1544`) はリスナー場を更新した
  **後**に同じフレームで鳴らせる (`TickRunner` の時点では前フレームの場しか無い)。
  キューは `Update` の**先頭で無条件 clear** (`:330` の `IsSuspended` 早期 return より前。
  でないと記録/検証中に溜まって解除後に一斉に鳴る) と `Reset()` でも clear。
- spatial は**波から**: `minDistance = cellSize` / `maxDistance = ChamferToMeters(maxRing·11) = maxRing·cellSize` /
  `rolloff = 2` (逆二乗) / `reverbSend = component 値` / 位置 = 原点セル中心 → 判断 2 で整形。
  `EnergyAt = amplitude · RolloffGain(2, cellSize, maxRing·cellSize, d)` (`AcousticGrid.cpp:91-101`) なので
  **「波が届く所でだけ聞こえる」が構造的に成立**する (`BuildRolloffCurve` の 16 点補間の範囲で)。
  音量 = `clamp(amplitude · waveVolume, 0, 1)`、`minWaveVolume` 未満は鳴らさない (呼吸を無音に)。
- 音の鍵は `toneSound[4]` (String64 ×4、`Wave.tone` 0..3 で引く)。`ResolveSoundKey` で
  `.sound.json` でも生クリップでも通す。アセットが解決したら `MakePlayDesc` で
  variation / volumeRandom / pitchRandom を貰い (乱数は **`AudioSourceSystem::rng_`**、
  `audioScriptRng` は触らない = スクリプトの一発再生の乱数列を動かさない)、3D ブロックは波で上書き。

### 判断 6 — 調整値は `AcousticAudioComponent` (TypeId **50**、`kComponentNoHash`)

- `Components.cpp:843` の明文「登録順 = TypeId なので飛ばし登録はできない」。M60′ の 50/51 予約は
  計画上のメモで登録ではない → **50 に登録し、Cloth/SoftBody の予約を 51/52 へ繰り下げ**
  (`Components.cpp:840` のコメント / `CLAUDE.md` の「50/51」/ `plans\supple-weaving-loom.md` の予約表を同時に)。
- `kComponentNoHash` なので snapshot 版も .rep 版も上げない。最小 `entity.index` の active な 1 個が勝つ
  (`AcousticVolume` と同じ)。無ければ機能ごと off (override も Clear)。
- フィールド (すべて M68a で確定、後から動かさない):
  `enabled`(Bool) / `probeMaxRing`(Int32, 64) / `bendFullM`(Float, 8.0) / `lpfFloor`(0.25) /
  `occludedGain`(0.15) / `occludedLpf`(0.1) / `smoothTicks`(6) / `roomProbeM`(6.0) /
  `openSmall`(0.2) / `openLarge`(0.6) / `reverbSmall`(Int32, 3) / `reverbLarge`(6) /
  `detourWet`(0.25) / `waveVolume`(1.0) / `minWaveVolume`(0.02) / `waveReverbSend`(0.35) /
  `toneSound0..3`(String64)。全部 `MYE_JP` 付き、範囲付きは `MYE_FIELD_RANGE`。
- `AcousticShapeState` / `AcousticProbe` / `PendingWaveShot` は ECS に置かない
  (`SourceState` と同じ「シリアライズもコピペもされない側テーブル」)。

### 判断 7 — 音源は SynthCore で焼いた WAV + `.sound.json` (ユーザー決定 1)

- 5 本: `step_soft` (tone 0) / `step_wood` (1) / `step_hard` (2) / `step_metal` (3) / `hum` (部屋 B の持続音)。
  初回は一時プローブ (`SynthRender` → `WriteWavToFile`、コミットしない) で焼き、
  **最終 `SynthParams` を M68b の申し送りに書く** (再現可能にする)。目安:
  soft = Noise 0.12s amp 0.35 短い減衰 / wood = Triangle 180→90Hz 0.16s / hard = Square 900→300Hz duty 0.3 0.10s /
  metal = Sine 1800→1500Hz 0.45s 長い減衰 / hum = Sine 110Hz **2.0s ちょうど** (220 周期 = 継ぎ目なし。
  attack/decay/release 0、sustain 1)。合計 ~250KB。
- `.sound.json`: bus `SE`、`spatialBlend 1`、`minDistance 0.5`、`maxDistance 30`、`rolloff "inverse"`、
  `dopplerScale 0`、`reverbSend 0.35`、足音は `pitchRandom 0.08` / `volumeRandom 0.1`。hum は `loop true`。
  `.meta` はエディタの初回スキャンが作るので**一緒にコミット**する (既存 `assets\audio\*.meta` と同じ)。
- `default.mixer.json` は触らない (per-voice の送りは source voice の send 1 で直接 `reverbVoice_` へ行く
  `AudioSystem.cpp:1119-1128`。バスの送り 0 は無関係。`reverbPreset "Default"` は override が丸ごと置き換える)。

---

## サブ計画

### M68a — リスナー場 + 遮蔽・回折 (仮想発音位置 + LPF) + 部品 + selftest 45 本目

**新規**: `src\Engine\Engine\Audio\AcousticAudio.h/.cpp` (`AcousticProbe` = Dial の写し + 箱 + 予算 /
`ClassifyPath` / `ShapeAcousticSpatial` / `Openness` / `AcousticShapeState` / `PendingWaveShot`)、
`src\Engine\Engine\Audio\AcousticAudioSelfTest.h/.cpp`。`pwsh -File tools\gen_project_files.ps1`。

**変更**:
- `Components.h/.cpp`: `AcousticAudioComponent` を**末尾 (TypeId 50)** に `kComponentNoHash` で登録、
  50/51 予約コメントを 51/52 へ (`CLAUDE.md` / `supple-weaving-loom.md` も)。
- `AudioSystem.h/.cpp`: `AudioSpatial::lpfCoefficient` 末尾追加 + `applyLpf` の `min`。
- `AudioSourceSystem.h/.cpp`: `SetAcousticField(const AcousticField*)`、`AcousticProbe probe_`、
  `Update` で (1) 有効な `AcousticAudio` を探す (2) リスナーのセル / 署名でリスナー場を再構築
  (3) 各 voice を `ShapeAcousticSpatial` で整形 (`desc.volume *= gain`、`spatial.position` 差し替え、
  `dopplerScale = 0`、`lpfCoefficient`) (4) `SourceState::shape` のリセット 2 箇所。
  `--no-audio` は `:330` の `IsReady/IsSuspended` return の**後**にリスナー場を組む = ヘッドレスはゼロコスト。
- `EngineLoop.cpp`: 配線 1 行 (`audioSources.SetAcousticField(&acoustic)`、`:287` の隣。**唯一の配線点**)。
- `ProfilerWindow.cpp`: `acoustic-audio: probe %6.3f ms (rebuilds %d, box %d cells, shaped %d, shots %d, room t=%.2f)`
  行 (有効な `AcousticAudio` が無ければ行ごと出さない。既存 `:87-101` の隣)。
- CLI `--acoustic-audio-log N` (`--acoustic-dump` と同じ 7 ファイル配線): N tick のあいだ
  `[acaudio] t=<tick> src=<idx> class=<Direct|Detour|Occluded|Bypass> dPath=%.2f dLine=%.2f lpf=%.2f gain=%.2f room=%.2f`
  を標準出力へ。**reviewer が耳なしで数値で検証する口**。
- `EditorMain.cpp`: 連鎖末尾に `&& mye::RunAcousticAudioSelfTest()`。
- デモ (`DemoContent.cpp` `BuildAcousticShowcaseScene`): `Watcher` に `AudioListenerComponent`
  (y 1.35 = ボリューム内。俯瞰カメラは golden 用にそのまま)。★NoHash でもアーキタイプが変わるので
  **golden 18/19 (frame 120) がバイト同一か先に確認**。動いたら Watcher の子エンティティ (末尾 append) に置く。
  `AcousticAudio` エンティティと部屋 B の `Hum` (`AudioSource` ループ、**MeshRenderer 無し**) を
  **ランプ 3 個の後ろに append** (`DemoContent.cpp:3062` の順序規則 / [[demo-entity-order-is-rng-stream]])。
  hum の WAV + `.sound.json` + `.meta` はこのサブでコミット (足音 4 本は M68b)。

**selftest (ヘッドレス、`DebugSetGrid` + `MakeLMaze` 24×1×24)**: (1) 対称性 全セル: probe(L) vs wave(S) 完走後の
`DistanceAt` が箱の交差で一致 (2) 役割を入れ替えても一致 (3) 2 回組んで memcmp 一致 (4) 密閉: 孤立セルの S は
`Occluded`、gain = occludedGain、位置不変 (5) 箱外は `Occluded`、グリッド外は `Bypass` (6) 到来方向 = 腕の向き、
仮想位置 = L + dir·dPath (7) 直線廊下: detour≈0 → lpf==1、仮想位置≈実位置 (8) detour 単調 → lpf 非増加、floor 尊重
(9) 開放度: 廊下 < 部屋 (10) ボリューム無し → `AudioSpatial` が memcmp で不変、gain==1 (11) 平滑化の
フレームレート非依存: dTicks=1×3 と dTicks=3×1 が 1e-5 以内、`valid=false` はスナップ (12) 波→spatial:
`Wave{maxRing 20, amp 0.7}` → min==cellSize / max==10m / rolloff==2、`RolloffGain·amp == EnergyAt` を 3 距離で
(13) ソリッド内の S は nudge されて密閉にならない。

**受け入れ**: 上記 selftest ALL PASS (45 本)。`--acoustic-demo` でエディタを起動し部屋 A から B へ歩くと
hum が壁越しにこもり、廊下の角で戸口側に定位する (`--acoustic-audio-log 600` の数列で `class` が
Occluded → Detour → Direct と遷移し dPath ≥ dLine が常に成立)。replay 7 ペア無風、golden 22 枚 maxDiff=0。

### M68b — 部屋の残響 (連続補間) + 鳴る波 (波→一発再生) + 足音 WAV 4 本

**変更**:
- `AudioSystem.h/.cpp`: `SetReverbOverride` / `ClearReverbOverride` / `ReverbOverrideActive`、
  `ApplyReverbParams` の 1 箇所で選択。`Audio\AcousticAudio.*` に `LerpI3dl2(a, b, t)` (純関数)。
- `AudioSourceSystem`: 開放度 → t → 平滑 → override 適用 (`|Δt| > 0.01` でのみ)、`Reset()` で Clear、
  `Detour/Occluded` に `detourWet`。`PushWaveShot` + `Update` 先頭の無条件 clear + drain
  (`ResolveSoundKey` → `MakePlayDesc` (rng_) → 波の 3D ブロック → `ShapeAcousticSpatial` → `audio.Play`)。
- `TickRunner.cpp`: `if (!ts.resim)` ブロックに波の走査と push。`AcousticField.h:54` のコメント更新。
- `AudioMixerWindow.cpp` + `LocalizationTable.inl`: `Mixer_AcousticOverride` (en/ja、`###` 不要)。
- `assets\audio\step_soft/wood/hard/metal.{wav,sound.json,meta}` (SynthParams を申し送りに記録)。
- デモ: `AcousticAudio.toneSound0..3` に 4 本を設定。`Walker` の足音 (床材 6 種)、`Impact Box` の落下、
  `WavePinger`、敵 2 体の自発音、`Watcher` の投擲がすべて鳴る。呼吸は `minWaveVolume` で無音。
- selftest 群を足す: (14) `LerpI3dl2` の端点一致と単調 (15) 開放度 → t の smoothstep 端点 (16) キューが
  `Reset` / `Update` 先頭で空になる (17) `minWaveVolume` 未満は drain されない (18) tone 0..3 → 鍵の引き当て。

**受け入れ**: 廊下 → 部屋 B で残響が段差なく伸びる (log の `room` が 0.2 → 0.7 級へ連続に動く)。
床材タイルを歩く Walker の足音が金属で遠くまで・カーペットで数歩で消える = **波の絵と一致**。
記録/検証 (`replay_verify`) 中は 1 音も鳴らず (`SetSuspended`)、解除後に一斉に鳴らない。
ミキサー窓の preset combo は資産値のまま、override 表示が出る。replay / golden 無風。

### M68c — 仕上げ (文書・手動手順・計画の申し送り)

- `engine_spec.md` §10.6 末尾 (`:1655` の `---` の前) に `**Audible output (M68).**` 段落
  (既存の「太字リード + 散文」様式。判断 1〜5 の要旨と「sim 状態ゼロ」の主張)。
  §10.6 は `10.6.x` の小節を持たないので小節は作らない。`:1701` 付近の "six replay pairs / seventeen golden"
  の**古い記述も直す**。
- `README.md`: `## 主要機能` に**音響伝播の bullet が無い** → M65 + M68 をまとめた 1 bullet を追加。
  `:91-100` の「被覆は 6 シーン」と `:119-123` の「リプレイ照合 6 ペア」を 7 に直す。
- `docs\adr\ADR-017-acoustic-audio.md` (ADR-016 の様式: 決定 / `### なぜ…` (却下案を書く) / 帰結 / 実測 / 既知の制限)。
  M65 に ADR が無いので「波面の 3 役」の決定も 1 節で拾う。
- `docs\test_checklists.md` に `## M68: 音響 × オーディオ (耳で確認)` 節 (`- [ ] 操作 → 期待` 形式)。
- `CLAUDE.md`: selftest 44 → 45、TypeId 末尾 49 → **50 = AcousticAudio** (Cloth/SoftBody 51/52)、
  CLI に `--acoustic-audio-log N`、検証表。
- 本計画の進捗表 + 申し送り。メモリ (`myengine-project.md`) の現在地を更新。

---

## 全サブ共通の検証チェックリスト

1. `pwsh -File tools\gen_project_files.ps1` (新規 .h/.cpp を足したサブのみ)
2. Debug / Release 両構成 0 警告 (`MYE_MSBUILD_ARGS=/p:MyeWarnAsError=true`)
3. `bin\x64\Debug\Editor.exe --selftest` — **45 スイート**全緑 (M68a から)
4. `tools\replay_verify.bat` — **7 ペア / 10 ジョブ 無風** (sim を触っていない証明)
5. `tools\shot_verify.bat` — **既存 22 枚 maxDiff=0** (デモへの追加は MeshRenderer 無し + 末尾 append)
6. `pwsh -File tools\check_rules.ps1` — 0 error (規則 10: 新しい `Tr()` は `Text("%s", Tr(x))` 形)
7. `tools\build_managed.bat Debug` / `Release` (C# は触らないが構成は保つ)
8. **exe の実行は PowerShell ツールから `cmd /c`** ([[running-exes-from-tools]])

**このマイルストーン固有**
- `AcousticField.h` / `.cpp` に diff が **コメント 1 行以外無い**こと (sim ループ不変の機械的証明)。
- `AudioSpatial` / `AcousticAudioComponent` に足したフィールドは**末尾**か。
- `--no-audio` の run でリスナー場が組まれていないこと (Profiler 行が出ない / log が空)。
- 記録 / 検証 / タイムトラベル中に `PushWaveShot` が呼ばれていない (`IsSuspended` gate)。
- ネットのロールバック (`resim`) で二重に鳴らない (`!ts.resim` gate は既存の一発再生と同じ場所)。

---

## 失敗の切り分け表

| 症状 | 最初に疑うもの | 確かめ方 |
|---|---|---|
| replay_verify が割れる | sim を触った (AcousticField の diff / 波表の読み方 / `world.Rng()` 混入) | `git diff -- src/Engine/Engine/Acoustic` が空か。`--hash-diff` |
| golden 18/19 が動く | Watcher のアーキタイプ変更 / デモの生成順 (末尾 append 違反) / MeshRenderer 付きの追加 | `AudioListener` を子エンティティへ。[[golden-diff-triage]] |
| 壁越しなのに素通しで聞こえる | `Bypass` に落ちている (リスナーがグリッド外 = 俯瞰カメラがリスナー) | log の `class`。Watcher に AudioListener が付いているか |
| 全部こもる | 音源セルがソリッド (nudge 不在) / 署名変化で場が古い | log の `dPath` が `Occluded` 固定か。`StaticSignature` の比較 |
| 定位が壁の向こう側 | 到来方向の符号 (`OppositeNeighbor` の取り違え) | selftest (6)。`parentDir` は「親へ向かう向き」 |
| セル境界でブツブツ鳴る | 平滑化が効いていない / dTicks が 0 | selftest (11)。`lastTick_` の上書き順 |
| 検証後に足音が一斉に鳴る | キューの clear が `IsSuspended` return の後 | `Update` 先頭の無条件 clear |
| ミキサーの preset が効かない | override 中 (仕様どおり) | 窓の「音響が上書き中」表示。`AcousticAudio.enabled` を切る |
| 波は見えるのに鳴らない | `toneSound` 未設定 / `minWaveVolume` / `IsReady` false | `[audio] unknown sound key` ログ、log の `shots` |
| CI (`--no-audio`) が遅くなった | リスナー場を `IsReady` の前で組んでいる | Profiler 行が `--no-audio` で出ないこと |

---

## 実装しない / できないもの (v1 の境界)

| 項目 | 理由 / やるなら何が要るか |
|---|---|
| 材質による伝播中の吸収 (音響側と同じ) | 経路依存 = セル配列が sim 状態になる。音側だけでやっても絵と食い違う |
| 動的コライダ (ドア・箱) の遮蔽 | 占有は静的ベイクのみ (M65 と同じ)。二層化が要る |
| 部屋ごとに別の残響バス (複数 reverb) | reverb submix は 1 本。リスナーの部屋で決める + 音源側は送り量で表現 |
| リスナー場の複数リスナー | `FindListener` は 1 人。ローカル 2P は主カメラの耳だけ |
| 一発再生の追従整形 | `Play` 時 1 回 (既存の PlayAtPoint と同じ)。クリップはセル横断より短い |
| ABI 追加 | 1 スロットも増やさない。スクリプトからの調整は v11 `SetComponentField` で `AcousticAudio` を書く |
| HRTF / 高さ方向の定位 | X3DAudio のパンのまま |
| 音の絵 (tone) への反映 | M65 の判断どおり残光は強度のみ |

---

## 実装開始時の手順 (harness)

1. 本ファイルをリポジトリへコピー: `plans\m68-acoustic-audio\plan.md` (M66/M67 と同じ置き場。
   harness の planner が `spec.md` / `sub-NN.md` をこの隣に育てる)。
2. `/harness` を「`plans\m68-acoustic-audio\plan.md` を正本に M68 を 3 サブで実装」で起動。
   planner への申し送り: **ユーザー決定 4 点は確定済み (蒸し返さない)**、判断 1〜7 は設計レビュー
   (Plan エージェント、2026-09-06) を通した内容、TypeId は **50**、選択肢を出すなら
   「一発再生の追従整形」「到来方向の平均化」の 2 点だけ。
3. 1 サブ = 1 コミット (`M68a:` 形式の日本語件名)。コミット本文に「ABI 変更なし / sim 状態変更なし」を明記。
4. 再開手順: `git log --oneline -5` で最後の M68x を確認 → 本ファイルの該当サブ → 進捗表の申し送り。

## 進捗表 (完了時に更新。計画外の事実・罠・申し送りだけ書く)

| サブ | 状態 | 版 / 契約の変更 | メモ |
|---|---|---|---|
| M68a 場 + 遮蔽 + LPF | 未着手 | TypeId 50 (NoHash) / `AudioSpatial` 末尾 1 本 / selftest 45 本目 / `--acoustic-audio-log` | |
| M68b 残響 + 鳴る波 + WAV | 未着手 | `AudioSystem` reverb override 3 関数 / `Mixer_AcousticOverride` / assets 4 本 | |
| M68c 仕上げ | 未着手 | ADR-017 / spec §10.6 / README / test_checklists / CLAUDE.md | |

---

## 別件 (この計画の外)

- 案 4 (XPBD 布・ソフトボディ、M60'e〜n の再開) は 2026-09-06 20:00 にセッション内リマインドを設定済み
  (`CronCreate`、セッションを閉じると消える)。
