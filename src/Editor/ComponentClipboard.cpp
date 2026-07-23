#include "Editor/ComponentClipboard.h"

#include "Engine/Core/ComponentRegistry.h"
#include "Engine/Core/JsonUtil.h"

namespace mye {

ComponentClipboard& GetComponentClipboard()
{
    static ComponentClipboard s_clipboard;
    return s_clipboard;
}

nlohmann::json ComponentFieldsToJson(const ComponentDesc& desc, const void* comp)
{
    nlohmann::json j = nlohmann::json::object();
    for (const FieldDesc& f : desc.fields) {
        j[f.name] = FieldToJson(comp, f);
    }
    return j;
}

void ComponentFieldsFromJson(const ComponentDesc& desc, void* comp, const nlohmann::json& j)
{
    if (!j.is_object()) {
        return;
    }
    for (const FieldDesc& f : desc.fields) {
        auto it = j.find(f.name);
        if (it != j.end()) {
            FieldFromJson(comp, f, *it); // 型不一致 / EntityRef は false = 現在値維持
        }
    }
}

} // namespace mye
