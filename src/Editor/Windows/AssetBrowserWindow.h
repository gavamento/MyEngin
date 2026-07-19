#pragma once
#include <string>

#include "Engine/Engine/EngineLoop.h"

namespace mye {

// Asset Browser (engine_spec.md 9 章、M11)。
// assets/ をフォルダツリー + ファイルグリッドで表示。テクスチャはサムネイル、
// 他は拡張子アイコン。ダブルクリックで OS 既定アプリで開く。
class AssetBrowserWindow {
public:
    void OnImGui(EngineContext& ctx);

private:
    void DrawDirTree(const std::wstring& dir);

    std::wstring current_; // 表示中フォルダ (絶対パス)
    bool init_ = false;
};

} // namespace mye
