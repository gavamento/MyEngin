//====================================================================================
//                          TrackProjectId.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          projectId と pipe 名の計算 (宣言)
//====================================================================================
#pragma once
#include <string>

namespace mye {

// projectId のもとになる文字列。NormalizePathKey の結果から末尾の '\' を全部落とす
// (ドライブのルート "c:\" は落とさない)。tools\track\src\project.rs の project_id_source と同じ規則で、
// 固定表 tools\track\tests\project_id_cases.tsv が両者の一致を保つ
std::string ProjectIdSource(const std::wstring& projectRoot);

// ProjectIdSource の UTF-8 の SHA-256 の先頭 16 桁。pipe 名 `\\.\pipe\mye-track-<projectId>` に使う。
// SHA-256 を使えない環境では空
std::string ProjectId(const std::wstring& projectRoot);

// `\\.\pipe\mye-track-<projectId>`。projectId が空なら空
std::string TrackPipeName(const std::string& projectId);

} // namespace mye
