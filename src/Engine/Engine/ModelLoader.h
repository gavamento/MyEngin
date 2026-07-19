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

} // namespace ModelLoader
} // namespace mye
