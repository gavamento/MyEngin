//====================================================================================
//                          NavLinkGen.h
//  MyEngin/ 秋田蓮音                                                     10/05/2026
//                                          ベイク時の Off-Mesh Link の自動生成 (飛び降り・飛び越え)
//====================================================================================
#pragma once

#include <atomic>
#include <vector>

#include "Engine/Engine/Navigation/NavBakeInput.h"
#include "Engine/Engine/Navigation/NavTileCacheSupport.h"

namespace mye {

// 1 回のベイクで作る Link の上限。超えた分は作らない (タイルのポリゴン数と .mnav の大きさの歯止め)
inline constexpr int kNavMaxGeneratedLinks = 4096;

// 層から組んだナビメッシュ (Off-Mesh Link を含まないこと) の外周の辺を一定間隔で調べ、Link を作る (M84e、Unity の Generate Links)。
//   飛び降り: 辺の外側の下 (maxClimb より低く linkDropHeight 以内) に歩行面がある。一方通行
//   飛び越え: 辺の外側の水平 linkJumpDistance (縁から縁の隙間) 以内に、高さの差が maxClimb 以内の歩行面がある。双方向
// 途中が soup の三角形にぶつかる候補と、ナビメッシュ上を歩いても近い (Link の長さの 2 倍以内) 候補は捨て、
// 近い候補はまとめる。out は key 昇順 (kNavGeneratedLinkKeyBit | 通し番号)。同じ入力から同じ列になり、World に触れない
// (ワーカースレッドから呼べる)。cancel が立ったら false
bool NavGenerateLinks(const NavBakeConfig& config, const NavTriangleSoup& soup, const dtNavMesh& nav,
                      const std::atomic<bool>* cancel, std::vector<NavLinkSpec>& out);

} // namespace mye
