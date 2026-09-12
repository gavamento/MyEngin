# Impact Sound System 設計案

## 1. 目的

本設計は、既存の `AudioSystem` / `SoundAsset` / `AudioClip` / `SynthCore` を活かしつつ、
以下の効果音を手続き生成して自作エンジンへ統合することを目的とする。

- 足音
- 物体衝突音
- ガラス瓶の衝突音
- ガラス瓶の破壊音

Deep-Modal、FEM、Voxel、ニューラルネットは使用しない。
今回の目的では既存オーディオ基盤が十分に揃っているため、追加実装は
**「PCM を生成する層」** に限定する。

---

# 2. 基本方針

既存の再生系は変更しない。

```text
Game / Physics
      |
      v
 Sound Event
      |
      v
 SoundAsset
      |
      v
 AudioSystem::Play()
      |
      +--> Mixer
      +--> X3DAudio
      +--> Reverb
      +--> Occlusion / Diffraction
```

追加するのはアセット生成側のみ。

```text
Material / Event / Seed
          |
          v
    ImpactSynth
          |
          v
      AudioClip
          |
          v
AudioSystem::RegisterClip()
          |
          v
     SoundAsset
```

手続き生成した音も、最終的には通常の `SoundAsset` として扱う。

---

# 3. 既存システムで利用する機能

現在のエンジンには以下が既に存在するため、新規実装しない。

## AudioClip

内部 PCM 形式は以下。

```cpp
struct AudioClip
{
    std::vector<int16_t> samples;
    uint32_t sampleRate;
    uint16_t channels;
};
```

ImpactSynth は最終的に `AudioClip` を返す。

---

## AudioSystem

既存の以下の機能を利用する。

```cpp
AssetID RegisterClip(
    AssetID id,
    AudioClip clip,
    const std::string& name);

AudioHandle Play(const PlayDesc& desc);
```

再生ボイス、Voice Stealing、XAudio2、3D 定位は既存機能を使用する。

---

## SoundAsset

以下の既存機能を利用する。

- variation
- volumeRandom
- pitchRandom
- priority
- maxInstances
- spatialBlend
- minDistance
- maxDistance
- dopplerScale
- reverbSend

そのため、ImpactSynth 側で再生制御まで実装しない。

---

## SynthCore

既存の以下を再利用する。

- PCM 生成の考え方
- `Pcg32`
- `AudioClip`
- WAV 書き出し
- Pure Function 方針

ImpactSynth も同様に、
**デバイス・XAudio2・ECS に依存しない純関数**
として実装する。

---

# 4. 追加ファイル

推奨構成。

```text
Engine/Engine/Audio/
|
+-- AudioSystem.*
+-- AudioClip.*
+-- AudioMixer.*
+-- SoundAsset.*
+-- SynthCore.*
|
+-- ImpactSynth.h
+-- ImpactSynth.cpp
```

最小化したい場合は `SynthCore.*` に統合してもよいが、
責務分離のため `ImpactSynth.*` を推奨する。

---

# 5. 公開 API

## ImpactSoundKind

```cpp
enum class ImpactSoundKind : int32_t
{
    Footstep,
    Impact,
    GlassBreak
};
```

---

## AcousticMaterial

```cpp
enum class AcousticMaterial : int32_t
{
    Wood,
    Metal,
    Concrete,
    Glass,
    Plastic,
    Grass,
    Gravel
};
```

---

## ImpactSynthParams

```cpp
struct ImpactSynthParams
{
    ImpactSoundKind kind = ImpactSoundKind::Impact;

    AcousticMaterial materialA = AcousticMaterial::Wood;
    AcousticMaterial materialB = AcousticMaterial::Concrete;

    float strength = 1.0f;
    float size = 1.0f;

    float durationSec = 0.4f;

    uint32_t sampleRate = 44100;
    uint64_t seed = 1;
};
```

---

## エントリポイント

```cpp
void ImpactSynthRender(
    const ImpactSynthParams& params,
    AudioClip& out);
```

