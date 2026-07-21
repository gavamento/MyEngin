#pragma once
#include <cstdint>
#include <string>

#include <DirectXMath.h>

namespace mye {

struct EngineContext;
struct Selection;
class UndoStack;

// AssetBrowser のドラッグ&ドロップ用ペイロード名。データは UTF-8 のファイルパス (null 終端)。
inline constexpr const char* kAssetDragPayload = "MYE_ASSET";

// ---- アセット新規作成 (AssetBrowser の右クリック Create) ----
bool CreateFolderAsset(const std::wstring& dir, const std::string& name);
std::wstring CreateSceneAsset(const std::wstring& dir, const std::string& name);   // .scene.json (空)
std::wstring CreateAnimationAsset(EngineContext& ctx, const std::wstring& dir,
                                  const std::string& name);                        // .anim.json
std::wstring CreateMaterialAsset(EngineContext& ctx, const std::wstring& dir,
                                 const std::string& name);                         // .mat.json
std::wstring CreateCppScript(EngineContext& ctx, const std::string& name); // src\GameLogic\Scripts\<name>.cpp
std::wstring CreateCSharpScript(EngineContext& ctx, const std::string& name); // assets\scripts\<name>.cs

// ---- アセット配置 (ドラッグ&ドロップ) ----
// path が .prefab.json ならインスタンス化、.glb/.gltf ならモデルロード。1 Undo エントリ + 自動選択。
// pos 非 null でその位置に、parentFileId 非 0 でその子に配置。
void InstantiateAssetAtPath(EngineContext& ctx, Selection& selection, UndoStack& undo,
                            const std::wstring& path, const DirectX::XMFLOAT3* pos,
                            uint64_t parentFileId);

// ---- スクリプトワークフロー ----
void OpenInExternalEditor(const std::string& editorCmd, const std::wstring& path); // {file}/{line} 置換
void RebuildGameLogic(EngineContext& ctx); // tools\build_scripts.bat 起動 (gen + msbuild GameLogic)
void CompileCSharpScripts(EngineContext& ctx); // assets\scripts\*.cs をエンジン内 Roslyn でコンパイル

} // namespace mye
