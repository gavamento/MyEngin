//====================================================================================
//                          ProjectSettingsFileSelfTest.h
//  MyEngin/ 秋田蓮音                                                       10/06/2026
//                                          project_settings.json の読み書きの回帰テスト
//====================================================================================
#pragma once

namespace mye {

// 保存が他の機能のキーを消さないこと、壊れたファイルを上書きしないこと、改行コードを保つことを固定する
bool RunProjectSettingsFileSelfTest();

} // namespace mye
