#pragma once
#include <string>

#include "Engine/Engine/GameObject.h"

namespace mye {

class Scene;
class ShaderManager;
struct RenderResources;

// FBX ローダ (M24、ufbx ベース。ModelLoader (glTF) の兄弟)。
// ノード階層をエンティティ階層として再現し、メッシュのマテリアルパート毎に MeshRenderer を付ける。
// ジオメトリックトランスフォーム (pivot) は ufbx のヘルパーノード ("geo") として階層に現れる。
// 座標系: ufbx に左手系 (エンジン標準) Y-up への変換を任せる (target_axes + 巻き順の自動反転)。
// glTF ローダは cgltf に同等機能が無いため手動 Z 反転のままで、規約が分かれている点に注意。
// マテリアルはベースカラー / ノーマルマップ / 半透明 / metallic-roughness / emissive に対応。
// emissive は M46i でエンジン側の受け皿 (Material::emissiveIntensity) ができたので取り込む
// (スカラー強度なので発光色は baseColor に従う)。第 2 UV セットは受け皿が無く WARN のみ。
// 現状はスタティックメッシュ + マテリアル + 階層に対応 (スキン/アニメは将来)。
namespace FbxLoader {

// 失敗時は無効な GameObject を返す (エラーは Console ログへ)
GameObject Load(Scene& scene, RenderResources& resources, ShaderManager& shaders,
                const std::wstring& path);

// ホットリロード: エンティティは触らず、メッシュ / マテリアルを同じ AssetID のまま再構築する
bool ReloadMeshes(RenderResources& resources, ShaderManager& shaders, const std::wstring& path);

} // namespace FbxLoader
} // namespace mye
