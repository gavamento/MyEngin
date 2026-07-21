#pragma once
#include <string>

#include "Engine/Engine/GameObject.h"

namespace mye {

class Scene;
class ShaderManager;
struct RenderResources;

// FBX ローダ (M24、ufbx ベース。ModelLoader (glTF) の兄弟)。
// ノード階層をエンティティ階層として再現し、メッシュのマテリアルパート毎に MeshRenderer を付ける。
// 座標系: ufbx で右手系 Y-up に正規化 → glTF と同じ Z 反転 + 巻き順反転で左手系 (エンジン標準) へ。
// 現状はスタティックメッシュ + マテリアル + 階層に対応 (スキン/アニメは将来)。
namespace FbxLoader {

// 失敗時は無効な GameObject を返す (エラーは Console ログへ)
GameObject Load(Scene& scene, RenderResources& resources, ShaderManager& shaders,
                const std::wstring& path);

// ホットリロード: エンティティは触らず、メッシュ / マテリアルを同じ AssetID のまま再構築する
bool ReloadMeshes(RenderResources& resources, ShaderManager& shaders, const std::wstring& path);

} // namespace FbxLoader
} // namespace mye
