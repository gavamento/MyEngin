#pragma once
#include <functional>

#include "Engine/Core/EntityID.h"
#include "Engine/Engine/GameObject.h"

namespace mye {

struct EngineContext;
struct Selection;
class UndoStack;

// 組み込みプリミティブ / オブジェクトのファクトリ。生成した GameObject を返す
// (Undo・選択・親子付けは呼び出し側 or DrawCreateMenuItems が行う)。
GameObject CreateEmpty(EngineContext& ctx, const char* name);
GameObject CreateCube(EngineContext& ctx, const char* name);
GameObject CreateSphere(EngineContext& ctx, const char* name);
GameObject CreatePlane(EngineContext& ctx, const char* name);
GameObject CreateQuad(EngineContext& ctx, const char* name);
GameObject CreateCylinder(EngineContext& ctx, const char* name);
GameObject CreateCapsule(EngineContext& ctx, const char* name);
GameObject CreateDirectionalLight(EngineContext& ctx, const char* name);
GameObject CreatePointLight(EngineContext& ctx, const char* name);
GameObject CreateSpotLight(EngineContext& ctx, const char* name);
GameObject CreateCamera(EngineContext& ctx, const char* name);

// 生成操作を 1 つの Undo エントリとして記録し、生成物を選択する。
GameObject RecordCreate(EngineContext& ctx, Selection& selection, UndoStack& undo, const char* label,
                        const std::function<GameObject()>& make);

// Unity 風の生成メニュー項目一式を発行する (呼び出し側の BeginPopup / BeginMenu 内で呼ぶ)。
// parent が非 null なら生成物をその子にする。
void DrawCreateMenuItems(EngineContext& ctx, Selection& selection, UndoStack& undo,
                         EntityID parent = kNullEntity);

} // namespace mye
