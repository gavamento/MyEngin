//====================================================================================
//                          ReflectionClassJson.cpp
//  MyEngine/ 秋田蓮音                                                      09/08/2026
//                                          .mat.json の reflectionClass を読む唯一の規則
//====================================================================================
#include "Engine/Renderer/ReflectionClassJson.h"

#include <cmath>
#include <string>

#include "Engine/Core/Log.h"
#include "Engine/Renderer/RayTracing/RtTypes.h" // kRtReflClassDefault / kRtReflClassCount

namespace mye {

namespace {

// 警告に出す「読めなかった値の見え方」。
// ★dump() は既定だと**不正な UTF-8 の文字列で type_error を投げる** ので、置換ハンドラを
//   明示する — この関数の存在理由が「どんな入力でも例外を外へ出さない」なので、
//   警告を組み立てる側で投げたら本末転倒になる
std::string ShowJsonValue(const nlohmann::json& node)
{
    return node.dump(/*indent=*/-1, /*indent_char=*/' ', /*ensure_ascii=*/false,
                     nlohmann::json::error_handler_t::replace);
}

} // namespace

int ParseReflectionClassJson(const nlohmann::json& root)
{
    constexpr const char* kKey = "reflectionClass";
    // (1) 欠損は無言で中立 (既存の .mat.json は 1 枚も書いていない = 挙動不変)
    if (!root.contains(kKey)) {
        return kRtReflClassDefault;
    }
    const nlohmann::json& node = root[kKey];

    // (2) 「非整数」は値で判定する。get<double>() は**数値型でしか呼ばない** ので
    //     文字列・真偽・null・配列・オブジェクトが来ても type_error は出ない。
    //     非有限 (NaN / Inf) は JSON テキストには書けないが、コードで組んだ json が
    //     渡ってくる経路 (プレビュー / テスト) があるので値としても弾いておく。
    //     double は 2^53 まで整数を正確に持つ = クラス番号の桁で丸め誤差は出ない
    bool integerValued = false;
    double v = 0.0;
    if (node.is_number()) {
        v = node.get<double>();
        integerValued = std::isfinite(v) && v == std::floor(v);
    }
    if (integerValued && v >= 0.0 && v < static_cast<double>(kRtReflClassCount)) {
        return static_cast<int>(v);
    }

    // (3) キーがあって落としたときだけ 1 行。値の見え方をそのまま出さないと
    //     「どの .mat.json のどの書き方が拾われなかったのか」が分からない
    MYE_LOG_WARN("material: %s=%s is not a whole number in [0,%d); using %d (Default)", kKey,
                 ShowJsonValue(node).c_str(), kRtReflClassCount, kRtReflClassDefault);
    return kRtReflClassDefault;
}

} // namespace mye
