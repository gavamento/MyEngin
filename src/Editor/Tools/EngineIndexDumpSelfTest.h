//====================================================================================
//                          EngineIndexDumpSelfTest.h
//  MyEngin/ 秋田蓮音                                                       10/15/2026
//                                          エンジン索引ダンプの回帰テスト
//====================================================================================
#pragma once

namespace mye {

// BuildEngineIndexDumpJson の件数・既知フィールド・決定性 (2 回で同一)・JSON として読めること
bool RunEngineIndexDumpSelfTest();

} // namespace mye
