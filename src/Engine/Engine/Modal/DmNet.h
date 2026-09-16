//====================================================================================
//                          DmNet.h
//  MyEngine/ 秋田蓮音                                                      09/16/2026
//                                          .dmnet (学習済みネット) のローダと op 表
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Engine/Engine/Modal/ModalTypes.h"

namespace mye {

// op 種別 (spec §4.2 / tools/deepmodal/layout.py の OP_* と同値)
constexpr int32_t kDmNetOpConv3d = 0;
constexpr int32_t kDmNetOpConvTranspose3d = 1;
constexpr int32_t kDmNetOpRelu = 2;
constexpr int32_t kDmNetOpAdd = 3;

// weightOffset/biasOffset の「無し」センチネル (ReLU/Add はどちらも持たない)。
// offset=0 はヘッダ直後に置かれる最初の重みの正当な位置なので「無し」に使えない
// (tools/deepmodal/layout.py の DMNET_OFFSET_NONE と同値)
constexpr uint32_t kDmNetOffsetNone = 0xFFFFFFFFu;

// 256 B ヘッダに続く op 表 1 エントリのバイト数 (12 フィールド × 4 B、spec §4.2)
constexpr size_t kDmNetHeaderBytes = 256;
constexpr size_t kDmNetOpBytes = 48;

// export.py (tools/deepmodal/model.py の MAX_PARAM_COUNT) と同値の予算。
// ★check_rules.ps1 の $constGroups には乗せていない (spec §4.2 が登録を求める 4 組
//   (kModalVoxelN/kModalMapN/kModalBands/kModalChannels) にこの定数は入っていない) —
//   両側で書き換えたら手で揃えること。ドリフトすれば LoadDmNet が黒っぽく拒否するだけ
//   (安全側に壊れる) なので影響は限定的
constexpr uint32_t kDmNetMaxParamCount = 2'000'000u;

// .dmnet の op 表 1 エントリ (spec §4.2)。weightOffset/biasOffset は
// **ファイル先頭からの絶対バイトオフセット** (export.py の書式が正本)
struct DmNetOp {
    int32_t type = kDmNetOpRelu;
    int32_t in0 = -1;
    int32_t in1 = -1;
    int32_t out = 0;
    int32_t cin = 0;
    int32_t cout = 0;
    int32_t k = 0;
    int32_t stride = 1;
    int32_t pad = 0;
    int32_t outPad = 0;
    uint32_t weightOffset = kDmNetOffsetNone;
    uint32_t biasOffset = kDmNetOffsetNone;
};

// 読み込み済み .dmnet の全体。重み/バイアスの実体は複製せず、op の
// weightOffset/biasOffset で bytes を直接指す (フルサイズのネットは重みだけで
// 数 MB あるので、Prepare のたびに複製しない)
struct DmNet {
    DmNetHeader header;
    std::vector<DmNetOp> ops;
    std::vector<uint8_t> bytes; // ファイル全体 (ヘッダ含む)
};

// path から .dmnet を読む。**フィールド単位で読み** (struct を memcpy しない。
// Material の暗黙パディングの罠と同じ理由)、次を全て満たさないと false + *err:
//   - magic/version が既知の値
//   - ファイルサイズが「ヘッダ + op 表 + 実際に参照される重み/バイアス」を包含する
//   - paramCount が予算 (kDmNetMaxParamCount) 以下
//   - 重み+バイアス blob の FNV-1a 64bit が header.weightsHash と一致 (破損検知)
bool LoadDmNet(const std::wstring& path, DmNet& out, std::string* err);

// net.bytes 上の op の重み (fp16 → float 展開済み) とバイアス (fp32 のまま) を取り出す。
// weightOffset/biasOffset が無い (ReLU/Add) / 範囲外は false
bool DmNetOpWeightFloat(const DmNet& net, const DmNetOp& op, std::vector<float>& weightOut,
                        std::vector<float>& biasOut, std::string* err);

// FNV-1a 64bit (export.py の fnv1a64 と同一定数)。selftest からも直接叩けるように公開する
uint64_t DmNetFnv1a64(const uint8_t* data, size_t size);

} // namespace mye
