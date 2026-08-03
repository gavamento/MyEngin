#include "Engine/Core/LocalizationSelfTest.h"

#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "Engine/Core/Localization.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Utf8.h"

namespace mye {

namespace {

// "###" 以降 = ImGui の ID 部。無ければ空を返す
std::string IdPart(const char* s)
{
    const char* p = std::strstr(s, "###");
    return (p != nullptr) ? std::string(p + 3) : std::string();
}

// 書式文字列から変換指定子だけを順に抜き出す ("%%" はリテラルなので無視)。
// 例: "%d 件 / %.1f ms" -> { "%d", "%.1f" }
std::vector<std::string> FormatSpecs(const char* s)
{
    std::vector<std::string> out;
    for (const char* p = s; *p != '\0'; ++p) {
        if (*p != '%') {
            continue;
        }
        if (p[1] == '%') {
            ++p; // エスケープされたパーセント
            continue;
        }
        const char* start = p++;
        while (*p != '\0' && std::strchr("-+ #0123456789.*", *p) != nullptr) {
            ++p;
        }
        while (*p == 'h' || *p == 'l' || *p == 'z' || *p == 'j' || *p == 't' || *p == 'L'
               || *p == 'I' || *p == '3' || *p == '2' || *p == '6' || *p == '4') {
            ++p; // 長さ修飾子 (%llu / %zu / %I64d …)
        }
        if (*p == '\0') {
            break;
        }
        out.emplace_back(start, static_cast<size_t>(p - start) + 1);
    }
    return out;
}

// UTF-8 として完結しているか (末尾でマルチバイト列が切れていないか)
bool IsWellFormedUtf8(const char* s)
{
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p != '\0';) {
        size_t seq = 1;
        if (*p >= 0xF0) {
            seq = 4;
        } else if (*p >= 0xE0) {
            seq = 3;
        } else if (*p >= 0xC0) {
            seq = 2;
        }
        for (size_t k = 1; k < seq; ++k) {
            if (p[k] == '\0') {
                return false; // 継続バイトが足りない
            }
        }
        p += seq;
    }
    return true;
}

} // namespace

