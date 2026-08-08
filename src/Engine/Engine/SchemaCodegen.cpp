#include "Engine/Engine/SchemaCodegen.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "Engine/Core/ComponentRegistry.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/SchemaComponents.h"
#include "Engine/Platform/PathUtil.h"

namespace mye::schema {
namespace {

// ---- 識別子の検査 -----------------------------------------------------------
// 生成名は C++ と C# の両方でそのまま識別子になるので、両言語の予約語を併せて弾く。
// スキーマ名は自由文字列 (登録は通る) — 生成できないだけで、実行時アクセスは
// ハッシュ経由で引き続き可能
bool IsReservedWord(const std::string& s)
{
    static const char* kWords[] = {
        // C++
        "alignas", "auto", "bool", "break", "case", "catch", "char", "class", "const",
        "constexpr", "continue", "default", "delete", "do", "double", "else", "enum",
        "explicit", "extern", "false", "float", "for", "friend", "goto", "if", "inline", "int",
        "long", "mutable", "namespace", "new", "noexcept", "nullptr", "operator", "private",
        "protected", "public", "register", "return", "short", "signed", "sizeof", "static",
        "struct", "switch", "template", "this", "throw", "true", "try", "typedef", "typeid",
        "typename", "union", "unsigned", "using", "virtual", "void", "volatile", "while",
        // C#
        "abstract", "as", "base", "byte", "checked", "decimal", "delegate", "event", "finally",
        "fixed", "foreach", "implicit", "in", "interface", "internal", "is", "lock", "object",
        "out", "override", "params", "readonly", "ref", "sbyte", "sealed", "stackalloc",
        "string", "uint", "ulong", "unchecked", "unsafe", "ushort", "var",
        // 生成物側で使う名前 (メンバと衝突すると C++ が通らない)
        "Read", "Write", "FieldHash", "NameHash", "kNameHash",
    };
    for (const char* w : kWords) {
        if (s == w) {
            return true;
        }
    }
    return false;
}

bool IsCleanIdentifier(const std::string& s)
{
    if (s.empty() || IsReservedWord(s)) {
        return false;
    }
    const auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    if (!alpha(s[0]) && s[0] != '_') {
        return false;
    }
    for (char c : s) {
        if (!alpha(c) && !digit(c) && c != '_') {
            return false;
        }
    }
    return true;
}

// アクセサ名 = 先頭 1 文字を大文字化 ("current" → "Current")
std::string Capitalize(const std::string& s)
{
    std::string r = s;
    if (!r.empty() && r[0] >= 'a' && r[0] <= 'z') {
        r[0] = static_cast<char>(r[0] - 'a' + 'A');
    }
    return r;
}

std::string Hex64(uint64_t v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(v));
    return buf;
}

std::string U32(uint32_t v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u", v);
    return buf;
}

// 値型 (固定長配列でない型) の C++ / C# 型名。配列型 (String64/256, Float4x4) は
// 呼び出し側が特別扱いするので nullptr を返す
const char* CppValueType(FieldType t)
{
    switch (t) {
    case FieldType::Float:     return "float";
    case FieldType::Int32:     return "int32_t";
    case FieldType::UInt32:    return "uint32_t";
    case FieldType::UInt64:    return "uint64_t";
    case FieldType::Bool:      return "bool";
    case FieldType::Float2:    return "MyeVec2";
    case FieldType::Float3:    return "MyeVec3";
    case FieldType::Float4:    return "MyeVec4";
    case FieldType::Quat:      return "MyeQuat";
    case FieldType::Color:     return "MyeColor";
    case FieldType::EntityRef: return "MyeEntityId";
    case FieldType::AssetRef:  return "uint64_t";
    default:                   return nullptr;
    }
}

const char* CsValueType(FieldType t)
{
    switch (t) {
    case FieldType::Float:     return "float";
    case FieldType::Int32:     return "int";
    case FieldType::UInt32:    return "uint";
    case FieldType::UInt64:    return "ulong";
    case FieldType::Bool:      return "bool";
    case FieldType::Float2:    return "MyeVec2";
    case FieldType::Float3:    return "MyeVec3";
    case FieldType::Float4:    return "MyeVec4";
    case FieldType::Quat:      return "MyeQuat";
    case FieldType::Color:     return "MyeColor";
    case FieldType::EntityRef: return "MyeEntityId";
    case FieldType::AssetRef:  return "ulong";
    default:                   return nullptr;
    }
}

bool IsStringType(FieldType t)
{
    return t == FieldType::String64 || t == FieldType::String256;
}

} // namespace

