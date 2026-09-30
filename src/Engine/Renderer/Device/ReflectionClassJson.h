//====================================================================================
//                          ReflectionClassJson.h
//  MyEngine/ 秋田蓮音                                                      09/08/2026
//                                          .mat.json の reflectionClass を読む唯一の規則
//====================================================================================
#pragma once

// ★このヘッダを include してよいのは `.mat.json` を読む 2 か所だけ
//   (GpuResources.cpp の ParseMaterialJson / InspectorWindow.cpp の LoadMaterialEdit)。
//   nlohmann/json.hpp は重いので GpuResources.h (45 ファイルが引く) には出さない —
//   そのためだけに切り出した小さなヘッダ。
#include "nlohmann/json.hpp"

namespace mye {

// M67h: `.mat.json` の "reflectionClass" を読む**唯一の規則** (spec §4.1)。
//
// 規則を 1 本にしてあるのは、読み方が 2 か所に分かれると「Inspector に出る値」と
// 「描画に効く値」が静かに食い違うため (CLAUDE.md の ShapeAcousticSpatial /
// MakeSourcePlay と同型)。Inspector 側は private でヘッドレスから叩けないので、
// **この関数を AssetOpsSelfTest が固定していることが Inspector 側の唯一の機械的な担保**。
//
// 判定順 (M67h / spec §4.1):
//   (1) キーが無ければ kRtReflClassDefault を返して終了。**無言** — 既存の .mat.json は
//       1 枚もこのキーを書いていないので、ここで警告を出すと本物の警告が埋もれる
//   (2) 数値型なら値を見る。**「非整数」は JSON の型ではなく値**で判定するので
//       `3.0` は 3 として受ける (jq / Python / 手編集は整数を float で書き出しうる。
//       型で弾くと「書いた値が無言で無視される」静かなデータ損失になる)
//   (3) 小数部を持つ値 / 数値でない型 / 範囲外は kRtReflClassDefault + 警告 1 行。
//       範囲外は**クランプしない** — -1 が 0 (Hero) に丸まると、打ち間違いが
//       「最も重いクラス」に化けて静かにコストだけ増える (RtTypes.h の kRtReflClassDefault)
//
// ★どの入力でも例外を投げない。呼び元の ParseMaterialJson は try の外なので、
//   ここで nlohmann の type_error が飛ぶとマテリアル 1 枚で起動ごと落ちる。
int ParseReflectionClassJson(const nlohmann::json& root);

} // namespace mye