同じ入力なら同じ PCM を生成する。

---

# 6. 内部設計

ImpactSynth は以下の 3 要素で構成する。

```text
ImpactSynthRender
|
+-- Transient
|
+-- Resonator Bank
|
+-- Shard Events
```

---

# 7. Transient

衝突直後の短い成分。

用途:

- 足音の「コッ」
- ガラス衝突の「カッ」
- 破壊音の「パキッ」

基本式:

```text
noise(t) * exp(-decay * t)
```

例:

```cpp
float transient =
    randomNoise *
    std::exp(-transientDecay * time);
```

長さの目安:

```text
Footstep    5 - 30 ms
Impact      3 - 20 ms
GlassBreak  2 - 15 ms
```

---

# 8. Resonator Bank

材質らしさを作る主要部分。

基本式:

```text
A * exp(-d*t) * sin(2*pi*f*t)
```

6 モード程度から開始する。

```cpp
constexpr float kModeRatios[] =
{
    1.00f,
    1.37f,
    1.91f,
    2.63f,
    3.42f,
    4.71f
};
```

整数倍を避けて、楽器的になりすぎるのを防ぐ。

---

# 9. Material Preset

```cpp
struct ImpactMaterialPreset
{
    float baseFrequency;
    float resonanceDecay;

    float noiseAmount;
    float brightness;

    float shardAmount;
};
```

初期値例。

```text
Wood
 baseFrequency   300 - 500 Hz
 resonanceDecay high
 noiseAmount     medium-high
 brightness      low

Metal
 baseFrequency   700 - 1200 Hz
 resonanceDecay low
 noiseAmount     low
 brightness      high

Glass
 baseFrequency   1200 - 2000 Hz
 resonanceDecay medium
 noiseAmount     low-medium
 brightness      very high

Plastic
 baseFrequency   200 - 400 Hz
 resonanceDecay very high
 noiseAmount     medium
 brightness      low

Concrete
 noiseAmount     high
 resonance       weak

Grass
 noise dominant
 resonance       almost none

Gravel
 many transient clicks
 resonance       weak
```

値は物理精度より聴感調整を優先する。

---

# 10. Size Scaling

物体サイズに応じて共鳴周波数を変える。

```cpp
frequency /= size;
```

例:

```text
小瓶 -> 高い音
大瓶 -> 低い音
```

極端な値を防ぐためクランプする。

```cpp
size = clamp(size, 0.25f, 4.0f);
```

---

# 11. Strength

衝突強度は主に以下へ反映する。

```text
strength
 |
 +--> volume
 +--> transient amount
 +--> high frequency amount
 +--> shard count
```

例:

```cpp
amplitude = baseAmplitude * strength;
```

必要なら非線形にする。

```cpp
amplitude = sqrt(strength);
```

---

# 12. Footstep

構成:

```text
Footstep
|
+-- Short Noise
|
+-- 2 - 4 Resonators
```

地面材質を主成分とする。

```text
Wood      -> low-mid resonance
Metal     -> bright resonance
Concrete  -> dry noise
Grass     -> filtered noise
Gravel    -> multiple short clicks
```

入力:

```cpp
kind      = Footstep
materialB = GroundMaterial
strength  = foot velocity / landing strength
```

---

# 13. Glass Impact

瓶が割れない場合。

構成:

```text
Glass Impact
|
+-- short transient
|
+-- 4 - 6 high resonators
```

周波数例:

```text
900 Hz
1320 Hz
2150 Hz
3470 Hz
5100 Hz
7300 Hz
```

実際には size と seed で少し変化させる。

---

# 14. Glass Break

構成:

```text
GlassBreak
|
+-- Crack
|
+-- Bottle Resonance
|
+-- Shard Events
```

---

## Crack

短いノイズバースト。

```cpp
crack =
    noise *
    exp(-150.0f * t);
```

---

## Bottle Resonance

6 モード程度。

破壊直後に短く鳴らす。

---

## Shard Events

20 - 30 個程度から開始する。

