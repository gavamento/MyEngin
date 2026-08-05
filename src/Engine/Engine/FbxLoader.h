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

// M50a: 一括ヘッドレス登録 — メッシュ / マテリアル / スキン / クリップをエンティティを
// 作らずに `Load` とバイト同一のキーで登録する (詳細は ModelLoader::RegisterAssets と同じ)。
// logErrors=false は起動全走査用 (壊れたファイルでも黙って諦める)
bool RegisterAssets(RenderResources& resources, ShaderManager& shaders, const std::wstring& path,
                    bool logErrors);

// ホットリロード: エンティティは触らず、メッシュ / マテリアルを同じ AssetID のまま再構築する。
// 実体は RegisterAssets + リロードログ
bool ReloadMeshes(RenderResources& resources, ShaderManager& shaders, const std::wstring& path);

// M48g: スケルトン (SkinnedModel) だけを**エンティティも GPU バッファも作らずに**登録する。
// 詳細は ModelLoader::RegisterSkinnedModels と同じ (保存済みシーンのロード経路の穴埋め)。
// キーは Load / ReloadMeshes と厳密に同じ (`パス#mesh<id>#skin<id>`)
size_t RegisterSkinnedModels(RenderResources& resources, const std::wstring& path);

} // namespace FbxLoader
} // namespace mye
