#pragma once
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "Engine/Core/EntityID.h"

namespace mye {

class Scene;
class ManagedHost;

// シーン JSON シリアライズ (engine_spec.md 8.3 / 10 章)。
// - リフレクションフィールド表で全登録コンポーネントを自動処理
// - エンティティは永続 fileId で識別 (保存時に未割り当てなら採番)
// - 親子関係は親の fileId で表現
// - Play モードのスナップショット/復元にも同じ経路を使う
// 注意: AssetRef はハッシュ値で保存される。ファイル由来アセットの
// パス→ID 解決は M3 の AssetManager が担う
namespace SceneSerializer {

// C# スクリプトコンポーネントのフィールドは managed インスタンスが保持するため、
// シーン保存/復元時にこの hook 経由で JSON 化/復元する。EngineLoop が起動時に設定する
// (null のときは C# フィールドは既定値のまま = 存在のみ保存)。
void SetManagedHost(ManagedHost* mh);

nlohmann::json SaveToJson(Scene& scene);
bool LoadFromJson(Scene& scene, const nlohmann::json& root); // 既存内容は破棄される

bool SaveToFile(Scene& scene, const std::wstring& path);
bool LoadFromFile(Scene& scene, const std::wstring& path);

// 実行中シーンへの差分適用 (engine_spec.md 8.3 — 外部エディタでの編集を反映)。
// fileId で照合し、既存エンティティは更新 / 新規は生成 / 消えたものは破棄する。
// fileId を持たないエンティティ (未保存の編集中オブジェクト) には触れない
bool ApplyDiff(Scene& scene, const nlohmann::json& root);

// ---- 単一エンティティ / サブツリーのシリアライズ (M8: Undo/Redo・コピペ・プレハブの基盤) ----
// SaveToJson の "entities" 要素と同じ形式 (fileId/name/parent/childIndex/components) を返す。
// EntityRef フィールドと親は fileId で保存されるため、別シーンや Undo 復元でも解決できる。
// e に fileId が無ければ採番する
nlohmann::json EntityToJson(Scene& scene, EntityID e);        // 単一エンティティ (オブジェクト)
nlohmann::json SubtreeToJson(Scene& scene, EntityID root);    // root + 子孫 (DFS、配列)

// entities (SaveToJson の entities と同形式の配列) をシーンへ適用する。
// fileId で照合し、無ければ生成・あれば更新 (シリアライズ対象コンポーネントは JSON に一致させる)。
// **ApplyDiff と違い incoming に無いエンティティは破棄しない** — Undo/Redo・コピペ・プレハブ展開用。
// 兄弟位置は childIndex で復元。EntityRef/親は fileId で再解決 (incoming 優先、次にシーン内検索)
bool ApplyPartial(Scene& scene, const nlohmann::json& entities);

// subtree (SubtreeToJson の出力配列) を新しい fileId 群で複製してシーンに生成する (複製/コピペ)。
// 集合内の parent / EntityRef 参照は新 fileId に付け替え、集合外への参照は維持する。
// 生成した「トップレベル」(集合内に親を持たない) エンティティの新 fileId を返す (複製後の選択に使う)
std::vector<uint64_t> CloneSubtree(Scene& scene, const nlohmann::json& subtree);

} // namespace SceneSerializer
} // namespace mye