```cpp
struct ShardEvent
{
    float startTime;
    float frequency;
    float amplitude;
    float decay;
};
```

生成例:

```cpp
startTime = Random(0.0f, 0.25f);
frequency = Random(2000.0f, 10000.0f);
amplitude = Random(0.02f, 0.2f);
decay     = Random(20.0f, 100.0f);
```

各 shard は短い減衰 sine としてよい。

```text
time 0ms
|
+-- crack
|
+------ shard
|
|   +--- shard
|
|        +------ shard
|
+-------------------------- 300ms
```

---

# 15. Material Pair

将来的には

```text
Glass x Concrete
Glass x Metal
Glass x Wood
```

のように相手材質も考慮する。

最小実装では materialA を主体にして、
materialB は以下だけ補正する。

```text
Concrete -> transient 増加
Metal    -> brightness 増加
Wood     -> high frequency 減少
Grass    -> transient 減少
```

---

# 16. PCM 生成

内部では float buffer を使う。

```cpp
std::vector<float> mix;
```

処理順:

```text
1. Transient を加算
2. Resonator を加算
3. Shard を加算
4. Peak / Gain 調整
5. Soft Clip または Clamp
6. int16_t 化
7. AudioClip に格納
```

---

# 17. 音割れ対策

単純 clamp の前に簡易 soft clip を入れてもよい。

例:

```cpp
x = x / (1.0f + std::abs(x));
```

その後、

```cpp
int16_t pcm =
    static_cast<int16_t>(
        std::clamp(x, -1.0f, 1.0f) * 32767.0f);
```

---

# 18. リアルタイム生成しない

衝突時に `ImpactSynthRender()` を直接呼ばない。

理由:

- `std::vector` 確保
- CPU 負荷
- 同一音を毎回作る必要がない
- Audio thread に生成処理を入れたくない

推奨:

```text
Startup / Asset Import
        |
        v
ImpactSynthRender
        |
        v
4 - 8 variations
        |
        v
RegisterClip
```

ゲーム中は通常再生だけにする。

---

# 19. Variation 生成

例:

```text
glass_break_0
glass_break_1
glass_break_2
glass_break_3
```

seed だけ変更する。

```cpp
for (int i = 0; i < 4; ++i)
{
    params.seed = BaseSeed + i;

    AudioClip clip;
    ImpactSynthRender(params, clip);

    audio.RegisterClip(
        MakeProceduralAssetId(...),
        std::move(clip),
        name);
}
```

---

# 20. SoundAsset 側

例:

```json
{
  "name": "glass_break",
  "variations": [
    { "clip": 1001, "weight": 1 },
    { "clip": 1002, "weight": 1 },
    { "clip": 1003, "weight": 1 },
    { "clip": 1004, "weight": 1 }
  ],
  "volume": 1.0,
  "volumeRandom": 0.08,
  "pitch": 1.0,
  "pitchRandom": 0.03,
  "bus": "SE",
  "priority": 180,
  "maxInstances": 4,
  "spatialBlend": 1.0,
  "minDistance": 1.0,
  "maxDistance": 35.0,
  "reverbSend": 0.25
}
```

---

# 21. Physics 接続

Audio 側は破壊判定をしない。

Physics / Gameplay 側で判定する。

```text
Bottle Collision
      |
      v
 Normal Impulse
      |
      +--------------------+
      |                    |
 impulse < threshold   impulse >= threshold
      |                    |
      v                    v
 GlassImpact           BreakBottle
                           |
                           v
                       GlassBreak
```

例:

```cpp
void Bottle::OnCollision(const CollisionInfo& hit)
{
    const float strength = hit.normalImpulse;

    if (strength >= breakThreshold)
    {
        BreakBottle();
        PlaySound("glass_break", hit.position, strength);
    }
    else
    {
        PlaySound("glass_impact", hit.position, strength);
    }
}
```

---

# 22. Footstep 接続

```text
Character Controller
        |
        v
 Foot Contact
        |
        v
 Ground Material
        |
        v
 footstep_<material>
```

