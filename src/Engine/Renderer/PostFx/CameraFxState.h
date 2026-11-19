#pragma once
#include "Engine/Core/Ecs/EntityID.h"
#include "Engine/Renderer/Shader/ProjectShaderProperties.h"
#include <algorithm>
#include <cmath>

namespace mye {
// Presentation state is owned by the world and keyed by the camera entity.
struct CameraFxPassState {
    PropertyParseResult schema;
    std::unordered_map<std::string, PropValue> overrides;
    bool drawn = false;
    bool seen = false;
    uint64_t schemaSource = 0;
    std::string shaderName;

    bool Set(const char* name, const PropValue& value) {
        if (!name) return false;
        for (const auto& property : schema.properties) {
            if (property.name != name) continue;
            if (const auto* scalar = std::get_if<float>(&value)) {
                if ((property.type != PropType::Float && property.type != PropType::Range) ||
                    !std::isfinite(*scalar)) return false;
                overrides[name] = property.attr.hasRange
                    ? std::clamp(*scalar, property.attr.rangeMin, property.attr.rangeMax) : *scalar;
                return true;
            }
            const auto* vector = std::get_if<std::array<float, 4>>(&value);
            if (!vector || (property.type != PropType::Vector && property.type != PropType::Color))
                return false;
            for (float v : *vector) if (!std::isfinite(v)) return false;
            overrides[name] = *vector;
            return true;
        }
        return false;
    }
    void RefreshSchema(const PropertyParseResult& current) {
        schema = current;
        auto previous = std::move(overrides);
        overrides.clear();
        for (const auto& kv : previous) Set(kv.first.c_str(), kv.second);
    }
};
struct CameraFxState {
    AssetID stack;
    std::unordered_map<std::string, CameraFxPassState> passes;
    CameraFxPassState* Find(const char* id) {
        if (!id) return nullptr;
        auto it = passes.find(id);
        return it == passes.end() ? nullptr : &it->second;
    }
};
} // namespace mye
