/*----
 CameraFxState.h  カメラごとの fxstack パス上書き状態 (ABI v37)
 作成者: 秋田蓮音                                11/19/2026
----*/
#pragma once
#include "Engine/Core/Ecs/EntityID.h"
#include "Engine/Renderer/Shader/ProjectShaderProperties.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>

namespace mye {

// 表示専用の状態 (シミュレーションに入らない)。World がカメラのエンティティ単位で持ち、
// fxstack の差し替えで捨てる。上書き値は現在のシェーダのスキーマで検証してから保持する
struct CameraFxPassState
{
    PropertyParseResult schema;
    std::unordered_map<std::string, PropValue> overrides;
    bool drawn = false;             // このフレームで実際に描画 / Dispatch された
    bool seen = false;              // このフレームの fxstack に含まれていた
    uint64_t schemaSource = 0;      // schema を取り込んだときのシェーダ世代
    std::string shaderName;

    // スキーマに無い名前・型違い・非有限値は false。Range は範囲に丸めて保持する
    bool Set(const char* name, const PropValue& value)
    {
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

    // シェーダが変わったら新しいスキーマで既存の上書きを検証し直す (合わないものは捨てる)
    void RefreshSchema(const PropertyParseResult& current)
    {
        schema = current;
        auto previous = std::move(overrides);
        overrides.clear();
        for (const auto& kv : previous) Set(kv.first.c_str(), kv.second);
    }
};

struct CameraFxState
{
    AssetID stack;
    std::unordered_map<std::string, CameraFxPassState> passes; // キー = fxstack パスの id

    CameraFxPassState* Find(const char* id)
    {
        if (!id) return nullptr;
        auto it = passes.find(id);
        return it == passes.end() ? nullptr : &it->second;
    }
};

} // namespace mye
