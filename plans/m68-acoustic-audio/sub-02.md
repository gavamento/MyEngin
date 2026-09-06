# sub-02: 部屋の残響 (連続補間) + 鳴る波 (`PendingWaveShot`) + 足音 WAV 4 本 (M68b)

- 依存: sub-01
- 状態: 未着手
- 往復: 0

## やること

spec §4.1.3 / §4.1.4 / §4.1.6 (M68b 分) / §4.2 (足音・tone) / §4.3 (ミキサー窓) を実装する。

1. `AudioSystem.h/.cpp`: `AudioReverbParams` (POD、13 フィールド、spec S19) / `static AudioReverbParams PresetReverbParams(int)` /
   `SetReverbOverride(const AudioReverbParams&)` / `ClearReverbOverride()` / `bool ReverbOverrideActive() const`。
   **`ApplyReverbParams()` (`:589-600`) の 1 箇所**で「override があればそれ、無ければ `kReverbPresets[idx]`」。
   `.cpp` に SDK 型との `static_assert(sizeof(AudioReverbParams) == sizeof(XAUDIO2FX_REVERB_I3DL2_PARAMETERS))` と相互変換。
2. `AcousticAudio.h/.cpp`: `LerpReverbParams(a, b, t)` (純関数、int は float 補間 → 四捨五入)、`RoomBlend(openness, small, large)`
   (= smoothstep)、`MakeWaveShotPlay(const PendingWaveShot&, const AcousticAudioComponent&, const AcousticProbe&, listenerPos,
   const AudioSystem&, const SoundLibrary&, Pcg32&, PlayDesc&, AudioSpatial&, AcousticShapeInfo*) -> WaveShotResult`
   (Played / BelowMin / UnknownKey / Stream。`Play` は呼ばない)。
3. `AudioSourceSystem`: `PushWaveShot(const PendingWaveShot&)` (上限 `kMaxPendingShots = 64`、超過は捨てて数える)、
   `Update` **先頭で無条件に**キューをローカルへ move して clear (`:330` の return より前)、`Reset()` で clear + `ClearReverbOverride`。
   probe 更新の後: (a) `room`: `t_target = RoomBlend(...)` → `roomSmoothTicks` で平滑 → `|t − appliedT| > 0.01` で override
   (有効な AcousticAudio / probe が無ければ Clear) (b) per-voice 整形に `detourWet` (`ShapeAcousticSpatial` の表どおり)
   (c) shot の drain: `MakeWaveShotPlay` → `audio.Play`。stats に `shots / skipped / unknownKey` を足す。log 行末に ` room=%.2f`。
