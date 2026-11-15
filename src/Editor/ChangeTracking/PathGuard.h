//====================================================================================
//                          PathGuard.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          復元の書き先の検査 (履歴のパスがプロジェクトの外・保護領域を指さないか)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>

#include "Editor/ChangeTracking/RestoreFileIo.h"

namespace mye {

// 検査の結果。None 以外は、そのパスへ書かない (プレビューでも拒否として出す)。
// 前例: tools\mcp\src\notes.rs (notes の書き先の検査) と、そのシナリオ (tests\mcp\notes_*.ndjson)
enum class PathIssue : uint8_t {
    None,
    Empty,              // 空
    Absolute,           // ドライブ名・UNC・先頭の区切り (履歴のパスはルートからの相対)
    Traversal,          // "." / ".." の要素
    Separator,          // バックスラッシュ・空の要素 (// や末尾の /)。履歴のパスの区切りは / だけ
    InvalidChar,        // 制御文字と <>"|?*
    Stream,             // ":" を含む (代替データストリーム)
    ReservedDevice,     // CON / PRN / AUX / NUL / COM1-9 / LPT1-9 (拡張子つきも)
    TrailingDotOrSpace, // 要素の末尾が空白かピリオド (Win32 が黙って削る)
    ProtectedDirectory, // .git / .mye の要素 (8.3 の短い名前の形も含む)
    OutsideRoot,        // ルートの下に収まらない / ルートが無い
    Link,               // 書き先か親ディレクトリがシンボリックリンク・ジャンクション (再解析ポイント)
    ParentNotDirectory, // 親がファイルで、ディレクトリを作れない
};

// 純粋な部分: 相対パス (UTF-8、/ 区切り) の綴りだけを見る。ファイルシステムは見ない
PathIssue CheckRelativePath(const std::string& rel);

// ファイルシステムを見る部分: ルートの下の各要素 (ルート自身は除く) を io の属性で調べる。
// 存在しない要素から先は調べない (これから作る場所)。CheckRelativePath が None のパスにだけ使う
PathIssue CheckPathOnDisk(const std::wstring& projectRoot, const std::string& rel, RestoreFileIo& io);

// 両方 (プレビューのときと、書く直前の 2 回使う口)
PathIssue CheckRestorePath(const std::wstring& projectRoot, const std::string& rel, RestoreFileIo& io);

// ルートと相対パスを結んだ絶対パス (区切りは \)。CheckRelativePath が None のときだけ意味がある
std::wstring JoinProjectPath(const std::wstring& projectRoot, const std::string& rel);

// 絶対パス → ルートからの相対 (/ 区切り、UTF-8)。ルートの下でなければ空 (大文字小文字は区別しない)
std::string RelativeToProject(const std::wstring& projectRoot, const std::wstring& absPath);

// パスの比較用の鍵 (大文字小文字を区別せず、区切りを揃える)。Windows のパスは大小を区別しない
std::string RestorePathKey(const std::string& rel);

} // namespace mye
