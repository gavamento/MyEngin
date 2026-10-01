//====================================================================================
//                          StartScene.h
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          起動シーンの用意 (Runtime / ヘッドレス共通)
//====================================================================================
#pragma once
#include <string>

#include "Engine/Engine/Demo/ShowcaseScenes.h"

namespace mye {

struct EngineContext;

struct StartSceneOptions {
    std::wstring scenePath;                  // --scene。空 = ショーケース、無ければ main.scene.json
    const ShowcaseDef* showcase = nullptr;   // --*-demo (表の行)
    ShowcaseOptions showcaseOptions;
    // Editor の OnStart と同じくロード前に showcase->prepare を呼ぶ (--flow-demo の 2 シーンを作る)。
    // Runtime は呼ばない (flow は editorOnly で Runtime には来ない)
    bool runShowcasePrepare = false;
};

// 起動シーンを組み、Editor の Play と同じ Save+Load で EntityID を正規化する。
// Editor で録った .rep と初期状態を揃えるための手順で、Runtime とヘッドレスが共有する。
// 戻り値は実際に使ったシーンのパス
std::wstring PrepareStartScene(EngineContext& ctx, const StartSceneOptions& options);

} // namespace mye
