//====================================================================================
//                          EngineIndexDump.h
//  MyEngin/ 秋田蓮音                                                       10/15/2026
//                                          エンジン索引のダンプ (--dump-index。組み込み型・フィールド・CLI 表を JSON へ)
//====================================================================================
#pragma once
#include <string>

namespace mye {

// 組み込みコンポーネント・フィールド・共通 CLI 表・showcase 表を JSON 文字列にする。
// 同じバイナリなら何度呼んでもバイト同一 (時刻・パス・PID を含めない)。D3D もウィンドウも作らない
std::string BuildEngineIndexDumpJson();

// --dump-index <outPath> の実行。UTF-8 (BOM なし) で書いて終了コードを返す (0 = 書いた / 1 = 失敗)
int RunDumpIndexCli(const std::wstring& outPath);

} // namespace mye