4. `TickRunner.cpp:570-576`: `if (!ts.resim)` ブロック内、`ApplyScriptAudioEvent` ループの**後**に波の走査と push (spec §4.1.4 の gate)。
   `AcousticField.h:54` の `bornTick` コメント「診断用」→「診断用 + M68b の出力レーンが『この tick に生まれた波』を拾う鍵」。
   **これが `src\Engine\Engine\Acoustic\` の唯一の diff** (A6)。
5. `AudioMixerWindow.cpp:382-389` の combo の後: `ReverbOverrideActive()` なら `SameLine` + `TextDisabled("%s", Tr(StrId::Mixer_AcousticOverride))`。
   `LocalizationTable.inl:642` の隣に `MYE_STR(Mixer_AcousticOverride, "(acoustics override this preset)", "(音響が上書き中)")`。
6. `assets\audio\step_soft / step_wood / step_hard / step_metal` の `.wav` + `.sound.json` + `.meta` ×2 (spec §4.2 の目安。
   `SynthParams` を**実装メモに全部書く**)。
7. `DemoContent.cpp` の `Acoustic Audio` に `toneSound0..3 = "step_soft" / "step_wood" / "step_hard" / "step_metal"`。
7b. `Components.h` の `AcousticAudioComponent::openLarge` 既定 **0.6 → 0.8** (spec 変更履歴 #7。sub-01 実測: 部屋 A で
    0.33〜0.62 が上端に張り付く)。`openSmall` 0.2 は据え置き — 実機ログの `open=` が廊下で ≥ 0.25 になるなら「不安・質問」で報告
    (planner が下端を動かす)。
7c. sub-01 の申し送りどおり、log の `kind=` を shot 側で `"shot"` にし、`src=-1 name=tone<k>` を出す。
8. `EngineLoop.cpp` summary 行に ` shots=%d skipped=%d unknownKey=%d`、`ProfilerWindow.cpp` の行に `, shots %d, room t=%.2f`。
9. `AcousticAudioSelfTest.cpp` に T16〜T21。

## やらないこと (このサブでは)

- 一発再生の追従 (S17)。`AcousticAudio` のフィールド追加 (M68a で確定済み)。文書 (M68c)。
- `audioScriptRng` を触ること (スクリプトの一発再生の乱数列を動かさない)。`default.mixer.json` の変更。

## 触る場所 (planner の見立て)

| ファイル | 場所 | 何を |
|---|---|---|
| `src\Engine\Engine\Audio\AudioSystem.h` | `AudioSpatial` の後 / class 内 `SetReverbPreset` の隣 / private に `override_` + `overrideActive_` | POD + 4 関数 |
| `src\Engine\Engine\Audio\AudioSystem.cpp` | `:43-52` の隣に変換、`:589-600` | 選択 1 箇所 |
| `src\Engine\Engine\Audio\AcousticAudio.h/.cpp` | 末尾 | 3 関数 |
| `src\Engine\Engine\Audio\AudioSourceSystem.h/.cpp` | `Update` 先頭 / probe 更新の後 / `Reset` | キュー・room・drain |
| `src\Engine\Engine\TickRunner.cpp` | `:570-576` | push |
| `src\Engine\Engine\Acoustic\AcousticField.h` | `:54` | コメント 1 行 |
| `src\Editor\Windows\AudioMixerWindow.cpp` | `:389` の後 | 表示 |
| `src\Engine\Core\LocalizationTable.inl` | `:642` の隣 | 1 行 |
| `src\Engine\Engine\DemoContent.cpp` | `Acoustic Audio` エンティティ | tone 名 |
| `src\Engine\Engine\EngineLoop.cpp` / `ProfilerWindow.cpp` | summary / 行 | 欄追加 |
| `assets\audio\` | 新規 16 ファイル | 足音 |

## 受け入れ条件 (このサブ)

spec §5 の A1〜A9 (無風) と A15〜A19。selftest の内訳:

| T | 内容 | 期待 |
|---|---|---|
| T16 | `LerpReverbParams` | `t=0` で a と 13 フィールド全部一致、`t=1` で b と一致、`t=0.5` で `Room` / `DecayTime` が中点 (int は四捨五入)、t を 0→1 で刻んで `DecayTime` 単調 |
| T17 | `RoomBlend` | `openness ≤ openSmall` → 0、`≥ openLarge` → 1、中点 → 0.5、単調 |
| T18 | キュー | `PushWaveShot` ×3 → `Reset()` → 空。`PushWaveShot` ×3 → 未 Init の `AudioSystem` (IsReady false) で `Update` → 戻った時点で空 (= return より前に clear されている)。65 個 push → 64 個 + dropped 1 |
| T19 | `minWaveVolume` | `amplitude · waveVolume < minWaveVolume` の shot → `BelowMin`、`PlayDesc` 不変 |
| T20 | tone → 鍵 | `SoundLibrary` に `step_soft..metal` を `Register`、`toneSound0..3` 設定 → tone 0..3 それぞれの shot が対応する asset の clip を引く。未設定の tone → `UnknownKey` |
| T21 | 波 → spatial + detourWet | `Wave{maxRing 20, amp 0.7, cell 0.5}` → `minDistance == 0.5`、`maxDistance == 10`、`rolloff == waveRolloff`、`dopplerScale == 0`、`reverbSend == waveReverbSend`、`volume == 0.7·waveVolume·gain`。L-maze で Detour の音源 → `reverbSend == min(1, waveReverbSend + detourWet)`、Direct → `waveReverbSend` のまま |

## 検証コマンド

sub-01 と同じ (A1〜A9)。加えて A16 (sub-01 の A11 コマンドをそのまま。`kind=shot` 行と `room=` 欄を数える)。
A6 は `git diff 8e4272e --stat -- src/Engine/Engine/Acoustic` が `AcousticField.h | 2 +-` 級 (1 行) であること。

## 実装メモ (coder が追記)

## フィードバック履歴
