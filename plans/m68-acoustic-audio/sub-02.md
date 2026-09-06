# sub-02: 部屋の残響 (連続補間) + 鳴る波 (`PendingWaveShot`) + 足音 WAV 4 本 (M68b)

- 依存: sub-01
- 状態: OK (commit 01183b1)
- 往復: 2

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
7b. `Components.h` の `AcousticAudioComponent::openLarge` 既定 **0.6 → 0.8** (spec 変更履歴 #7。round 1 で済み)。
    **round 2: `openSmall` 既定 0.2 → 0.30** (spec 変更履歴 #10 / S23。coder 実測: 廊下 0.29〜0.50、部屋 A 中央 0.67、
    部屋 B 0.80。コメントに実測値と「部屋の隅と廊下の端は区別できない指標」の注記を残す)。
7c. sub-01 の申し送りどおり、log の `kind=` を shot 側で `"shot"` にし、`src=-1 name=tone<k>` を出す。
8. `EngineLoop.cpp` summary 行に ` shots=%d skipped=%d unknownKey=%d`、`ProfilerWindow.cpp` の行に `, shots %d, room t=%.2f`。
9. `AcousticAudioSelfTest.cpp` に T16〜T21。
10. (round 2、should) `AudioSourceSystem` の drain で `audio.Play(desc)` の戻り値が `!Valid()` なら `acStats_.shotsPlayFailed`
    を +1。summary の**末尾**に ` playFailed=%d`。A16 (d) に `playFailed == 0` が加わる (spec 変更履歴 #12)。
11. (round 2、nit、任意) shot の log `src=` を `shot.source.index` (null なら −1) に (spec 変更履歴 #13)。`name=tone<k>` はそのまま。

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

### round 1

```
SELF_EVAL: sub-02 (round 1)
実装:
  - src/Engine/Engine/Audio/AudioSystem.h — AudioReverbParams (I3DL2 13 フィールドの POD、
    AudioBusState の直後) / PresetReverbParams (static) / SetReverbOverride / ClearReverbOverride /
    ReverbOverrideActive、private に reverbOverride_ + reverbOverrideActive_
  - src/Engine/Engine/Audio/AudioSystem.cpp — SDK 型との相互変換 ToPod / FromPod +
    static_assert(sizeof ==)、**ApplyReverbParams() の 1 箇所**で override / プリセットを選択
  - src/Engine/Engine/Audio/AcousticAudio.h/.cpp — LerpReverbParams ((1-t)*a + t*b の形。
    端点が厳密一致する / 整数は lround) / RoomBlend (smoothstep、下端 >= 上端は段に倒す) /
    WaveShotResult / MakeWaveShotPlay。ShapeAcousticSpatial の手順 6 に
    「Detour/Occluded は io.reverbSend += detourWet (上限 1)」を追加
  - src/Engine/Engine/Audio/AudioSourceSystem.h/.cpp — PushWaveShot (上限 64、超過は数える) /
    PendingShotCount (テスト用) / Reset(AudioSystem&)、Update の**先頭**でキューを swap して空に、
    probe 更新の後に room (RoomBlend → roomSmoothTicks 平滑 → |Δt| > 0.01 で SetReverbOverride、
    非 active は ClearReverbOverride)、音源ループの後に shot の drain (MakeWaveShotPlay → Play)、
    log 行末に room=、stats に shots / shotsSkipped / shotsUnknownKey / shotsDropped / roomT
  - src/Engine/Engine/TickRunner.cpp — !ts.resim ブロックの ApplyScriptAudioEvent の後で
    Waves() を舐め、active かつ bornTick == ctx.tickIndex を PushWaveShot。
    LoadScene の audioSources.Reset(audioSystem)
  - src/Engine/Engine/Acoustic/AcousticField.h — bornTick のコメント 1 行 (A6 の唯一の diff)
  - src/Editor/Windows/AudioMixerWindow.cpp / src/Engine/Core/LocalizationTable.inl —
    ReverbOverrideActive() のとき SameLine + TextDisabled("%s", Tr(Mixer_AcousticOverride))、
    en/ja 1 行追加
  - src/Engine/Core/Components.h — AcousticAudioComponent::openLarge 0.6 → 0.8 (7b)
  - src/Engine/Engine/DemoContent.cpp — Acoustic Audio に toneSound0..3 = step_soft/wood/hard/metal
  - src/Engine/Engine/EngineLoop.cpp — summary に shots / skipped / unknownKey / dropped / room
  - src/Editor/Windows/ProfilerWindow.cpp — 行末に , shots %d, room t=%.2f
  - src/Engine/Engine/Audio/AcousticAudioSelfTest.cpp — T16〜T21 (23 アサート、合計 70)
  - assets/audio/step_soft|wood|hard|metal.wav + .sound.json + .meta ×2 (新規 16 ファイル)

足音 4 本の SynthParams (SynthCore で焼いた実値。**これだけで再現できる**。
共通: duty 以外に書いていない値は SynthParams の既定、sampleRate 44100 / channels 1 /
noiseSeed 0x9E3779B97F4A7C15 (既定)):
  step_soft : wave=Noise    freqStart=freqEnd=200.0 (Noise は周波数を見ない) durationSec=0.12
              amplitude=0.35 duty=0.5 attack=0.005 decay=0.05 sustain=0.15 release=0.06
              → 5292 frames / 10628 bytes / peak 11275 / 先頭 0 末尾 0
  step_wood : wave=Triangle freqStart=180.0 freqEnd=90.0  durationSec=0.16
              amplitude=0.50 duty=0.5 attack=0.004 decay=0.06 sustain=0.25 release=0.09
              → 7056 frames / 14156 bytes / peak 16015 / 先頭 0 末尾 1
  step_hard : wave=Square   freqStart=900.0 freqEnd=300.0 durationSec=0.10
              amplitude=0.45 duty=0.3 attack=0.002 decay=0.04 sustain=0.20 release=0.055
              → 4410 frames / 8864 bytes / peak 14740 / 先頭 0 末尾 -1
  step_metal: wave=Sine     freqStart=1800.0 freqEnd=1500.0 durationSec=0.45
              amplitude=0.50 duty=0.5 attack=0.002 decay=0.12 sustain=0.35 release=0.32
              → 19845 frames / 39734 bytes / peak 16372 / 先頭 0 末尾 0
  (4 本とも attack+decay+release < durationSec なので Envelope の比例縮小は掛かっていない。
   末尾がほぼ 0 = ブツ切りのクリックが出ない)

.meta の guid と .sound.json の clip (S11 の一致):
  step_soft .wav=5a1e7c04b9f3268d (=6493764072456267405)  .sound.json=8d2f6b15c0a4739e
  step_wood .wav=4c3d9e26a1b5f708 (=5493721009220876040)  .sound.json=7e0a1f38d2c64b95
  step_hard .wav=2b96d47f1e83c05a (=3140931432575385690)  .sound.json=9f41c8b063e527da
  step_metal.wav=1d58a3e6740fb92c (=2114620235235440940)  .sound.json=b3e07d51962a4fc8
  (既存 122 個の guid と重複なしを機械確認。.sound.json は bus SE / spatialBlend 1 /
   volume 0.9 + volumeRandom 0.1 (振り切れないよう 0.9 にした) / pitchRandom 0.08 /
   minDistance 1.0 / maxDistance 30 / rolloff logarithmic / dopplerScale 0 / reverbSend 0.35。
   3D 欄は一発再生では波の値で上書きされるので実効値ではない = spec §4.2 のとおり)

仕様との差分:
  - [追加] MakeWaveShotPlay の引数に const AcousticField& field と const AcousticProbe& probe を
    先頭に置いた (sub-02.md の署名は field を含んでいなかった)。ShapeAcousticSpatial が
    field を要求する (sub-01 の [追加] 2) ため。同じ理由で戻り値以外の並びも
    (field, probe, comp, shot, listenerPos, audio, sounds, rng, outDesc, outSpatial, info) に揃えた
  - [追加] AudioSourceSystem::Reset() → Reset(AudioSystem& audio)。spec §4.1.3 が
    「Clear の契機 = Reset()」と書いているが、Reset は AudioSystem を持っていなかった。
    呼び出し元は TickRunner の 1 箇所だけ (同じ行の直前に audioSystem.StopAll() がある)
  - [追加] AcousticAudioStats に shotsDropped、summary 行末に ` dropped=%d room=%.2f`。
    spec §4.1.6 は ` shots=%d skipped=%d unknownKey=%d` までしか定めていないが、
    T18 の「65 個目は捨てて数える」を外から観測する口が要るのと、room の最終値が
    run-to-run 比較で効くため。**追加は末尾**なので既存の grep は壊れない
  - [追加] MakeWaveShotPlay で outDesc.loop = false を強制。ループする .sound.json を
    toneSound に指すと一発再生が鳴りっぱなしになるため (spec は触れていない)
  - [追加] PickVariationIndex が -1 (クリップ未割当) のとき UnknownKey を返す。
    spec の分類には無いが、黙って無音を Play するより数えたほうがよい
  - [追加] AudioReverbParams の既定値を I3DL2 の DEFAULT プリセットにした (ゼロ初期化だと
    DecayTime = 0 が SDK の有効範囲外)。spec は既定値を定めていない
  - [追加] tone の鍵が引けないときの警告を **tone ごとに 1 回**にする抑制フラグ
    (spec §4.1.4 の「tone ごとに 1 回だけ」をメンバで実装。Reset で戻す)
  - [逸脱なし] それ以外は spec §4.1.3 / §4.1.4 / §4.1.6 / §4.2 / §4.3 のとおり

検証:
  - MSBuild Debug/Release x64 (/p:MyeWarnAsError=true) → 0 error / 0 warning (A5)
  - tools\build_managed.bat Debug / Release → 両方 0 warning / 0 error (A5)
  - pwsh -File tools\gen_project_files.ps1 → **.vcxproj に diff なし** (新規ソースは無い)
  - cmd /c bin\x64\Debug\Editor.exe --selftest → exit 0 / FAIL 0 件。
    45 本目 "Acoustic audio self test: ALL PASS" = T1〜T21 の 70 アサート。
    T16〜T21 の 23 本はすべて新規 PASS (A1 / A15)
  - tools\replay_verify.bat → [PASS] 10 ジョブ / 7 シーン / 99.3s、規則検査 0 error (A2)
  - tools\shot_verify.bat → **2 回とも exit 0 / 22 枚すべて maxDiff=0 diffPixels=0** (A3)。
    sub-01 で 1 回だけ出た「1 shot(s) differ」は今回は再現しなかった (2/2 緑)
  - pwsh -File tools\check_rules.ps1 → 0 error / 0 warning (A4)
  - git diff 8e4272e --stat -- src/Engine/Engine/Acoustic → `AcousticField.h | 2 +-` の 1 行のみ (A6)
  - git diff 8e4272e -- src/Shared/EngineAPI.h src/Scripting → 空 (A7)
  - A8/A9: T1 が TypeId 50 / kComponentNoHash / kSimSnapshotVersion == 11 を PASS。
    追加したフィールドは AudioReverbParams (新規型) / AcousticAudioStats の末尾 /
    AcousticAudioComponent は**フィールド追加なし** (openLarge の既定値だけ変更)
  - A16 (Release Runtime、sub-01 と同一コマンド ×2):
      summary: ticks=601 rebuilds=66 boxCells=16224 probeMsAvg=0.609/0.633 shaped=652
               classes D/T/O/B=23/629/0/0 shots=51 skipped=0 unknownKey=0 dropped=0 room=0.14
      (a) kind=shot = **51 行** (>= 20) → 予測どおり
      (b) shot の class は **Direct 23 / Detour 28** の両方。tone の内訳は
          tone0=7 / tone1=33 / tone2=4 / tone3=7 = 4 音色すべてが実際に鳴っている → 予測どおり
      (c) room= の tick 間 max|Δroom| = **0.03** (<= 0.05、t >= 2)。room は run 中に
          0.98 → 0.13 まで動いた (= 開放度が実際に響きを変えている)。
          ※ t=1 の 1 行だけ占有未ベイク (S21) で open=1.00 → room が 1.0 にスナップし、
            そこから 300ms 半減期で降りてくる。帯には収まっている
      (d) shots=51 (>= 20)、unknownKey=0 → 予測どおり
      (e) 2 run の `[acaudio] t=` 行 **650 行が完全一致** (byte-identical)。
          summary も probeMsAvg 以外一致
      shot の gain は全行 1.000 (Direct/Detour は減衰を rolloff に任せる設計どおり)、
      dPath 2.86〜31.45 m、Detour の lpf は 0.250 (床) 〜 0.327
  - A12 相当: 同じコマンド + --no-audio → [acaudio] 行 0 (summary 含む)
  - A14 相当: 起動ログに `[audio] loaded clip: step_hard/step_metal/step_soft/step_wood`
    (それぞれ 1 ch @ 44100Hz / 0.10s / 0.45s / 0.12s / 0.16s)。
    `[audio] unknown sound key` は 0 件
  - A17: T18 が「Reset で空」「IsReady false の Update から戻った時点で空」を機械検査。
    加えて TickRunner 側も IsSuspended() で push しない二重ゲート (コード読み)
  - A18: LocalizationSelfTest を含む 45 スイート ALL PASS。ミキサー窓は combo が
    audio.ReverbPreset() (= 資産値) を読み、上書き中の注記だけを SameLine で足す
    (コード読み。**GUI の目視はしていない**)
  - 独立検算 (廊下の開放度、下記「不安・質問 1」): --acoustic-demo と同じ間取りを
    World に組んで AcousticField::Sync に焼かせる使い捨てプローブ (Engine.lib にリンク) で
    openness を実測。デモの部屋 A の実測 (0.33〜0.62) と整合した

自己採点 (1-5):
  仕様適合: 4 — A1〜A9 / A15〜A19 は全部成立。[追加] 7 件はすべて上に明記してあり、
    うち 2 件 (MakeWaveShotPlay の署名 / Reset のシグネチャ) は spec の署名が
    実装に必要な引数を落としていたことの穴埋め。残り 5 件は小さく可逆な補強
  正しさ: 4 — selftest 70 アサート / replay 7 ペア / golden 22 枚 ×2 / 規則検査が緑で、
    実機ログは 2 run バイト一致。5 にしないのは (i) 「実際に voice が立ち上がって
    音が出ている」ことを直接観測していない (Play の戻り値を数える口が無い。クリップの
    ロードと desc.volume > 0 と gain=1 までは確認) (ii) ミキサー窓の注記を GUI で目視していない
  コード品質: 4 — 響きの選択は ApplyReverbParams の 1 箇所、一発再生の組み立ては
    MakeWaveShotPlay の 1 本、遮蔽・回折は ShapeAcousticSpatial の 1 本に閉じていて、
    どれも「規則は 1 本だけ」の既存流儀に合わせた。コメントは「なぜ」と踏んだ罠を日本語で。
    5 にしないのは Update が 200 行近くまで伸びたこと (probe / room / voice / shot が 1 関数)
  テスト: 4 — T16〜T21 の 23 アサートで「端点厳密一致」「四捨五入」「早期 return より前の
    clear」「捨てたら PlayDesc を触らない」「tone → クリップ」「maxDistance = 波の到達上限」
    「Detour の +detourWet」を機械化した。5 にしないのは XAudio2 が実際に出す音
    (reverb APO のパラメータが効いているか) は誰も検査できていないため

不安・質問:
  1. **廊下の開放度が 0.287〜0.496 で、openSmall = 0.2 では「狭い側」に落ちない** (7b の報告)。
     --synth-input の Watcher は 4000 tick 回しても部屋 A を出ない (S8 のとおり。dPath 24.77〜30.73、
     open 0.33〜0.62 のまま) ので、実機ログからは廊下の open= が取れなかった。
     代わりに **同じ間取りを World に組んで AcousticField::Sync に焼かせる使い捨てプローブ**
     (エンジンの占有ベイクと probe をそのまま呼ぶ) で測った既定 roomProbeM=6 の値:
       部屋 A の隅 (map 1,1)   open=0.468 → t=0.42
       部屋 A の中央 (2,2)      open=0.668 → t=0.88
       横廊下 西端 (2,4)        open=0.496 → t=0.49
       横廊下 中央 (5,4)        open=0.357 → t=0.17
       横廊下 東端 (9,4)        open=0.287 → t=0.06
       縦廊下 (9,5) / (9,6)     open=0.404 / 0.529 → t=0.27 / 0.57
       部屋 B 戸口 (9,7)        open=0.607 → t=0.76
       部屋 B 中央 / hum (6,9)  open=0.800 → t=1.00
     openLarge 0.8 は部屋 B がちょうど上端に着く良い値だった。一方 openSmall 0.2 は
     廊下でも t が 0.06〜0.49 あり、特に廊下の**西端と縦廊下**は部屋 A と区別が付かない。
     **openSmall を 0.30 前後へ上げると**廊下中央 t=0.09 / 東端 0.00 / 西端 0.24 になり、
     「廊下は狭い響き / 部屋は広い響き」が素直に出る。既定値を動かすのは planner の裁定
     (spec §4.2 の表と §8 に載る話なので勝手に変えていない)。
  2. **「実際に鳴っている」ことの直接証拠が無い。** ログは Play を呼ぶ**前**に出しているので、
     51 行は「51 回 Play を呼んだ」までしか主張していない。クリップは 4 本ともロード済み
     (起動ログ)、desc.volume は 0 でない、AudioSystem::Play の失敗経路は
     「クリップ表に無い / suspended」の 2 つだけ、までは追えたが、voice が立ち上がった数を
     数える口がエンジンに無い。必要なら AudioSystem に「Play 失敗数」を足す小改修が要る
     (v1 では不要と判断した。耳の確認は M68c の test_checklists)。
  3. sub-02.md 7c は shot 行の src を **-1 固定**と指定しているのでそう実装したが、
     PendingWaveShot::source (発音元エンティティ) は運んでいて未使用。将来
     「自分の音を自分で聞かない」除外やログの追跡に使える。今のままでよいか。

触ったファイル:
  src/Engine/Engine/Audio/AudioSystem.h
  src/Engine/Engine/Audio/AudioSystem.cpp
  src/Engine/Engine/Audio/AcousticAudio.h
  src/Engine/Engine/Audio/AcousticAudio.cpp
  src/Engine/Engine/Audio/AcousticAudioSelfTest.cpp
  src/Engine/Engine/Audio/AudioSourceSystem.h
  src/Engine/Engine/Audio/AudioSourceSystem.cpp
  src/Engine/Engine/Acoustic/AcousticField.h
  src/Engine/Engine/TickRunner.cpp
  src/Engine/Engine/EngineLoop.cpp
  src/Engine/Engine/DemoContent.cpp
  src/Engine/Core/Components.h
  src/Engine/Core/LocalizationTable.inl
  src/Editor/Windows/AudioMixerWindow.cpp
  src/Editor/Windows/ProfilerWindow.cpp
  assets/audio/step_soft.wav
  assets/audio/step_soft.wav.meta
  assets/audio/step_soft.sound.json
  assets/audio/step_soft.sound.json.meta
  assets/audio/step_wood.wav
  assets/audio/step_wood.wav.meta
  assets/audio/step_wood.sound.json
  assets/audio/step_wood.sound.json.meta
  assets/audio/step_hard.wav
  assets/audio/step_hard.wav.meta
  assets/audio/step_hard.sound.json
  assets/audio/step_hard.sound.json.meta
  assets/audio/step_metal.wav
  assets/audio/step_metal.wav.meta
  assets/audio/step_metal.sound.json
  assets/audio/step_metal.sound.json.meta
  plans/m68-acoustic-audio/sub-02.md

申し送り:
  - **M68c (文書) が拾うもの**: CLI は増えていない (--acoustic-audio-log のまま)。
    セルフテストの本数も 45 のまま (T が 15 → 21 に増えただけ)。CLAUDE.md の
    「コンポーネントを足す」チェックリストは TypeId 50 のままで変更不要。
    engine_spec §10.6 / ADR-017 には「響きの選択は ApplyReverbParams の 1 箇所」
    「波 → 一発再生は tick で push / フレームで drain」「reverb override は
    reverbPreset_ に触らない (combo は資産値のまま)」を書くとよい。
  - **summary の書式が伸びた**: `... classes D/T/O/B=%d/%d/%d/%d shots=%d skipped=%d
    unknownKey=%d dropped=%d room=%.2f`。reviewer のレシピは正規表現で拾えば無傷。
  - **openSmall の裁定待ち** (不安・質問 1)。既定を動かすなら Components.h の 1 行だけで、
    replay / golden には影響しない (NoHash / 出力レーン)。
  - AudioSourceSystem::Update が 4 つの仕事 (probe / room / voice / shot) で 200 行近い。
    分割する余地はあるが、**順序に意味がある** (probe → room → voice → shot) ので、
    切るなら「順序の理由」をコメントで縛ってからにすること。
  - 作業ツリーの改行: Edit は LF で書き戻した。.meta 4×2 だけは既存 (エディタ生成) に
    合わせて **CRLF・末尾改行なし**で書いた (blob は LF になる)。
  - 一時プローブ 2 本 (WAV を焼く / 間取りの開放度を測る) は scratchpad に置いただけで
    リポジトリには 1 バイトも残していない。どちらも bin\x64\Release\Engine.lib に
    直接リンクして cl で単体ビルドした (再現手順は上の SynthParams と本メモ)。
```

### round 2

```
SELF_EVAL: sub-02 (round 2)
実装:
  - #1 (must): src/Engine/Core/Components.h — AcousticAudioComponent::openSmall 0.2 → 0.30。
    コメントに実測 9 点 (廊下 東 0.287 / 中 0.357 / 西 0.496、縦廊下 0.404・0.529、
    部屋 A 隅 0.468 / 中央 0.668、部屋 B 戸口 0.607 / 中央 0.800) と、
    「**この指標は部屋の隅と廊下の端を区別できない** (廊下西端 0.50 > 部屋 A 隅 0.47)。
    局所の自由体積しか見ておらず囲まれ方の形は見ていない = v1 の割り切り。だから下端は
    『廊下の中央付近が 0 に落ちる』ところへ置く」を残した
  - #2 (should): AcousticAudio.h の AcousticAudioStats に shotsPlayFailed、
    AudioSourceSystem.cpp の drain で `if (!audio.Play(desc).Valid()) ++acStats_.shotsPlayFailed;`
    (戻り値を捨てない)、EngineLoop.cpp の summary **末尾**に ` playFailed=%d`
  - #3 (nit): AudioSourceSystem.cpp の shot ログの `src=` を
    `shot.source.IsNull() ? -1 : (int)shot.source.index` に。`name=tone<k>` は据え置き
    (drain は tick の後なので発音元が死んでいることがあり、名前の借用には IsAlive 検査が要る、
    とコメントに理由を残した)
検証:
  - MSBuild Debug/Release x64 (/p:MyeWarnAsError=true) → 0 error / 0 warning (A5)
  - tools\build_managed.bat Debug / Release → 両方 0 warning / 0 error (A5)
  - cmd /c bin\x64\Debug\Editor.exe --selftest → exit 0 / FAIL 0、45 本目 ALL PASS
    (T1〜T21 の 70 アサート。T17 は openSmall/openLarge をテスト内で明示指定しているので
     既定値の変更に影響されない) (A1 / A15)
  - tools\replay_verify.bat → [PASS] 10 ジョブ / 7 シーン / 112.9s、規則検査 0 error (A2)
  - tools\shot_verify.bat → **完走 2 回とも exit 0 / 22 枚すべて maxDiff=0** (A3)。
    ★★間に**割れた run が 1 回**あり、**枚名を捕まえた**: `acoustic_deferred`
    `maxDiff=125 diffPixels=520 (tol=3) worst pixel (236,444)`。**原因を特定した** (下記)
  - pwsh -File tools\check_rules.ps1 → 0 error / 0 warning (A4)
  - git diff 8e4272e --stat -- src/Engine/Engine/Acoustic → `AcousticField.h | 2 +-` のみ (A6)
  - git diff 8e4272e -- src/Shared/EngineAPI.h src/Scripting → 空 (A7)
  - A8/A9: T1 PASS (TypeId 50 / NoHash / snapshot 版 11)。round 2 の追加は
    AcousticAudioStats::shotsPlayFailed (**末尾**) のみ。AcousticAudioComponent は
    フィールド追加なし (openSmall の既定値だけ変更)
  - A16 (Release Runtime、同一コマンド ×2):
      summary: ticks=601 rebuilds=66 boxCells=16224 probeMsAvg=0.620/0.638 shaped=652
               classes D/T/O/B=23/629/0/0 shots=51 skipped=0 unknownKey=0 dropped=0
               **room=0.02 playFailed=0**
      (a) kind=shot = 51 行 (>= 20)
      (b) shot の class は Direct 23 / Detour 28 の両方 (openSmall は整形に効かないので round 1 と同じ)
      (c) room= の tick 間 max|Δroom| = 0.03 (<= 0.05、t >= 2)。行ごとの room は
          0.02 〜 0.97 (0.97 は S21 の tick 1 スナップから 18 tick 半減期で降りてくる途中)
      (d) shots=51 / unknownKey=0 / dropped=0 / **playFailed=0** → 51 回すべて voice が立った
      (e) 2 run の `[acaudio] t=` 行 650 行が完全一致 (byte-identical)。summary も probeMsAvg 以外一致
      (f) **予測 0.00〜0.75 に対し summary の room = 0.02** → 予測どおり
          (round 1 は openSmall 0.2 で 0.14。終端の開放度 0.347 が u=(0.347-0.30)/0.5=0.094 →
           smoothstep で 0.025 になる計算とも一致)
      shot の `src=` は 88 (Wave Source) / 96 (Walker) / 97 / 100 / 101 (敵 2 体) の 5 種が出た
      = 発音元が読めるようになった (nit #3 の効果)
  - **shot_verify のフレークを特定した (sub-01 申し送りの「1 shot(s) differ、枚名未捕捉」の正体)**:
    枚名 = `acoustic_deferred`、maxDiff=125 / diffPixels=520 / tol=3 / worst pixel (236,444)。
    切り分け (golden-diff-triage の 4 点計測):
      (1) 割れた PNG を保存して再照合 → 何度 img-diff しても同じ差分 = **画像そのものが違う**
          (照合器のブレではない)
      (2) 同じコマンドを単体で **6 連続実行 → 6/6 maxDiff=0** = 常時割れではない
      (3) 差分の位置 (236,444) は俯瞰画の左下 = **部屋 A の隅に立つ Watcher の箱**。
          ヒートマップも箱 1 個ぶんの小さな塊
      (4) `WatcherFpsCamera.cpp:110` が `GetMouseDelta` を `yawDeg` に積分し、:134 で
          `self.SetLocalRotation(rot)` = **プレイヤーの箱 (MeshRenderer 付き) を回している**。
          デルタの出所は `Input.cpp:40` の `WM_INPUT` = **物理マウスの生カウント**。
          shot_verify は acoustic の 2 枚に `--synth-input` を渡さない (golden はその画角で
          撮ってある) ので、**撮影中の 123 フレームの間にマウスが動くと箱が回って golden が割れる**。
    → **M68b が原因ではない**ことは構造で言える: acoustic の 2 枚は `--no-audio` で撮るので
      `AudioSourceSystem::Update` は `IsReady()` で即 return し、`TickRunner` の push も
      `audioSystem.IsReady()` で弾かれる = **M68 のコードは 1 行も実行されない**。
      M65g (プレイヤー投入) から埋まっていた性質で、sub-01 の 1 回も同じ枚だったと考えられる。
    → 恒久対策の候補 (このサブの範囲外。申し送りへ): `--screenshot` 指定時に生マウスデルタを
      0 に落とす (frame == tick に倒すのと同じ「撮影モードの決定化」の一種)、
      あるいは acoustic の 2 枚だけ Watcher の yaw を固定する
自己採点 (1-5):
  仕様適合: 5 — 指摘 3 件すべて対応。round 1 の [追加] 7 件は planner が spec §8 #11 で採用済みで、
    round 2 で新たに仕様から外れた点は無い
  正しさ: 5 — **playFailed=0 が取れたので round 1 で 4 に留めた理由 (i) が消えた** =
    「51 回 Play を呼んだ」ではなく「51 回 voice が立った」が機械で言える。
    selftest 70 アサート / replay 7 ペア / golden 22 枚 ×2 / 規則検査が緑、実機ログ 2 run バイト一致。
    残る未確認は「XAudio2 が実際に空気を震わせた音」だけで、これは耳 (M68c の checklists) の領分
  コード品質: 4 — 追加は 3 箇所とも数行で、既定値の変更にはコメントで実測値と指標の限界を残した。
    5 にしないのは round 1 から続く AudioSourceSystem::Update の長さ (4 仕事で 200 行超)
  テスト: 4 — T16〜T21 の 23 アサートは round 1 のまま。playFailed / openSmall の既定値は
    セルフテストではなく実機ログで固定した (前者は voice プールが要る = デバイス依存、
    後者は既定値そのものなので固定テストを書くと「値を変えるとテストが落ちる」だけになる)
不安・質問:
  1. **shot_verify のフレークの恒久対策をどこで打つか** (裁定がほしい)。原因は特定した
     (上の検証欄): `acoustic_*` の 2 枚は `--synth-input` 無しで撮るのに、Watcher の yaw が
     `GetMouseDelta` (= WM_INPUT の生カウント) を積分して**箱の回転に出る**ので、撮影中に
     マウスが動くと golden が割れる。**M68 のコードは --no-audio のこの run で 1 行も
     動いていない**ので M68b の責任ではないが、放置すると「acoustic の 1 枚だけ時々赤い」が
     恒常的な雑音になる。案: (a) `--screenshot` 指定時に `InputSnapshot::mouseDelta*` を 0 に
     落とす 1 行 (frame == tick と同じ「撮影モードの決定化」。golden は動かないはずだが未検証)、
     (b) M65 追補として別コミットに切る、(c) 台帳の申し送りに残して触らない。
     **このサブの範囲外**と判断して手を付けていない。
触ったファイル: (round 1 + round 2 の全量)
  src/Engine/Engine/Audio/AudioSystem.h
  src/Engine/Engine/Audio/AudioSystem.cpp
  src/Engine/Engine/Audio/AcousticAudio.h
  src/Engine/Engine/Audio/AcousticAudio.cpp
  src/Engine/Engine/Audio/AcousticAudioSelfTest.cpp
  src/Engine/Engine/Audio/AudioSourceSystem.h
  src/Engine/Engine/Audio/AudioSourceSystem.cpp
  src/Engine/Engine/Acoustic/AcousticField.h
  src/Engine/Engine/TickRunner.cpp
  src/Engine/Engine/EngineLoop.cpp
  src/Engine/Engine/DemoContent.cpp
  src/Engine/Core/Components.h
  src/Engine/Core/LocalizationTable.inl
  src/Editor/Windows/AudioMixerWindow.cpp
  src/Editor/Windows/ProfilerWindow.cpp
  assets/audio/step_soft.wav / .wav.meta / .sound.json / .sound.json.meta
  assets/audio/step_wood.wav / .wav.meta / .sound.json / .sound.json.meta
  assets/audio/step_hard.wav / .wav.meta / .sound.json / .sound.json.meta
  assets/audio/step_metal.wav / .wav.meta / .sound.json / .sound.json.meta
  plans/m68-acoustic-audio/sub-02.md
申し送り:
  - **summary の最終形**: `[acaudio] summary: ticks= rebuilds= boxCells= probeMsAvg= shaped=
    classes D/T/O/B=a/b/c/d shots= skipped= unknownKey= dropped= room= playFailed=`。
    追加は常に末尾なので既存の grep / 正規表現は無傷。
  - **playFailed は「ログは緑なのに無音」を捕まえる唯一の口**。M68c の test_checklists には
    「耳で聞こえないのに playFailed=0 なら XAudio2 側 (ミキサーのミュート / バス音量) を疑う」
    と書いておくとよい。
  - openSmall 0.30 / openLarge 0.8 は**デモの間取りに合わせた値**。他のシーンでは
    roomProbeM と一緒に測り直すのが正しい (指標が「部屋の隅と廊下の端」を区別できない
    ことは Components.h のコメントに残した)。
  - ★★**shot_verify の `acoustic_forward` / `acoustic_deferred` は生マウスデルタに晒されている**
    (M65g からの性質、M68 とは無関係)。撮影中にマウスが動くと Watcher の箱が回って割れる。
    reviewer / CI で「acoustic の 1 枚だけ割れた」が出たら、まず**撮影中に机を触っていないか**を
    疑うこと (コードを疑う前に除外する、が replay-verify-triage と同じ手順)。
    恒久対策 (M68c 以降 or M65 追補の候補): `--screenshot` 指定時に `InputSnapshot::mouseDelta*`
    を 0 にする 1 行。frame == tick に倒すのと同じ「撮影モードの決定化」で、
    golden は 1 画素も動かない (無入力の run と同値になるだけ) はず — ただし**未検証**。
  - 一時プローブ 2 本 (WAV を焼く / 間取りの開放度を測る) は scratchpad のみ。リポジトリには残っていない。
```

## フィードバック履歴

- round 1: **VERDICT REWORK** (planner、2026-09-06)。A1〜A9 / A15〜A19 は SELF_EVAL の検証欄と実コード (`ApplyReverbParams` の
  1 箇所 / `Update` 先頭の swap / `TickRunner` の push gate / `MakeWaveShotPlay` の spatial / `AcousticField.h` 1 行 / ミキサー窓 /
  `step_metal.sound.json`) で突き合わせて成立。差分 7 件は全部仕様側の穴として spec §8 #11 に採用。REWORK の理由は
  **must 1 件 = `openSmall` 0.2 → 0.30** (coder の実測 S23 に基づく裁定。M68c はコードを触らないのでここで直す)。
  should 1 件 = `shotsPlayFailed` (質問 2 の答え)、nit 1 件 = shot の `src=` (質問 3、任意)。
- round 2: **VERDICT OK** (planner、2026-09-06)。must / should / nit の 3 件とも実コードで確認 (`Components.h` openSmall 0.30 +
  実測 9 点のコメント / `AudioSourceSystem.cpp:671` の `!audio.Play(desc).Valid()` 計数と `EngineLoop.cpp:1860` の summary 末尾 /
  `:657` の `src=`)。A16 は予測どおり (room 0.02 ∈ [0, 0.75]、playFailed 0、2 run バイト一致、src 5 種)。
  shot_verify のフレークは coder が枚名 (`acoustic_deferred`) と原因 (撮影中の生マウスデルタ → Watcher の箱の yaw、M65g 由来) まで
  特定 = M68 起因でない。恒久対策 (質問 1) は planner 裁定で **sub-03 に 1 行の例外**として載せる (spec S24 / A26)。
