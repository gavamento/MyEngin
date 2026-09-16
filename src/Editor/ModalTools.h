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

// --modal-bake [--project DIR] [--modal-backend cpu|d3d11cs] (M76e) の本体。プロジェクト
// (または裸のエンジンリポジトリ) の assets 以下をヘッドレス登録し、登録された全メッシュを
// ModalSoundLibrary::BakeSync で焼いて `.msfm` へ書く。ウィンドウも D3D も作らない。
// 1 行/メッシュ (`name state ms validCells`) + 合計
// (`bakes= bakeMsAvg= silent=`。silent は全 cell・全帯域で mask が落ちているメッシュ数 —
// どんな力積でも鳴らない「常時無音」の可視化、spec sub-10 C) を標準出力へ。
// backendName は空なら "cpu" (--modal-backend 未指定時の既定と同じ)。
// exit 0 = 成功 (0 件でも成功) / 1 = assets root が見つからない / 2 = .dmnet が無い・ロード失敗
int RunModalBakeCli(const std::wstring& projectDir, const std::wstring& backendName = L"cpu");

} // namespace modaltools
} // namespace mye
