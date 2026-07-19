#pragma once
#include <string>

#include "nlohmann/json.hpp"

namespace mye {

class Scene;

// シーン JSON シリアライズ (engine_spec.md 8.3 / 10 章)。
// - リフレクションフィールド表で全登録コンポーネントを自動処理
// - エンティティは永続 fileId で識別 (保存時に未割り当てなら採番)
// - 親子関係は親の fileId で表現
// - Play モードのスナップショット/復元にも同じ経路を使う
// 注意: AssetRef はハッシュ値で保存される。ファイル由来アセットの
// パス→ID 解決は M3 の AssetManager が担う
namespace SceneSerializer {

nlohmann::json SaveToJson(Scene& scene);
bool LoadFromJson(Scene& scene, const nlohmann::json& root); // 既存内容は破棄される

bool SaveToFile(Scene& scene, const std::wstring& path);
bool LoadFromFile(Scene& scene, const std::wstring& path);

// 実行中シーンへの差分適用 (engine_spec.md 8.3 — 外部エディタでの編集を反映)。
// fileId で照合し、既存エンティティは更新 / 新規は生成 / 消えたものは破棄する。
// fileId を持たないエンティティ (未保存の編集中オブジェクト) には触れない
bool ApplyDiff(Scene& scene, const nlohmann::json& root);

} // namespace SceneSerializer
} // namespace mye
