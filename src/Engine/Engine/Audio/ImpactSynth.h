//====================================================================================
//                          ImpactSynth.h
//  MyEngine/ 秋田蓮音                                                      09/12/2026
//                                          衝突音・足音・ガラス破壊音の手続き生成 (純関数)
//====================================================================================
#pragma once
#include <cstdint>
#include <string_view>

#include "Engine/Engine/Audio/AudioClip.h"

namespace mye {

// 足音 / 物体衝突音 / ガラス瓶の衝突音・破壊音を PCM として手続き生成する
// (計画 plans/ImpactSoundDesign.md)。SynthCore と同じ契約の**純関数** —
// I/O もデバイスも ECS も踏まない。乱数は spec 11.2 規則 8 に従い Pcg32 のみ。
//
// ★ここは **PCM を作るだけ**。再生制御 (variation / 揺らぎ / 3D / 遮蔽 / 残響) は
//   既存の SoundAsset → AudioSystem::Play がそのまま担当する (計画 §2 / §23)。
// ★**衝突のたびに呼ばない** (計画 §18)。起動時 / アセット読込時に数本の variation を
//   生成して RegisterClip し、ゲーム中は登録済み PCM を鳴らすだけ。その糊は
//   ImpactSoundAsset.* (.impact.json) が持つ。
// ★Deep-Modal / FEM / ニューラル / リアルタイム FFT はやらない (計画 §27)。
//   Transient (短いノイズ) + Resonator Bank (減衰正弦 6 モード) + Shard Events
//   (破片の短い減衰正弦) の 3 要素だけで、物理精度より聴感を優先して値を詰める。

enum class ImpactSoundKind : int32_t {
    Footstep = 0,   // 足音。materialB = 地面の材質が主成分
    Impact = 1,     // 物体衝突 (割れない瓶 / 石 / 一般)。materialA が主体、materialB は補正
    GlassBreak = 2, // ガラス瓶の破壊。Crack + 瓶の共鳴 + 破片
};

enum class AcousticMaterial : int32_t {
    Wood = 0,
    Metal = 1,
    Concrete = 2,
    Glass = 3,
    Plastic = 4,
    Grass = 5,
    Gravel = 6,
};
inline constexpr int32_t kAcousticMaterialCount = 7;

struct ImpactSynthParams {
    ImpactSoundKind kind = ImpactSoundKind::Impact;
    AcousticMaterial materialA = AcousticMaterial::Wood;     // 主体 (Footstep では未使用)
    AcousticMaterial materialB = AcousticMaterial::Concrete; // 相手 (Footstep では地面)
    float strength = 1.0f;   // 衝突強度。音量 / transient 量 / 高域 / 破片数に効く (計画 §11)
    float size = 1.0f;       // 物体サイズ。共鳴周波数を 1/size に (計画 §10。0.25..4 にクランプ)
    // 高域の倍率 (0.25..2 にクランプ)。材質プリセットの brightness に掛かり、transient の LPF /
    // 高次モードの量 / 破片の周波数帯を一緒に動かす。1 = プリセットのまま。
    // ★「全体的に高い / こもる」を .impact.json 1 行で直すための耳用の摘み。size は共鳴の
    //   **音程**を、これは**明るさ**を動かす (別の軸)
    float brightness = 1.0f;
    float durationSec = 0.4f;
    uint32_t sampleRate = 44100;
    uint64_t seed = 1;       // 同じ seed なら同じ PCM。variation は seed を変えるだけ (計画 §19)
};

// 材質プリセット (計画 §9)。値は物理精度より聴感調整を優先する
struct ImpactMaterialPreset {
    float baseFrequency;   // 第 1 モードの周波数 [Hz] (size 1.0 のとき)
    float resonanceDecay;  // 共鳴の減衰 [1/s]。大きいほど短く鳴る
    float resonanceAmount; // 共鳴成分の量 0..1 (Concrete / Grass は弱い)
    float noiseAmount;     // transient (ノイズ) の量 0..1
    float brightness;      // 高域の量 0..1 (LPF の開きと高次モードの振幅)
    float shardAmount;     // GlassBreak の破片量の係数 (Glass = 1)
    float transientDecay;  // transient の減衰 [1/s]
    // 低域の胴鳴り ("ドン"。70-110 Hz の短い減衰正弦) の量 0..1。
    // ★transient + 共鳴だけだと硬い床は「チッ」になり全体が高く聞こえる。足や石の質量が
    //   床を押す低い成分を別枝で足す。柔らかい床 (Grass) ほど多く、Glass はほぼ無し
    float bodyAmount;
};
const ImpactMaterialPreset& MaterialPreset(AcousticMaterial m);

// 名前 ⇄ 列挙 (.impact.json と selftest が使う)。未知の名前は false
const char* ImpactSoundKindName(ImpactSoundKind k);
const char* AcousticMaterialName(AcousticMaterial m);
bool ParseImpactSoundKind(std::string_view s, ImpactSoundKind& out);
bool ParseAcousticMaterial(std::string_view s, AcousticMaterial& out);

// params から PCM を合成する。**同じ入力なら同じ PCM** (ビット単位)。
// 出力はモノラル 16bit、長さ = durationSec * sampleRate フレーム。ピークは約 -1 dBFS に
// 正規化される (計画 §16-17。後段の SoundAsset.volume で下げる前提)
void ImpactSynthRender(const ImpactSynthParams& params, AudioClip& out);

} // namespace mye
