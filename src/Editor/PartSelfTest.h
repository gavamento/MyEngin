#pragma once

namespace mye {

// 部位 (ソケット) の回帰テスト (M48f)。
// Engine 層の `Parts::FindPart` / `FindPartsByTag` / `IsStructureLocked` と、
// エディタ層のタグ名テーブル (`PartTagNames`) の往復をまとめて検証する。
// **Editor 層に置いてある**のは PartTagNames が Editor 層だから (UndoSelfTest と同じ理由)
bool RunPartSelfTest();

} // namespace mye
