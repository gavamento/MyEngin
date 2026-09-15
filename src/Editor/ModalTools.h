//====================================================================================
//                          ModalTools.h
//  MyEngine/ 秋田蓮音                                                      09/16/2026
//                                          --modal-voxelize (Deep-Modal のヘッドレス CLI)
//====================================================================================
#pragma once
#include <string>

namespace mye {
namespace modaltools {

// --modal-voxelize --list F --out DIR の本体。list の各行 (1 行 1 パス) を
// 32^3 ボクセルへ焼いて DIR\<stem>#mesh<N>#prim<M>.mvox を書く。
// 行の形式は "builtin://<name>" (MeshLibrary の 6 種) / ".off" / ".obj" /
// ".fbx" / ".gltf" / ".glb"。ウィンドウも D3D も作らない。
// FBX / glTF はヘッドレス登録 (SubAssetMigration.cpp と同経路) でモデル中の全メッシュ×パーツを
// 列挙する。存在しないパス・読めないファイル・未対応の拡張子は 1 行ごとにエラーを出して
// 続行し、1 件でもあれば戻り値は 1 (exit code に使う想定)。list 自体が読めなければ 1
int RunModalVoxelizeCli(const std::wstring& listPath, const std::wstring& outDir);

} // namespace modaltools
} // namespace mye
