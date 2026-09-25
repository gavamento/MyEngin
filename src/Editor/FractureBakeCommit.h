//====================================================================================
//                          FractureBakeCommit.h
//  MyEngin/ 秋田蓮音                                                     09/25/2026
//                                          焼き成功結果の確定 (.mfrac 保存・登録・Undo、M80i)
//====================================================================================
#pragma once
#include <cstdint>

#include "Engine/Core/EntityID.h"
#include "Editor/FractureBakeService.h"

namespace mye {

struct EngineContext;
struct Selection;
class UndoStack;

// BakeFracture が成功させた結果 (request/result とも result.success==true が前提。失敗/拒否は
// 呼ばない — 理由表示は呼び出し側の仕事) を確定させる:
//   1. assets\Fracture\<root の名前>_<seed>_<pieceCount>.mfrac へ書き出す (同名は上書き)
//   2. AssetDatabase の .meta を確定させてから FractureLibrary へ登録する
//      (先に確定させないと登録名が path-hash に落ち、ファイル移動だけで参照が壊れる)
//   3. root の Destructible.fractureAsset を書き換え、BuildFracturePieces で子を組み直す —
//      ここまでを 1 Undo エントリにまとめる (spec §4.3「子の組み直しと欄の変更は 1 記録」)
// 戻り値 false は書き出し/登録に失敗した (ディスク書き込み不可、root が既に破棄された等)。
// root に Destructible が無い (焼き待ちの間に外された) 場合も false
bool CommitFractureBake(EngineContext& ctx, Selection& selection, UndoStack& undo, EntityID root,
                        uint64_t fid, const FractureBakeRequest& request,
                        const FractureBakeResult& result);

} // namespace mye