bool RunLocalizationSelfTest()
{
    MYE_LOG_INFO("==== Localization self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    const Lang saved = CurrentLanguage();
    const size_t count = static_cast<size_t>(StrId::Count);
    MYE_LOG_INFO("  entries = %zu", count);

    // ---- 全項目が en/ja とも非空 ----
    {
        size_t empty = 0;
        for (size_t i = 0; i < count; ++i) {
            const StrId id = static_cast<StrId>(i);
            if (TrIn(Lang::En, id)[0] == '\0' || TrIn(Lang::Ja, id)[0] == '\0') {
                MYE_LOG_ERROR("    empty string at index %zu", i);
                ++empty;
            }
        }
        check(empty == 0, "all entries non-empty in both languages");
    }

    // ---- "###" の ID 部が en/ja で一致 (言語切替でドッキング配置が壊れないこと) ----
    {
        size_t mismatch = 0;
        for (size_t i = 0; i < count; ++i) {
            const StrId id = static_cast<StrId>(i);
            const std::string en = IdPart(TrIn(Lang::En, id));
            const std::string ja = IdPart(TrIn(Lang::Ja, id));
            if (en != ja) {
                MYE_LOG_ERROR("    ### id mismatch at index %zu: '%s' vs '%s'", i, en.c_str(),
                              ja.c_str());
                ++mismatch;
            }
        }
        check(mismatch == 0, "### id part identical across languages");
    }

    // ---- "###" の ID 部が一意 (ウィンドウ ID の衝突防止) ----
    {
        std::unordered_set<std::string> seen;
        size_t dup = 0;
        for (size_t i = 0; i < count; ++i) {
            const std::string id = IdPart(TrIn(Lang::En, static_cast<StrId>(i)));
            if (id.empty()) {
                continue;
            }
            if (!seen.insert(id).second) {
                MYE_LOG_ERROR("    duplicate ### id '%s' at index %zu", id.c_str(), i);
                ++dup;
            }
        }
        check(dup == 0, "### id part unique across table");
    }

    // ---- 変換指定子の並びが一致 (MSVC printf は "%1$s" 形式に非対応) ----
    {
        size_t mismatch = 0;
        for (size_t i = 0; i < count; ++i) {
            const StrId id = static_cast<StrId>(i);
            if (FormatSpecs(TrIn(Lang::En, id)) != FormatSpecs(TrIn(Lang::Ja, id))) {
                MYE_LOG_ERROR("    format specifier mismatch at index %zu", i);
                ++mismatch;
            }
        }
        check(mismatch == 0, "format specifiers match across languages");
    }

    // ---- 言語切替が Tr() に反映される / 往復で戻る ----
    {
        const StrId probe = StrId::Win_Hierarchy;
        SetLanguage(Lang::En);
        const bool en = std::strcmp(Tr(probe), TrIn(Lang::En, probe)) == 0;
        SetLanguage(Lang::Ja);
        const bool ja = std::strcmp(Tr(probe), TrIn(Lang::Ja, probe)) == 0;
        check(en && ja, "SetLanguage switches Tr() result");
        check(CurrentLanguage() == Lang::Ja, "CurrentLanguage reflects SetLanguage");
        check(LangFromString(LangToString(Lang::En)) == Lang::En
                  && LangFromString(LangToString(Lang::Ja)) == Lang::Ja
                  && LangFromString(nullptr) == Lang::Ja && LangFromString("xx") == Lang::Ja,
              "Lang <-> string round trip (unknown falls back to ja)");
    }

    // ---- UTF-8 の切り詰めがマルチバイト列を分断しない ----
    {
        char buf[64];
        struct Case {
            const char* src;
            size_t cap;      // ヌル終端込みの容量
            size_t expected; // 期待する書き込みバイト数
        };
        // "あいう" = 3 バイト x 3。cap-1 バイトに収まる範囲で文字単位に落ちる
        const Case cases[] = {
            { "\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86", 10, 9 }, // ちょうど収まる
            { "\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86", 9, 6 },  // 8 バイトまで -> 2 文字
            { "\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86", 7, 6 },  // 6 バイトまで -> 2 文字
            { "\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86", 6, 3 },  // 5 バイトまで -> 1 文字
            { "\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86", 4, 3 },  // 3 バイトまで -> 1 文字
            { "\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86", 3, 0 },  // 2 バイトまで -> 0 文字
            { "\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86", 1, 0 },  // 終端のみ
            { "abc", 3, 2 },                                   // ASCII は 1 バイト単位
            { "", 8, 0 },
            { "\xE3\x81", 8, 0 },      // 途中で終端した壊れた入力は落とす
            { "\xF0\x9F\x98\x80", 5, 4 }, // 4 バイト文字 (絵文字) がちょうど収まる
            { "\xF0\x9F\x98\x80", 4, 0 }, // 1 バイト足りない -> 落とす
        };
        size_t bad = 0;
        for (const Case& c : cases) {
            const size_t n = utf8::CopyTruncated(buf, c.cap, c.src);
            if (n != c.expected || std::strlen(buf) != n || !IsWellFormedUtf8(buf)) {
                MYE_LOG_ERROR("    CopyTruncated(cap=%zu) wrote %zu, expected %zu", c.cap, n,
                              c.expected);
                ++bad;
            }
        }
        check(bad == 0, "utf8::CopyTruncated keeps sequences intact");

        // 引数の防御
        check(utf8::CopyTruncated(buf, sizeof(buf), nullptr) == 0 && buf[0] == '\0'
                  && utf8::CopyTruncated(nullptr, 8, "x") == 0
                  && utf8::CopyTruncated(buf, 0, "x") == 0,
              "utf8::CopyTruncated handles null/zero-capacity");
    }

    SetLanguage(saved);

    if (failCount == 0) {
        MYE_LOG_INFO("==== Localization self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Localization self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
