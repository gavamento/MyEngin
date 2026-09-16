//====================================================================================
//                          ModalTypes.h
//  MyEngine/ 秋田蓮音                                                      09/16/2026
//                                          Deep-Modal の共有定数と POD 型 (voxel/特徴/ネット)
//====================================================================================
#pragma once
#include <cmath>
#include <cstdint>

namespace mye {

// ---- Deep-Modal 共有定数 ----
// M76c (sub-03) で check_rules.ps1 の $constGroups に C++ ⇄ Python (dataset/layout.py) の
// 4 組として登録する。★1 行 1 定数の形を崩さないこと (正規表現 `constexpr\s+int\s+kXxx\s*=\s*(\d+)`
// で拾う。check_rules.ps1:76 近辺の既存グループと同型)
constexpr int kModalVoxelN = 32;    // ボクセルグリッドの一辺 (32^3 = 32768 voxel)。ネット入力
constexpr int kModalMapN = 16;      // 特徴マップの一辺 (16^3 = 4096 cell)。ネット出力
constexpr int kModalBands = 32;     // Mel 帯域数 (100..10000 Hz を 32 帯域に等分)
constexpr int kModalChannels = 192; // ネット出力チャンネル数 (力軸 3 × (mask 32 + amp 32))
constexpr int kModalVoxelPad = 1;   // flood-fill 用の外周パディング (層数、sub-02 が使う)

// ---- 特徴マップ 1 cell 分 (ネット出力の生値) ----
// ★ここに入る時点で **fp16 → float への変換は済んでいる** (.msfm はディスク上 fp16 だが、
//   ローダ (sub-05) が読み出す際に float へ展開してから BuildModes へ渡す。BuildModes 自身は
//   フォーマット変換をしない — 「特徴の意味」と「ディスク上の圧縮形式」を分離するため)
struct ModalCellFeature {
    float v[kModalChannels] = {};
};

// チャンネル配置 (spec §4.1 BuildModes 手順 1): j = 力軸 (0=x, 1=y, 2=z)、i = 帯域 (0..31)。
// mask はシグモイド前の logit、amp は log-amp 正規化値 (0..1、手順 2 で ln へ戻す)
constexpr int MaskCh(int j, int i)
{
    return j * 64 + i;
}
constexpr int AmpCh(int j, int i)
{
    return j * 64 + 32 + i;
}

// .dmnet ヘッダ (spec §4.2)。**ファイルの物理レイアウトはこの構造体を memcpy しない** —
// ローダ (sub-05) がフィールド単位で読み書きする (Material の暗黙パディングの罠と同じ理由)。
// ここは「値の入れ物」として BuildModes / selftest から使う
struct DmNetHeader {
    uint32_t magic = 0x544E4D44u; // リトルエンディアンで読むと "DMNT" に見える値
    uint32_t version = 1;
    uint32_t opCount = 0;
    uint32_t bufferCount = 0;
    // inN/outN/bands/channels はヘッダに焼き込んだ「そのネットが要求する値」— 実際の推論・
    // BuildModes の反復回数はコンパイル時定数 (kModalVoxelN 等) 側を使う。ロード時の
    // 妥当性検査 (食い違えば拒否) は sub-05 のローダが持つ
    int32_t inN = kModalVoxelN;
    int32_t outN = kModalMapN;
    int32_t bands = kModalBands;
    int32_t channels = kModalChannels;
    float fMinHz = 100.0f;
    float fMaxHz = 10000.0f;
    float logAmpMin = 0.0f; // 手順 2: ln(amp) の正規化下限
    float logAmpMax = 0.0f; // 同上限 (下限と同値のままだと amp が常に定数になるので、
                             // 実データは export.py が統計から決めた非退化値を書く)
    float ampScale = 1.0f;
    float maskThreshold = 0.5f; // ModalSound.maskThreshold ≤ 0 のときに使う既定しきい値
    // ---- 参照材質 (spec §4.1「参照材質」。アルミ相当)。この構造体の既定値は
    // .dmnet を読む前の in-memory 初期値に過ぎず、実行時は必ずロード済みヘッダの値
    // (DmNet.cpp の DmNetHeader::Load) で上書きされる。値そのものの正本は
    // tools/deepmodal/layout.py (REF_YOUNG 等) — refSizeL は M76h (sub-08) で
    // 0.3->0.6 へ改訂した (layout.py のコメント参照。帯域内モードが増える方向の
    // 実測込みの判断) ----
    float refYoung = 7.0e10f;
    float refDensity = 2700.0f;
    float refPoisson = 0.33f; // FEM 参照用メタデータ。BuildModes は読まない (spec §2 #4)
    float refSizeL = 0.6f;
    float refAlpha = 6.0f;
    float refBeta = 1.0e-7f;
    float bandCenterHz[kModalBands] = {}; // MelBandCenters() で埋める (32 帯域の中心 Hz)
    uint64_t weightsHash = 0; // FNV-1a (export.py が計算、sub-04)
    uint32_t paramCount = 0;
    uint32_t reserved = 0;
};

// Mel(100Hz..10000Hz) を kModalBands 帯域に等分し、各帯域の中心 (区間中点の逆変換) を返す。
// spec §4.1「Mel」: mel(f) = 2595*log10(1+f/700)、mel(100)..mel(10000) を 33 点等分、
// 中心は区間中点の逆変換。C++ と Python (sub-03 の compact.py) の二重実装なので、
// selftest がここを別経路の double 計算と 1e-2 Hz で照合してドリフトを検知する。
// ★実行時はこの関数の値ではなく **ヘッダの bandCenterHz[32] を使う** (Python が焼いた値の
//   正本性を保つため。この関数はヘッダを作る側 (sub-03/sub-04) と selftest 用)
inline void MelBandCenters(float out[kModalBands])
{
    auto melOf = [](double f) { return 2595.0 * std::log10(1.0 + f / 700.0); };
    auto invMel = [](double m) { return 700.0 * (std::pow(10.0, m / 2595.0) - 1.0); };
    const double melMin = melOf(100.0);
    const double melMax = melOf(10000.0);
    for (int i = 0; i < kModalBands; ++i) {
        const double m0 = melMin + (melMax - melMin) * (static_cast<double>(i) / kModalBands);
        const double m1 = melMin + (melMax - melMin) * (static_cast<double>(i + 1) / kModalBands);
        out[i] = static_cast<float>(invMel(0.5 * (m0 + m1)));
    }
}

} // namespace mye