std::vector<CodegenComponent> BuildCodegenModel()
{
    std::vector<CodegenComponent> model;
    const ComponentRegistry& reg = ComponentRegistry::Get();
    for (const uint32_t id : RegisteredSchemaTypeIds()) {
        const ComponentDesc& d = reg.Desc(id);
        CodegenComponent c;
        c.name = d.name != nullptr ? d.name : "";
        c.nameHash = d.nameHash;
        c.size = d.size;
        c.align = d.align;
        if (!IsCleanIdentifier(c.name)) {
            MYE_LOG_WARN("[schema codegen] '%s': not a C/C# identifier - skipped", c.name.c_str());
            continue;
        }
        bool ok = true;
        std::vector<std::string> accessors;
        for (const FieldDesc& f : d.fields) {
            CodegenField g;
            g.name = f.name != nullptr ? f.name : "";
            g.nameHash = HashStr(g.name);
            g.type = f.type;
            g.offset = f.offset;
            g.size = FieldTypeSize(f.type);
            if (!IsCleanIdentifier(g.name)) {
                MYE_LOG_WARN("[schema codegen] '%s.%s': not a C/C# identifier - '%s' skipped",
                             c.name.c_str(), g.name.c_str(), c.name.c_str());
                ok = false;
                break;
            }
            // ★nameHash 衝突の検出。登録側 (RegisterOne) は名前の重複しか見ない —
            //   別名で同ハッシュだと ABI が常に先勝ちで解決し、静かに別フィールドを読む
            for (const CodegenField& prev : c.fields) {
                if (prev.nameHash == g.nameHash) {
                    MYE_LOG_ERROR("[schema codegen] '%s': field name hash collision "
                                  "('%s' vs '%s') - '%s' skipped",
                                  c.name.c_str(), prev.name.c_str(), g.name.c_str(),
                                  c.name.c_str());
                    ok = false;
                    break;
                }
            }
            if (!ok) {
                break;
            }
            const std::string acc = Capitalize(g.name);
            if (std::find(accessors.begin(), accessors.end(), acc) != accessors.end()) {
                MYE_LOG_WARN("[schema codegen] '%s': accessor name '%s' collides - '%s' skipped",
                             c.name.c_str(), acc.c_str(), c.name.c_str());
                ok = false;
                break;
            }
            accessors.push_back(acc);
            c.fields.push_back(std::move(g));
        }
        if (ok) {
            model.push_back(std::move(c));
        }
    }
    return model;
}