例:

```cpp
switch (groundMaterial)
{
case Wood:
    PlaySound("footstep_wood");
    break;

case Metal:
    PlaySound("footstep_metal");
    break;

case Concrete:
    PlaySound("footstep_concrete");
    break;
}
```

後で Material ID -> Sound Key のテーブル化を行う。

---

# 23. 3D / Occlusion / Reverb

ImpactSynth は一切担当しない。

既存の再生経路へ渡すことで、

```text
Generated Clip
     |
     v
SoundAsset
     |
     v
AudioSystem
     |
     +--> X3DAudio
     +--> Rolloff
     +--> Doppler
     +--> Mixer
     +--> Reverb
     +--> Acoustic Occlusion
     +--> Diffraction
```

をそのまま利用する。

---

# 24. Thread Policy

ImpactSynth は原則、

```text
Editor
Asset Import
Startup
```

でのみ実行する。

Audio callback / XAudio2 callback 内では実行しない。

ゲーム中は既に登録済み PCM を再生するだけ。

---

# 25. 最小完成条件

## Phase 1

以下だけ完成させる。

```text
GlassImpact
GlassBreak
```

GlassBreak:

```text
Crack
+
6 Resonators
+
24 Shards
```

これを WAV に出して音を確認する。

---

## Phase 2

登録。

```text
ImpactSynth
 -> AudioClip
 -> RegisterClip
 -> SoundAsset
 -> Play
```

---

## Phase 3

Physics 接続。

```text
瓶を投げる
|
+-- 弱い -> GlassImpact
|
+-- 強い -> GlassBreak
```

---

## Phase 4

Footstep。

```text
Wood
Metal
Concrete
Grass
Gravel
```

---

# 26. 想定作業時間

```text
ImpactSynth 骨組み
  0.5 - 1h

GlassImpact
  1 - 2h

GlassBreak
  1.5 - 3h

Variation / RegisterClip
  0.5 - 1h

SoundAsset 接続
  0.5h

Physics 接続
  0.5 - 1.5h

Footstep
  1 - 2h

Material Preset
  1 - 2h

Sound Tuning
  2 - 5h

Debug
  1 - 2h
```

最低限動くところまで:

```text
4 - 7 時間程度
```

実用レベル:

```text
6 - 10 時間程度
```

調整込み:

```text
1 - 2 日
```

---

# 27. 非目標

今回実装しないもの。

```text
Deep-Modal
FEM
Voxel
Mesh modal analysis
Neural network
Realtime procedural source voice
Realtime FFT
Physical fracture acoustics
BEM acoustic radiation
```

必要になった場合のみ後から追加する。

---

# 28. 最終アーキテクチャ

```text
               OFFLINE / STARTUP

        Material + Event + Seed
                  |
                  v
             ImpactSynth
                  |
                  v
              AudioClip
                  |
                  v
          AudioSystem::RegisterClip
                  |
                  v
              SoundAsset
                  |
                  |
================================================
                  |
                  v

                  RUNTIME

           Physics / Gameplay
                  |
                  v
              Sound Key
                  |
                  v
             SoundAsset
                  |
          variation / jitter
                  |
                  v
              PlayDesc
                  |
                  v
          AudioSystem::Play
                  |
       +----------+-----------+
       |          |           |
       v          v           v
     Mixer     X3DAudio   AcousticAudio
                              |
                        Occlusion
                        Diffraction
                        Reverb
```

---

# 29. 設計上の要点

1. **既存 AudioSystem は変更しない**
2. **ImpactSynth は PCM 生成だけ**
3. **生成はリアルタイムではなく事前生成**
4. **SoundAsset の variation を活用する**
5. **3D / Mixer / Reverb / Occlusion は既存機能を使う**
6. **瓶の破壊判定は Physics / Gameplay 側**
7. **最初は GlassImpact / GlassBreak だけ作る**
8. **音の品質は物理精度よりパラメータ調整を優先する**

