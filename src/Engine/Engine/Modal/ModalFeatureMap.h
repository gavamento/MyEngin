//====================================================================================
//                          ModalFeatureMap.h
//  MyEngine/ 秋田蓮音                                                      09/16/2026
//                                          1 メッシュ分の推論結果 (.msfm の中身) と焼き手順
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Engine/Engine/Modal/ModalInferenceBackend.h"

namespace mye {

// .msfm (cooked cache、.mcvx と同型) の内部版。ファイル形式を変えたらここを上げる —
// kCookVersion (Asset/CookedCache.h) は据え置きでよい (spec §4.2: 「内部版を持つので
// kCookVersion は据え置き」)
constexpr uint32_t kMsfmVersion = 1;

// 1 メッシュ分の推論結果 (spec §4.2)。有効 cell だけを詰めて持つ (典型 200-500 KB)。
// ★cellSlot は Voxelizer::BuildCellSlotTable と全く同じ意味 (自身が有効ならそのまま、
//   無効なら最寄りの有効 cell の**生の cell index**)。feat 配列内の格納順とは別物なので、
//   feat 上の行を引くには RowOf() を経由すること (feat は cell index 昇順に詰めてあるだけで、
//   無効 cell 自身のスロットは持たない)
struct ModalFeatureMap {
    uint32_t version = kMsfmVersion;
    uint64_t modelHash = 0; // 焼いたときの .dmnet の weightsHash (ロード側が modelHash 不一致をミス扱いする)
    modal::VoxelFrame frame;
    uint32_t validCount = 0;
    uint16_t cellSlot[4096] = {};
    std::vector<uint16_t> feat; // validCount * kModalChannels 個の fp16 (raw bit pattern)

    // rawCell (0..4095、modal::CellIndexOf と同じ添字) → feat 内の行番号 (0..validCount-1)。
    // cellSlot 経由で最寄りの有効 cell へ丸めてから、feat の格納順 (cell index 昇順) での
    // 順位を数える。呼び出し頻度は衝突 1 件につき高々 1 回 (kMaxModalShotsPerTick=4/tick) なので
    // O(4096) の線形カウントで十分 (sub-06 の接触 → 音の差し込みが呼ぶ想定)。
    // validCount==0 または rawCell が範囲外なら -1
    int RowOf(int rawCell) const;

    // RowOf() の行を fp16 → float へ展開して取り出す。行が無ければ false (out は変更しない)
    bool CellFeature(int rawCell, ModalCellFeature& out) const;
};

// backend.Infer() を呼び、有効 cell (占有ボクセルを 1 個以上含む cell。
// export.py の _cell_valid_from_occupancy と同じ規則 = Voxelizer::BuildCellSlotTable の
// 「自身にマップされる」cell) だけを抽出して fp16 へ詰める。
// out.modelHash は net.header.weightsHash を書く。false は backend.Infer() の失敗をそのまま返す
bool BuildFeatureMap(ModalInferenceBackend& backend, const DmNet& net, const modal::VoxelGrid& grid,
                    ModalFeatureMap& out, std::string* err);

// .msfm の表 ⇄ blob (ConvexColliderLibrary の .mcvx と同型。selftest から直接叩けるように公開する)
void SerializeModalTable(const std::vector<std::pair<std::string, ModalFeatureMap>>& table,
                         std::vector<uint8_t>& out);
bool DeserializeModalTable(const std::vector<uint8_t>& in,
                           std::vector<std::pair<std::string, ModalFeatureMap>>& out);

} // namespace mye