std::string EmitCppHeader(const std::vector<CodegenComponent>& model)
{
    std::string o;
    o += "// 自動生成 (MyEngine スキーマ codegen、M50d)。手編集禁止 — Rebuild Scripts で"
         "上書きされる。\n"
         "// assets\\schemas\\*.component.schema.json → レイアウトミラー + 型付きアクセサ\n"
         "// (汎用フィールド ABI v11 経由。値コピーのみ — エンジンのメモリへは触らない)。\n"
         "// nameHash は焼いてある (FNV-1a、HashStr と同一定数) / TypeId は焼かない "
         "(登録順依存)。\n"
         "#pragma once\n"
         "#include <stddef.h>\n"
         "#include <stdint.h>\n"
         "\n"
         "#include \"Shared/ScriptAPI.h\"\n";
    for (const CodegenComponent& c : model) {
        const std::string sn = c.name + "Schema";
        o += "\n// ---- " + c.name + " ----\n";
        o += "struct " + sn + " {\n";
        // ---- レイアウトミラー (実行時 offset をそのまま再現。隙間は明示の詰め物) ----
        uint32_t cur = 0;
        int padIdx = 0;
        for (const CodegenField& f : c.fields) {
            if (f.offset > cur) {
                o += "    uint8_t _pad" + U32(static_cast<uint32_t>(padIdx++)) + "_["
                     + U32(f.offset - cur) + "]; // 実行時レイアウトの詰め物ミラー\n";
            }
            if (f.type == FieldType::String64 || f.type == FieldType::String256) {
                o += "    char " + f.name + "[" + U32(f.size) + "];\n";
            } else if (f.type == FieldType::Float4x4) {
                o += "    float " + f.name + "[16];\n";
            } else {
                o += std::string("    ") + CppValueType(f.type) + " " + f.name + ";\n";
            }
            cur = f.offset + f.size;
        }
        if (c.size > cur) {
            o += "    uint8_t _pad" + U32(static_cast<uint32_t>(padIdx)) + "_["
                 + U32(c.size - cur) + "]; // 末尾詰め物 (サイズは型アラインメントへ切上げ)\n";
        }
        // ---- 定数 ----
        o += "\n    static constexpr uint64_t kNameHash = " + Hex64(c.nameHash) + "ull; // \""
             + c.name + "\"\n";
        o += "    struct FieldHash {\n";
        for (const CodegenField& f : c.fields) {
            o += "        static constexpr uint64_t " + f.name + " = " + Hex64(f.nameHash)
                 + "ull;\n";
        }
        o += "    };\n";
        // ---- 型付きアクセサ (失敗 = 非所持/未登録/型サイズ不一致で false) ----
        for (const CodegenField& f : c.fields) {
            const std::string acc = Capitalize(f.name);
            const std::string fh = "FieldHash::" + f.name;
            if (IsStringType(f.type)) {
                const std::string cap = U32(f.size);
                o += "\n    static bool Get" + acc
                     + "(const MyeUpdateContext& ctx, MyeEntityId e, char (&out)[" + cap
                     + "])\n    {\n        return MyeGetComponentField(ctx, e, kNameHash, " + fh
                     + ", out, " + cap + ") == " + cap + ";\n    }\n";
                o += "    static bool Set" + acc
                     + "(const MyeUpdateContext& ctx, MyeEntityId e, const char* utf8)\n"
                       "    {\n"
                       "        int32_t n = 0;\n"
                       "        while (utf8 != nullptr && utf8[n] != '\\0' && n < " + cap
                     + " - 1) { ++n; }\n"
                       "        // 尾部ゼロ埋め + 終端はエンジン側の契約 (String64 ハッシュ罠対策)\n"
                       "        return MyeSetComponentField(ctx, e, kNameHash, " + fh
                     + ", utf8 != nullptr ? utf8 : \"\", n + 1) != 0;\n    }\n";
            } else if (f.type == FieldType::Float4x4) {
                o += "\n    static bool Get" + acc
                     + "(const MyeUpdateContext& ctx, MyeEntityId e, float (&out)[16])\n"
                       "    {\n        return MyeGetComponentField(ctx, e, kNameHash, " + fh
                     + ", out, 64) == 64;\n    }\n";
                o += "    static bool Set" + acc
                     + "(const MyeUpdateContext& ctx, MyeEntityId e, const float (&v)[16])\n"
                       "    {\n        return MyeSetComponentField(ctx, e, kNameHash, " + fh
                     + ", v, 64) != 0;\n    }\n";
            } else {
                const std::string t = CppValueType(f.type);
                o += "\n    static bool Get" + acc + "(const MyeUpdateContext& ctx, MyeEntityId e, "
                     + t + "& out)\n    {\n        return MyeGetField(ctx, e, kNameHash, " + fh
                     + ", out);\n    }\n";
                o += "    static bool Set" + acc + "(const MyeUpdateContext& ctx, MyeEntityId e, "
                     "const " + t + "& v)\n    {\n        return MyeSetField(ctx, e, kNameHash, "
                     + fh + ", v);\n    }\n";
            }
        }
        // ---- 一括 (フィールドごとに ABI を往復する糖衣。false なら out は不定) ----
        o += "\n    static bool Read(const MyeUpdateContext& ctx, MyeEntityId e, " + sn
             + "& out)\n    {\n        return ";
        for (size_t i = 0; i < c.fields.size(); ++i) {
            if (i > 0) {
                o += "\n            && ";
            }
            o += "Get" + Capitalize(c.fields[i].name) + "(ctx, e, out." + c.fields[i].name + ")";
        }
        o += ";\n    }\n";
        o += "    static bool Write(const MyeUpdateContext& ctx, MyeEntityId e, const " + sn
             + "& v)\n    {\n        return ";
        for (size_t i = 0; i < c.fields.size(); ++i) {
            if (i > 0) {
                o += "\n            && ";
            }
            o += "Set" + Capitalize(c.fields[i].name) + "(ctx, e, v." + c.fields[i].name + ")";
        }
        o += ";\n    }\n";
        o += "};\n";
        // ---- 検査 (レイアウトのズレ / FNV 再掲のズレはプロジェクトのコンパイルで止まる) ----
        o += "static_assert(sizeof(" + sn + ") == " + U32(c.size)
             + ", \"layout mirror drifted from the runtime layout\");\n";
        for (const CodegenField& f : c.fields) {
            o += "static_assert(offsetof(" + sn + ", " + f.name + ") == " + U32(f.offset)
                 + ", \"field offset drifted\");\n";
        }
        o += "static_assert(MyeNameHash(\"" + c.name + "\") == " + sn
             + "::kNameHash, \"FNV constants diverged (Shared vs engine)\");\n";
        for (const CodegenField& f : c.fields) {
            o += "static_assert(MyeNameHash(\"" + f.name + "\") == " + sn + "::FieldHash::"
                 + f.name + ", \"FNV constants diverged (Shared vs engine)\");\n";
        }
    }
    return o;
}

