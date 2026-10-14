//====================================================================================
//                          EosOnlineService.h
//  MyEngin/ 秋田蓮音                                                     10/14/2026
//                                          Epic Online Services によるオンラインサービス
//====================================================================================
#pragma once
#include <memory>

#include "Engine/Engine/Online/IOnlineService.h"

namespace mye {

// EOS の実装を作る。SDK を入れずにビルドした (MYE_HAS_EOS が無い) ときは nullptr。
// ★SDK の型はこのヘッダから出さない (規則 13-d: EOS の名前は Online\Eos\ の .cpp に閉じる)
std::unique_ptr<IOnlineService> CreateEosOnlineService();

// この実行ファイルが EOS 付きでビルドされたか
bool EosAvailableInThisBuild();

} // namespace mye
