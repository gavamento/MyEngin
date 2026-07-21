#pragma once
#include <string>

#include "Editor/PlayModeController.h"
#include "Engine/Core/Log.h"

namespace mye {

struct EngineContext;

// 画面最下部の固定ステータスバー (M27b、Unity 最下部相当)。
// 左 = 最新ログ 1 行 (クリックで Console を開く)、右 = シーン/エンティティ数/FPS/Play 状態。
// BeginViewportSideBar で WorkArea を確保するため、DockSpaceOverViewport より前に呼ぶこと
class StatusBar {
public:
    void OnImGui(EngineContext& ctx, const std::string& projectName, const std::wstring& scenePath,
                 bool dirty, PlayState state, bool* consoleOpen);

private:
    uint64_t logCursor_ = 0; // Console とは独立したリーダー (ReadSince は複数リーダー可)
    LogEntry last_ = {};
    bool hasLast_ = false;
};

} // namespace mye
