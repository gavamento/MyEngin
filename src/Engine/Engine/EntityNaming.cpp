#include "Engine/Engine/EntityNaming.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "Engine/Core/Components.h"
#include "Engine/Core/NameUtil.h"
#include "Engine/Core/World.h"

namespace mye {

void SetEntityName(World& world, EntityID e, std::string_view name)
{
    auto* nc = world.GetComponent<NameComponent>(e);
    if (!nc) {
        return;
    }
    const std::string_view fit = nameutil::TruncateUtf8(name, kMaxEntityNameBytes);
    std::memset(nc->value, 0, sizeof(nc->value));
    std::memcpy(nc->value, fit.data(), fit.size());
}

std::string MakeUniqueSiblingName(World& world, EntityID parent, std::string_view desired,
                                  EntityID exclude)
{
    // 兄弟の名前を集める。unordered コンテナの range-for は check_rules 規則 7 の WARN 対象
    // かつ走査順が決定論的でないので、素直に vector + 線形探索にする (兄弟数は小さい)
    std::vector<std::string> taken;
    EntityID e = kNullEntity;
    if (parent == kNullEntity) {
        e = world.FirstRoot();
    } else if (auto* ph = world.GetComponent<HierarchyComponent>(parent)) {
        e = ph->firstChild;
    }
    while (e != kNullEntity) {
        auto* h = world.GetComponent<HierarchyComponent>(e);
        if (e != exclude) {
            if (auto* nc = world.GetComponent<NameComponent>(e)) {
                taken.emplace_back(nc->value); // value は必ず NUL 終端 (SetEntityName がゼロ埋め)
            }
        }
        e = h ? h->nextSibling : kNullEntity;
    }

    return nameutil::MakeUniqueNumbered<char>(
        desired, std::string_view(), kMaxEntityNameBytes, [&](const std::string& c) {
            return std::find(taken.begin(), taken.end(), c) != taken.end();
        });
}

std::string SanitizeEntityName(std::string_view desired, std::string_view fallback)
{
    std::string out;
    out.reserve(desired.size());
    for (const char c : desired) {
        if (c != '/') {
            out.push_back(c);
        }
    }
    // 前後の空白 (ASCII 空白類のみ。UTF-8 の継続バイトは 0x80 以上なので誤爆しない)
    const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    size_t b = 0;
    size_t e = out.size();
    while (b < e && isSpace(out[b])) {
        ++b;
    }
    while (e > b && isSpace(out[e - 1])) {
        --e;
    }
    out = out.substr(b, e - b);
    return out.empty() ? std::string(fallback) : out;
}

void FinishRename(World& world, EntityID e, std::string_view edited, std::string_view original)
{
    // edited / original は NameComponent のバッファを指しうるので、書き戻す前にコピーを作る
    const std::string text(edited);
    if (text == original) {
        // 変更なし (Esc キャンセル / 無編集でフォーカスが外れた)。**名前は一切変えない**。
        // 残骸バイトのゼロ埋めだけは行う (ImGui の ImStrncpy が NUL 以降を消さないため)
        SetEntityName(world, e, text);
        return;
    }
    const std::string clean = SanitizeEntityName(text, "GameObject");
    SetEntityName(world, e, MakeUniqueSiblingName(world, world.GetParent(e), clean, /*exclude=*/e));
}

} // namespace mye
