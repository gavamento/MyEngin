#pragma once
#include "Engine/Engine/EngineLoop.h"
#include "Engine/Engine/UI/UILayout.h" // ワールド追従 UI の射影コンテキスト
#include "Engine/Renderer/RenderTexture.h"

namespace mye {

struct Selection;

// ゲームカメラ (シーン内の CameraComponent) 視点の表示 (engine_spec.md 9 章)
class GameViewWindow {
public:
    bool open = true; // 閉じる / 再表示 (タブ [x] と Window メニューに連動)
    void OnRenderViews(EngineContext& ctx);
    void OnImGui(EngineContext& ctx, const Selection& selection); // M51f: 選択 UI の矩形表示
    // 直近の OnImGui で描いたゲーム画像の矩形 (メインウィンドウのクライアント px、2026-09-14)。
    // IEngineApp::GameMouseArea の中身。見えていない (閉じた / タブの裏) ときは w = h = 0
    InputRect GameArea() const { return gameArea_; }

private:
    InputRect gameArea_;
    RenderTexture rt_;
    int desiredW_ = 0;
    int desiredH_ = 0;
    bool hasCamera_ = false;
    // OnRenderViews で採ったワールド追従 UI の射影 (OnImGui のアウトラインが同フレームで読む)
    uilayout::UIWorldContext uiWc_;
    bool uiWcValid_ = false;
    int aspectMode_ = 0;    // 0=Free 1=16:9 2=4:3 3=1:1 (レターボックス)
    bool showStats_ = true; // 統計オーバーレイ (FPS/entities/tick)
};

} // namespace mye
