#include "Engine/Core/Localization.h"

#include <atomic>
#include <cstring>
#include <iterator>

namespace mye {
namespace {

constexpr const char* kEn[] = {
#define MYE_STR(id, en, ja) en,
#include "Engine/Core/LocalizationTable.inl"
#undef MYE_STR
};

constexpr const char* kJa[] = {
#define MYE_STR(id, en, ja) ja,
#include "Engine/Core/LocalizationTable.inl"
#undef MYE_STR
};

constexpr size_t kCount = static_cast<size_t>(StrId::Count);
static_assert(std::size(kEn) == kCount, "English table out of sync with StrId");
static_assert(std::size(kJa) == kCount, "Japanese table out of sync with StrId");

// FileWatcher のワーカスレッド (FileWatcher.cpp) からも MYE_LOG_* 経由で読まれうるので
// アトミックにする。x64 では単なる load/store になりコストは増えない
std::atomic<const char* const*> g_table{ kJa };
std::atomic<Lang> g_lang{ Lang::Ja };

} // namespace

const char* Tr(StrId id)
{
    const size_t i = static_cast<size_t>(id);
    if (i >= kCount) {
        return "?";
    }
    return g_table.load(std::memory_order_relaxed)[i];
}

const char* TrIn(Lang lang, StrId id)
{
    const size_t i = static_cast<size_t>(id);
    if (i >= kCount) {
        return "?";
    }
    return (lang == Lang::En) ? kEn[i] : kJa[i];
}

void SetLanguage(Lang lang)
{
    g_lang.store(lang, std::memory_order_relaxed);
    g_table.store(lang == Lang::En ? kEn : kJa, std::memory_order_relaxed);
}

Lang CurrentLanguage()
{
    return g_lang.load(std::memory_order_relaxed);
}

const char* LangToString(Lang lang)
{
    return lang == Lang::En ? "en" : "ja";
}

Lang LangFromString(const char* s)
{
    return (s != nullptr && std::strcmp(s, "en") == 0) ? Lang::En : Lang::Ja;
}

} // namespace mye
