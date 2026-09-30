#pragma once
#include <string>
#include <string_view>

namespace mye::nameutil {

// UTF-8 バイト列を maxBytes 以内へ切り詰める (マルチバイト文字を途中で割らない)。
inline std::string_view TruncateUtf8(std::string_view s, size_t maxBytes)
{
    if (s.size() <= maxBytes) {
        return s;
    }
    size_t n = maxBytes;
    // 継続バイト (10xxxxxx) の上に落ちたら文字の先頭バイトまで戻る
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) {
        --n;
    }
    return s.substr(0, n);
}

namespace detail {

inline std::string Numbering(char, int i) { return " (" + std::to_string(i) + ")"; }
inline std::wstring Numbering(wchar_t, int i) { return L" (" + std::to_wstring(i) + L")"; }

inline void Fit(std::string& s, size_t budget) { s = std::string(TruncateUtf8(s, budget)); }
inline void Fit(std::wstring& s, size_t budget)
{
    if (s.size() > budget) {
        s.resize(budget); // ファイル名用。UTF-16 なので呼び出し側は budget=0 (無制限) で使う
    }
}

} // namespace detail

// 「まず素の名前を試し、衝突する間 " (1)", " (2)", … を後置する」= リポジトリ唯一の連番規則。
// アセットのファイル名 (AssetOps::MakeUniquePath) とエンティティの兄弟名 (MakeUniqueSiblingName)
// がこれを共有する。taken(candidate) が true の間ループする。
//
// - **既存の " (n)" は剥がさない** ("a (1)" の衝突は "a (1) (1)")。アセット側の既存挙動
//   (AssetOpsSelfTest が固定) をそのまま保つための意図的な仕様。
// - budget > 0 なら候補**全体**をそのサイズ以内に収める (stem 側を詰める。char 版は UTF-8 境界を守る)。
//   連番ぶんの領域を先に確保してから詰めるので、切り詰めで候補が衝突名に戻って
//   ループが解けなくなることがない。
// - 反復上限は異常系のガード (実運用では到達しない)。到達時は最後の候補をそのまま返す。
template <typename CharT, typename TakenFn>
std::basic_string<CharT> MakeUniqueNumbered(std::basic_string_view<CharT> stem,
                                            std::basic_string_view<CharT> suffix, size_t budget,
                                            TakenFn&& taken)
{
    auto build = [&](const std::basic_string<CharT>& num) {
        std::basic_string<CharT> base(stem);
        if (budget > 0) {
            const size_t room = num.size() + suffix.size();
            detail::Fit(base, (budget > room) ? budget - room : 0);
        }
        return base + num + std::basic_string<CharT>(suffix);
    };

    std::basic_string<CharT> candidate = build(std::basic_string<CharT>());
    for (int i = 1; taken(candidate) && i < 100000; ++i) {
        candidate = build(detail::Numbering(CharT{}, i));
    }
    return candidate;
}

} // namespace mye::nameutil