std::string EmitCSharpSource(const std::vector<CodegenComponent>& model)
{
    std::string o;
    o += "// 自動生成 (MyEngine スキーマ codegen、M50d)。手編集禁止 — 起動時 / Compile C# "
         "Scripts で上書きされる。\n"
         "// assets\\schemas\\*.component.schema.json の定数 + 型付きアクセサ "
         "(汎用フィールド ABI v11)。\n"
         "// どのクラスも MyeScript 非派生なのでコンポーネントとしては登録されない。\n"
         "using MyeScripting;\n";
    for (const CodegenComponent& c : model) {
        o += "\npublic static class " + c.name + "Schema\n{\n";
        o += "    public const ulong NameHash = " + Hex64(c.nameHash) + "UL; // \"" + c.name
             + "\"\n\n";
        o += "    public static class FieldHash\n    {\n";
        for (const CodegenField& f : c.fields) {
            o += "        public const ulong " + Capitalize(f.name) + " = " + Hex64(f.nameHash)
                 + "UL; // \"" + f.name + "\"\n";
        }
        o += "    }\n";
        for (const CodegenField& f : c.fields) {
            const std::string acc = Capitalize(f.name);
            const std::string fh = "FieldHash." + acc;
            if (IsStringType(f.type)) {
                o += "\n    public static bool Get" + acc
                     + "(MyeEntity e, out string v) => e.TryGetFieldString(NameHash, " + fh
                     + ", out v);\n";
                o += "    public static bool Set" + acc
                     + "(MyeEntity e, string v) => e.SetFieldString(NameHash, " + fh + ", v);\n";
            } else if (f.type == FieldType::Float4x4) {
                o += "\n    // " + f.name + " (Float4x4) は定数のみ — C# 糖衣は未提供 "
                     "(TryGetField<T> を自前の 64 バイト構造体で呼ぶこと)\n";
            } else {
                const std::string t = CsValueType(f.type);
                o += "\n    public static bool Get" + acc + "(MyeEntity e, out " + t
                     + " v) => e.TryGetField(NameHash, " + fh + ", out v);\n";
                o += "    public static bool Set" + acc + "(MyeEntity e, " + t
                     + " v) => e.SetField(NameHash, " + fh + ", v);\n";
            }
        }
        o += "}\n";
    }
    return o;
}

bool WriteIfChanged(const std::wstring& path, const std::string& content)
{
    // 同一内容なら触らない — FileWatcher / AssetDatabase の .meta 生成を空騒ぎさせない
    {
        std::ifstream in{ std::filesystem::path(path), std::ios::binary };
        if (in) {
            std::string prev((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
            if (prev == content) {
                return false;
            }
        }
    }
    std::ofstream out{ std::filesystem::path(path), std::ios::binary };
    if (!out) {
        MYE_LOG_ERROR("[schema codegen] cannot write %s", WideToUtf8(path).c_str());
        return false;
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return true;
}

void WriteCSharpBindings(const std::wstring& assetsRoot)
{
    const std::vector<CodegenComponent> model = BuildCodegenModel();
    const std::wstring dir = assetsRoot + L"\\scripts\\Generated";
    const std::wstring path = dir + L"\\Schema.gen.cs";
    std::error_code ec;
    if (model.empty() && !std::filesystem::exists(path, ec)) {
        return; // スキーマ無し + 既存ファイル無し = 何も作らない
    }
    std::filesystem::create_directories(dir, ec);
    if (WriteIfChanged(path, EmitCSharpSource(model))) {
        MYE_LOG_INFO("[schema codegen] wrote %s (%zu component(s))", WideToUtf8(path).c_str(),
                     model.size());
    }
}

} // namespace mye::schema
