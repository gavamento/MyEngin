#pragma once

// Debug/Release 一貫性ポリシー (engine_spec.md 11.2) 準拠の検証マクロ。
//   - 条件式は Debug / Release の両構成で常に評価される (Release で消える assert は存在しない)
//   - 失敗時の処理も両構成で同一: エラーログ + (デバッガ接続時のみ) ブレーク
//   - ロジックをこのマクロの有無で分岐させてはならない
namespace mye::detail {
void CheckFailed(const char* expr, const char* file, int line, const char* fmt, ...);
}

#define MYE_CHECK(expr)                                                          \
    do {                                                                         \
        if (!(expr)) {                                                           \
            ::mye::detail::CheckFailed(#expr, __FILE__, __LINE__, nullptr);      \
        }                                                                        \
    } while (0)

#define MYE_CHECKF(expr, ...)                                                    \
    do {                                                                         \
        if (!(expr)) {                                                           \
            ::mye::detail::CheckFailed(#expr, __FILE__, __LINE__, __VA_ARGS__);  \
        }                                                                        \
    } while (0)
