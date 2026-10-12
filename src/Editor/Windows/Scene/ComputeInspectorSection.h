//====================================================================================
//                          ComputeInspectorSection.h
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          スクリプトのインスペクターの Compute 欄 (ImGui の描画)
//====================================================================================
#pragma once
#include <string>

#include "Engine/Engine/Compute/ComputeInspectorRows.h"

namespace mye {

struct EngineContext;

// Compute 欄の 1 行ぶんの文字列 (selftest が表示の取り違えを検出できるよう、描画から切り出してある)
const char* ComputeRowStateText(compute::RowState state);
const char* ComputeRowTargetText(compute::RowTarget target);
const char* ComputeRowVersionText(compute::RowVersion version, compute::CallerKind lane);

// 行の状態の補足 (ツールチップ)。宣言なし・ファイル無し・検査違反では直し方と規則、失敗の理由を返す。無ければ空
std::string ComputeRowDetail(const compute::ComputeInspectorRow& row, compute::CallerKind lane);

// scriptType は ComponentDesc の名前 (C++ は REGISTER_SCRIPT の型名、C# は Type.FullName)。
// 宣言も観測も無いスクリプトでは何も描かない
void DrawComputeInspectorSection(EngineContext& ctx, const char* scriptType, compute::CallerKind lane);

} // namespace mye
