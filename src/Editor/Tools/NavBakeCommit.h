//====================================================================================
//                          NavBakeCommit.h
//  MyEngin/ 秋田蓮音                                                     10/03/2026
//                                          ベイク結果の確定 (.mnav 保存・GUID 参照・Undo、M82b)
//====================================================================================
#pragma once

#include <cstdint>
#include <string>

#include "Engine/Core/Ecs/EntityID.h"
#include "Engine/Engine/Navigation/NavBake.h"

namespace mye {

struct EngineContext;
struct Selection;
class UndoStack;

// .mnav の保存先: <assetsRoot>\NavMesh\<Surface の名前>_<入力ハッシュ 16 桁>.mnav。
// 入力が同じなら同じファイルを指す
std::wstring NavBakeAssetPath(const std::wstring& assetsRoot, const std::string& surfaceName, uint64_t inputHash);

// status が Ok のベイク結果を確定する: .mnav を書き、.meta を確定させて GUID を得て、
// surface のグループ (同じ agentTypeId の Surface、M84b) の全 Surface の navAsset を書き換える (参照の設定は 1 Undo)。
// ファイル名は Agent Type の名前 (表に無ければ Surface の名前)。書き込み失敗・Surface が無い場合は false
bool CommitNavBake(EngineContext& ctx, Selection& selection, UndoStack& undo, EntityID surface, uint64_t fid,
                   const NavBakeOutput& output);

// surface のグループの全 Surface の navAsset を外す (1 Undo)。.mnav のファイルは消さない。参照が 1 つも無ければ false
bool ClearNavBake(EngineContext& ctx, Selection& selection, UndoStack& undo, EntityID surface, uint64_t fid);

} // namespace mye
