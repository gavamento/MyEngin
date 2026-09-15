//====================================================================================
//                          ModalSynth.h
//  MyEngine/ 秋田蓮音                                                      09/16/2026
//                                          学習済み特徴 → モード集合 → PCM (Deep-Modal 合成器)
//====================================================================================
#pragma once
#include <cstdint>

#include "Engine/Engine/Audio/AudioClip.h"
#include "Engine/Engine/Modal/ModalTypes.h"

namespace mye {

// Deep-Modal (ACM MM 2020, Jin et al.) の後処理 + 合成を担う**純関数**の対。
// I/O もデバイスも World も ECS も踏まない — 呼び出し側 (sub-06 の CollectModalImpacts /
// Inspector プレビュー / selftest) がどれも同じ経路を通ることを保証する。
//
// ★ここは「特徴 + 力 + 材質 → PCM」の 2 段だけ。ネット推論 (.dmnet → ModalCellFeature) は
//   ModalInferenceBackend (sub-05) が別に持つ。衝突からの差し込み・再生制御 (回転プール /
//   3D / 遮蔽 / リバーブ) は AudioSourceSystem (sub-06) が既存の Play 経路へ渡す。

// 1 発の衝突音を表す「モード集合」。BuildModes の出力であり ModalSynthRender の入力。
// count ≤ kModalBands (1 帯域につき生存できるモードは最大 1 本、spec §4.1 手順 7)
struct ModalModeSet {
    int32_t count = 0;
    float freqHz[kModalBands] = {};
    float amp[kModalBands] = {};
    float decay[kModalBands] = {}; // 指数減衰率 [1/s]
};

// BuildModes に渡す材質・サイズパラメータ。World も PhysMat も直接渡さない —
// 呼び出し側が PhysMat から抜き出した「値」だけを渡すことで、この関数をデバイスも
// ワールドも無しの純関数のまま selftest から直接叩ける (sub-01 の設計方針)
struct ModalPostParams {
    float young = 0.0f;         // Pa。0 = 参照材質のまま (σ1 = 1、spec §4.1 手順 6)
    float density = 1000.0f;    // kg/m^3
    float sizeL = 0.3f;         // m (対象の代表長さ、AABB の最長辺相当)
    float alpha = 0.0f;         // Rayleigh 減衰の質量項 [1/s]
    float beta = 0.0f;          // Rayleigh 減衰の剛性項 [s]
    float maskThreshold = 0.0f; // ≤0 はヘッダ既定 (hdr.maskThreshold) を使う
    float gain = 1.0f;
};

// PCM の可聴下限 (1/32768 = int16 の 1 LSB 相当)。これを下回るモードは鳴らさず、
// クリップ長 (ModalClipSeconds) もこれを基準に「聞こえなくなるまで」を切る
constexpr float kModalTailAmp = 1.0f / 32768.0f;

// ModalSynthRender の固定サンプルレート (mono/int16 と同じく spec §4.1 で固定)
constexpr int kModalSampleRate = 44100;

// 特徴マップ 1 cell + 接触の力ベクトル (ローカル軸) + 材質パラメータ → 鳴らすモード集合。
// spec §4.1 の手順 1-7 を**この順で 1 関数に** (順序を変えると聴感が変わるだけでなく、
// スケール則の相対誤差検査 (selftest) が壊れる)。純関数、乱数不使用
void BuildModes(const ModalCellFeature& feature, const float k[3], const DmNetHeader& hdr,
                const ModalPostParams& post, ModalModeSet& out);

// モード集合から合成に必要な長さ [s] を決める。
// T = clamp(max_i ln(a_i / kModalTailAmp) / c_i, 0.05, 2.0)
float ModalClipSeconds(const ModalModeSet& modes);

// モード集合 → PCM (mono/44100/int16)。2 次再帰共振器 (double 状態、位相 0) を
// kModalBands 本まで加算し、1ms の線形アタックと tanh ソフトクリップ (|x| > 0.8 のみ) を
// 経て量子化する。**同じ入力は常にビット単位で同じ結果になる** (乱数不使用)
void ModalSynthRender(const ModalModeSet& modes, AudioClip& out);

} // namespace mye
