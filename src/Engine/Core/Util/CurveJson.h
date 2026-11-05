//====================================================================================
//                          CurveJson.h
//  MyEngin/ 秋田蓮音                                                       11/04/2026
//                                          MyeCurve (Shared/FeelMath.h) の JSON 表現 (M94、ADR-035)
//====================================================================================
#pragma once

#include "nlohmann/json.hpp"

#include "Shared/FeelMath.h"

namespace mye {

// .feel.json (FeelLibrary) と actions.json (InputActions の応答カーブ) が同じ書式を使う:
//   preset: { "mode": "preset", "ease": "OutQuad", "from": 1, "to": 0, "param": 2.5 }   (param は 0 なら省く)
//   custom: { "mode": "custom", "keys": [[t, value, inTangent, outTangent], ...] }       (最大 MYE_CURVE_MAX_KEYS)
// custom を書くときも preset の欄 (ease / from / to) は書かない — 読むときは out の既定値のまま残る
nlohmann::json CurveToJson(const MyeCurve& c);
// 無い / 壊れた欄は out の値のまま。不明なイージング名は WARN して Linear。制御点の整列はしない
// (呼び出し側の Sanitize が行う)
void CurveFromJson(const nlohmann::json& j, MyeCurve& out);

} // namespace mye
