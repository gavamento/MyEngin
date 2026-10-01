//====================================================================================
//                          TickInputs.h
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          確定入力 (レーン入力 + システム入力) の ctx への置換 (EngineLoop / HeadlessSim 共通)
//====================================================================================
#pragma once
#include <cstdint>

#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Engine/Replay/Replay.h"
#include "Engine/Engine/Session/SessionTypes.h"

namespace mye {

// 確定入力 (レーン入力 + システム入力) を ctx へ置換する唯一の場所。
// EngineLoop (verify / ネット) と HeadlessSim (verify / サーバ・クライアントの確定 tick) が
// 同じ関数を呼ぶ。置換の規則が 2 か所に分かれると、サーバとクライアントで消費する列が食い違う。
// lanes は ctx.playerCount 本、sys はシステム入力を持つ構成 (ctx.hasSystemInput) だけが読む。
// sys が null のとき (イベント無しの tick) はゼロ値を置く
inline void ApplyConfirmedInputs(EngineContext& ctx, const InputSnapshot* lanes,
                                 const SystemInputTick* sys)
{
    for (uint32_t p = 0; p < ctx.playerCount; ++p) {
        ctx.inputs[p] = lanes[p];
    }
    if (ctx.hasSystemInput) {
        ctx.systemInput = (sys != nullptr) ? *sys : SystemInputTick{};
    }
}

// .rep の ctx.tickIndex 番目の記録で置換する (verify)
inline void ApplyReplayInputs(EngineContext& ctx, const ReplayPlayer& player)
{
    InputSnapshot lanes[kMaxPlayers] = {};
    for (uint32_t p = 0; p < ctx.playerCount; ++p) {
        lanes[p] = player.InputForTick(ctx.tickIndex, p);
    }
    const SystemInputTick* sys =
        ctx.hasSystemInput ? &player.SystemInputForTick(ctx.tickIndex) : nullptr;
    ApplyConfirmedInputs(ctx, lanes, sys);
}

} // namespace mye
