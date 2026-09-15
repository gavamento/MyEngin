//====================================================================================
//                          TriangleSoup.h
//  MyEngine/ 秋田蓮音                                                      09/16/2026
//                                          OFF/OBJ の最小テキストリーダ (Deep-Modal ボクセル化の入力)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <DirectXMath.h>

namespace mye {
namespace modal {

// ボクセライザに渡す最小限の三角形メッシュ (位置 + インデックスのみ、法線/UV は不要)。
// FBX / glTF は既存ローダのヘッドレス登録経路 (RegisterAssets) から Mesh::positions/indices を
// そのまま使うのでここは通らない — 通るのは ModelNet 等の OFF / 手書き OBJ だけ
struct TriangleSoup {
    std::vector<DirectX::XMFLOAT3> positions;
    std::vector<uint32_t> indices; // 3 の倍数、CCW/CW は問わない (TriBoxOverlap は向きを見ない)
};

// OFF (Object File Format) をテキストから読む。ModelNet の配布ファイルは
// ヘッダの "OFF" 直後に空白なしで先頭の数字が続くことがある (例 "OFF8 12 0") ため、
// その形も受理する。面の頂点数が 3 でなければ先頭頂点からのファン三角形分割にする。
// 面の後ろの色情報等の追加トークンは前提としない (ModelNet はジオメトリのみ)
bool LoadOffText(const std::string& text, TriangleSoup& out, std::string* error = nullptr);

// OBJ の "v"（頂点）と "f"（面）だけを読む最小リーダ。"vt"/"vn"/"o"/"g"/"mtllib" 等は無視する。
// 面の頂点参照は "i" 単体、"i/j" (位置/uv)、"i/j/k" (位置/uv/法線) のいずれも先頭成分だけを使う。
// 負インデックス (その面の時点で定義済みの頂点数から逆算する相対参照) を許容する。
// 頂点数 4 以上の面は先頭頂点からのファン三角形分割
bool LoadObjText(const std::string& text, TriangleSoup& out, std::string* error = nullptr);

} // namespace modal
} // namespace mye
