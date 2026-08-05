#pragma once
#include <string>

#include "Engine/Engine/GameObject.h"

namespace mye {

class Scene;
class ShaderManager;
struct RenderResources;

// glTF 2.0 (.glb / .gltf) ローダ (engine_spec.md 10 章)。
// ノード階層をエンティティ階層として再現し、プリミティブ毎に MeshRenderer を付ける。
// 座標系: glTF は右手系 — Z 反転 + 巻き順反転で左手系 (エンジン標準) に変換する
namespace ModelLoader {

// 失敗時は無効な GameObject を返す (エラーは Console ログへ)
GameObject Load(Scene& scene, RenderResources& resources, ShaderManager& shaders,
                const std::wstring& path);

// M50a: 一括ヘッドレス登録 — メッシュ / マテリアル / スキンをエンティティを作らずに
// `Load` とバイト同一のキーで登録する。保存済みシーンをロードする経路は `Load` を通らず、
// M48g のスケルトンだけでは MeshRenderer.mesh / .material の実体が誰にも登録されず
// モデルが描画されなかった (起動走査はこれを全モデルに呼ぶ)。
// logErrors=false は起動全走査用 (壊れた / 非モデルのファイルでも黙って諦める)
bool RegisterAssets(RenderResources& resources, ShaderManager& shaders, const std::wstring& path,
                    bool logErrors);

// M3 ホットリロード: エンティティは触らず、メッシュ / マテリアル / 埋め込みテクスチャを
// 同じ AssetID のまま再構築する (参照は AssetID 経由なので差し替えが透過になる)。
// 実体は RegisterAssets + リロードログ
bool ReloadMeshes(RenderResources& resources, ShaderManager& shaders, const std::wstring& path);

// M48g: スケルトン (SkinnedModel) だけを**エンティティを作らずに**登録する。
// 保存済みシーンをロードする経路では `Load` を通らないため、SkinnedMesh.model が指す
// AssetID が誰にも登録されずポーズ評価が丸ごと落ちていた (骨追従・ボーンパレット双方)。
// キーは `Load` と厳密に同じ (`パス#skin<index>`) — ずれると別物として二重登録される。
// 返り値 = 登録した skin の数 (パース失敗は 0)
size_t RegisterSkinnedModels(RenderResources& resources, const std::wstring& path);

} // namespace ModelLoader
} // namespace mye
