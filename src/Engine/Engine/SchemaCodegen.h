#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Engine/Core/Reflection.h"

namespace mye::schema {

// スキーマの型付きコード生成 (M50d、ADR-011 の「型付きアクセサは M49+」の回収)。
//
// 入力は**実行時レジストリ** (RegisteredSchemaTypeIds → ComponentRegistry) — スキーマ
// JSON を再パースしない。offset/size はランタイムが実際に使っている値そのものなので、
// レイアウト計算の二重実装が原理的に存在しない (ズレの検査は生成物内の static_assert)。
//
// 生成物の契約:
//   - nameHash (FNV-1a、HashStr と同一定数) は焼いてよい / **TypeId は絶対に焼かない**
//     (登録順依存 — スキーマの増減で全部ずれる)
//   - 生成物はエンジンヘッダを include しない (Shared/ScriptAPI.h のみ = MyePartTag 前例)
//   - 「生成のみでは挙動不変」— 誰かが include / 参照して初めて意味を持つ。
//     レガシー起動 (replay_verify 経路) の安全条件はこれで保たれる

struct CodegenField {
    std::string name;
    uint64_t nameHash = 0;
    FieldType type = FieldType::Float;
    uint32_t offset = 0;
    uint32_t size = 0;
};

struct CodegenComponent {
    std::string name;
    uint64_t nameHash = 0;
    uint32_t size = 0;
    uint32_t align = 0;
    std::vector<CodegenField> fields;
};

// 登録済みスキーマ型からモデルを組む。名前が C/C# 識別子として不正な型、
// フィールドの nameHash / アクセサ名が衝突する型は**警告して丸ごと落とす**
// (半端に生成して静かに別フィールドを読むより、無い方が安全)
std::vector<CodegenComponent> BuildCodegenModel();

// C++ ヘッダ (<project>\cache\Generated\SchemaComponents.gen.h の中身)。
// レイアウトミラー struct + nameHash 定数 + 型付きアクセサ + static_assert 検査
std::string EmitCppHeader(const std::vector<CodegenComponent>& model);

// C# ソース (assets\scripts\Generated\Schema.gen.cs の中身)。
// 静的クラス + 定数 + 型付きアクセサ。MyeScript 非派生なので ScriptRuntime の
// 型登録には一切影響しない (コンパイルに混ざるだけ)
std::string EmitCSharpSource(const std::vector<CodegenComponent>& model);

// 内容が同一なら書かない (FileWatcher / .meta 生成の空騒ぎ防止)。戻り値 = 書き込んだか
bool WriteIfChanged(const std::wstring& path, const std::string& content);

// C# バインディングを assetsRoot\scripts\Generated\Schema.gen.cs へ生成する。
// スキーマ 0 件で既存ファイルも無ければ何も作らない (無関係なプロジェクトを汚さない)
void WriteCSharpBindings(const std::wstring& assetsRoot);

} // namespace mye::schema
