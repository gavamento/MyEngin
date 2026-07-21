#pragma once
#include <string>

#include "Editor/Selection.h"
#include "Engine/Engine/EngineLoop.h"

namespace mye {

class UndoStack;
class AssetPreviewCache;

// Asset Browser (engine_spec.md 9 章、M11)。
// assets/ をフォルダツリー + ファイルグリッドで表示。テクスチャはサムネイル、
// 他は拡張子アイコン。ダブルクリックで OS 既定アプリで開く。
// .prefab.json はダブルクリックでシーンへインスタンス化 (M13)。
class AssetBrowserWindow {
public:
    bool open = true; // 閉じる / 再表示 (タブ [x] と Window メニューに連動)
    void OnImGui(EngineContext& ctx, Selection& selection, UndoStack& undo,
                 const std::string& externalEditorCmd, AssetPreviewCache& preview);

private:
    void DrawDirTree(const std::wstring& dir);
    void DoCreate(EngineContext& ctx, const std::string& externalEditorCmd); // 命名モーダルの確定処理

    std::wstring current_; // 表示中フォルダ (絶対パス)
    bool init_ = false;
    int pendingCreate_ = 0;     // Create モーダルで作る種別 (0=なし)
    bool requestModal_ = false; // 次フレームで命名モーダルを開く
    char createName_[96] = {};
};

} // namespace mye
