//====================================================================================
//                          EngineIndexDump.cpp
//  MyEngin/ 秋田蓮音                                                       10/15/2026
//                                          エンジン索引ダンプの実装
//====================================================================================
#include "Editor/Tools/EngineIndexDump.h"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "Engine/Core/Ecs/ComponentRegistry.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Platform/PathUtil.h" // WideToUtf8
#include "Engine/Engine/App/EngineCli.h"
#include "Engine/Engine/Demo/ShowcaseScenes.h"

namespace mye {
namespace {

// JSON 文字列リテラル。UTF-8 はそのまま通し、" \ と制御文字だけをエスケープする
void AppendJsonString(std::string& out, const char* text)
{
    out.push_back('"');
    for (const char* p = text; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
}

// 最短の往復表現。printf の丸めに依存しないので Debug / Release で同じ文字列になる
void AppendFloat(std::string& out, float v)
{
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v);
    out.append(buf, r.ptr);
}

const char* FieldTypeName(FieldType t)
{
    switch (t) {
    case FieldType::Float: return "Float";
    case FieldType::Int32: return "Int32";
    case FieldType::UInt32: return "UInt32";
    case FieldType::UInt64: return "UInt64";
    case FieldType::Bool: return "Bool";
    case FieldType::Float2: return "Float2";
    case FieldType::Float3: return "Float3";
    case FieldType::Float4: return "Float4";
    case FieldType::Quat: return "Quat";
    case FieldType::Color: return "Color";
    case FieldType::EntityRef: return "EntityRef";
    case FieldType::AssetRef: return "AssetRef";
    case FieldType::String64: return "String64";
    case FieldType::Float4x4: return "Float4x4";
    case FieldType::String256: return "String256";
    }
    return "Unknown"; // FieldType を足したのにここへ足し忘れた場合 (EngineIndexDumpSelfTest が検出する)
}

// ビットの立っているものを名前の配列にする (名前の並び = ビット順)
template <size_t N>
void AppendFlagNames(std::string& out, uint32_t flags, const std::pair<uint32_t, const char*> (&table)[N])
{
    out.push_back('[');
    bool first = true;
    for (const auto& [bit, name] : table) {
        if ((flags & bit) == 0) {
            continue;
        }
        if (!first) {
            out.push_back(',');
        }
        first = false;
        AppendJsonString(out, name);
    }
    out.push_back(']');
}

const std::pair<uint32_t, const char*> kComponentFlagNames[] = {
    { kComponentNoSerialize, "noSerialize" },
    { kComponentHidden, "hidden" },
    { kComponentNoHash, "noHash" },
    { kComponentUiAux, "uiAux" },
};

const std::pair<uint32_t, const char*> kFieldFlagNames[] = {
    { kFieldReadOnly, "readOnly" },
    { kFieldNoSerialize, "noSerialize" },
    { kFieldHidden, "hidden" },
    { kFieldNoHash, "noHash" },
};

void AppendField(std::string& out, const FieldDesc& f)
{
    out += "{\"name\":";
    AppendJsonString(out, f.name);
    out += ",\"type\":";
    AppendJsonString(out, FieldTypeName(f.type));
    out += ",\"offset\":" + std::to_string(f.offset);
    out += ",\"flags\":";
    AppendFlagNames(out, f.flags, kFieldFlagNames);
    if (f.displayName != nullptr) {
        out += ",\"jp\":";
        AppendJsonString(out, f.displayName);
    }
    if (f.tooltip != nullptr) {
        out += ",\"tooltip\":";
        AppendJsonString(out, f.tooltip);
    }
    if (f.minVal != f.maxVal) {
        out += ",\"min\":";
        AppendFloat(out, f.minVal);
        out += ",\"max\":";
        AppendFloat(out, f.maxVal);
    }
    out.push_back('}');
}

} // namespace

std::string BuildEngineIndexDumpJson()
{
    RegisterBuiltinComponents(); // 冪等。World は作らない

    std::string out;
    out += "{\n\"schema\":1,\n\"components\":[\n";
    const ComponentRegistry& registry = ComponentRegistry::Get();
    bool firstComponent = true;
    for (ComponentTypeId id = 0; id < registry.Count(); ++id) {
        const ComponentDesc& desc = registry.Desc(id);
        if ((desc.flags & kComponentScriptState) != 0) {
            continue; // GameLogic.dll の動的登録分は組み込みではない
        }
        if (!firstComponent) {
            out += ",\n";
        }
        firstComponent = false;
        out += "{\"name\":";
        AppendJsonString(out, desc.name);
        out += ",\"typeId\":" + std::to_string(id);
        out += ",\"size\":" + std::to_string(desc.size);
        out += ",\"align\":" + std::to_string(desc.align);
        out += ",\"flags\":";
        AppendFlagNames(out, desc.flags, kComponentFlagNames);
        out += ",\"fields\":[";
        for (size_t i = 0; i < desc.fields.size(); ++i) {
            out += (i == 0) ? "\n" : ",\n";
            AppendField(out, desc.fields[i]);
        }
        out += desc.fields.empty() ? "]}" : "\n]}";
    }

    out += "\n],\n\"cliEngine\":[\n";
    const std::vector<EngineCliFlagInfo> engineFlags = ListEngineCliFlags();
    for (size_t i = 0; i < engineFlags.size(); ++i) {
        out += (i == 0) ? "" : ",\n";
        out += "{\"name\":";
        AppendJsonString(out, WideToUtf8(engineFlags[i].name).c_str());
        out += ",\"value\":";
        AppendJsonString(out, engineFlags[i].value);
        out.push_back('}');
    }

    out += "\n],\n\"cliShowcase\":[\n";
    const std::vector<const ShowcaseDef*> showcases = ListShowcases();
    for (size_t i = 0; i < showcases.size(); ++i) {
        out += (i == 0) ? "" : ",\n";
        out += "{\"flag\":";
        AppendJsonString(out, WideToUtf8(showcases[i]->flag).c_str());
        out += showcases[i]->editorOnly ? ",\"editorOnly\":true}" : ",\"editorOnly\":false}";
    }
    out += "\n]\n}\n";
    return out;
}

int RunDumpIndexCli(const std::wstring& outPath)
{
    const std::string json = BuildEngineIndexDumpJson();
    std::ofstream file(outPath, std::ios::binary | std::ios::trunc);
    if (!file) {
        std::fprintf(stderr, "dump-index: cannot open %s\n", WideToUtf8(outPath).c_str());
        return 1;
    }
    file.write(json.data(), static_cast<std::streamsize>(json.size()));
    file.close();
    if (!file) {
        std::fprintf(stderr, "dump-index: write failed: %s\n", WideToUtf8(outPath).c_str());
        return 1;
    }
    return 0;
}

} // namespace mye
