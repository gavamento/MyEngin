#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mye {

// スキーマ由来の動的コンポーネント登録 (M48j)。
//
// `assets\schemas\*.component.schema.json` を読んで `ComponentRegistry` に型を足す。
// C++ の struct を書かずにゲーム固有データ (HP / 陣営 / ドロップ率…) を定義するための
// 入口で、登録さえ済めば **Inspector / シーン JSON / ワールドハッシュ / AddComponentByName は
// FieldDesc ジェネリックなので UI コードは 1 行も要らない**。
//
// 決定論の契約 (ここが崩れるとリプレイが壊れる):
//   - 登録順は **assets ルートからの相対パス昇順**に固定する。ディレクトリ走査順は
//     OS/ファイルシステム依存なので、必ず明示ソートしてから登録する (規則 7 と同じ思想)
//   - 呼ぶ位置は **組込みコンポーネント登録後・ScriptHost の DLL ロード前**の 1 箇所だけ。
//     この順序が固定されている限り、スキーマ型は「組込み群」と「スクリプト群」の間に
//     連続したブロックとして入るので、スクリプト型の TypeId は一様にずれるだけで
//     **エンティティ内の相対順は変わらない = 既存シーンのワールドハッシュは不変**
//   - 既に同名の型 (組込み / スクリプト / 別スキーマ) がある場合は**登録しない**。
//     ComponentRegistry::Register は nameHash が一致すると既存 TypeId を黙って返すので、
//     素通しするとサイズの違う別物に化ける (静かなデータ破壊)
//
// v1 の制限: ホットリロードなし (起動時 1 回)。スクリプトからフィールドを読み書きする
// 汎用 ABI も無い (型付きアクセサのコード生成は M49+)。v1 の用途は
// 「オーサリング + 保存 + ハッシュ被覆」まで。
namespace schema {

// 1 プロセスで登録できるスキーマ型の上限 (construct 関数ポインタのサムネイル数)
inline constexpr size_t kMaxSchemaComponents = 64;

// assetsRoot\schemas\**\*.component.schema.json を登録する。戻り値 = 登録できた型数。
// 冪等: 既に登録済みの名前は読み飛ばすので、二重呼び出しで型が増えることはない
size_t RegisterSchemaComponents(const std::wstring& assetsRoot);

// 上と同じだが走査対象ディレクトリを直接指定する (selftest 用)
size_t RegisterSchemaComponentsFrom(const std::wstring& schemaDir);

// このプロセスでスキーマから登録された型の TypeId (登録順 = 相対パス昇順)。
// codegen (SchemaCodegen) が「レジストリのどれがスキーマ型か」を知る唯一の入口
const std::vector<uint32_t>& RegisteredSchemaTypeIds();

} // namespace schema
} // namespace mye
