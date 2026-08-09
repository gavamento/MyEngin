#pragma once
#include <cstdint>
#include <string>

#include "Engine/Engine/GameFlow.h"

namespace mye {

// ランタイムセーブ (M51g、決定台帳 5)。ファイルは PersistStore + 現シーンパスの JSON。
// 書出は tick 末ハッシュ後の出力レーン (決定論を汚さない)。読込は LoadScene と同じ
// セーフポイントで EngineLoop が消費し、record/verify 中は no-op + WARN
// (「リプレイはセーブ読込を跨がない」を仕様とする)。
struct SaveGameData {
    std::wstring scenePath; // assets 相対 (assets 外は絶対)。空 = シーン切替なしのセーブ
    PersistStore::Map persist;
};

namespace SaveGameFile {

// <saveDir>\slot<N>.json。saveDir の二経路はプロジェクト起動 = <project>\save、
// レガシー起動/配布 = <exeDir>\save (cache\cooked と同じ規則、分岐は projectRoot で判定)
std::wstring PathForSlot(const std::wstring& saveDir, int slot);

// 出力は決定論 (persist はキー昇順、キーは 16 桁 hex なので JSON の辞書順 = 数値順)
bool Write(const std::wstring& path, const std::wstring& scenePath, const PersistStore& persist);

// 破損 (JSON 不正 / version 不一致 / hex 不正) は全体を失敗にする — 部分読込で
// 中途半端な永続状態を作らない (M51b の「検算してから信用する」と同じ方針)
bool Read(const std::wstring& path, SaveGameData& out);

} // namespace SaveGameFile

} // namespace mye
