//====================================================================================
//                          StartScene.h
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          起動シーンの用意 (Runtime / ヘッドレス共通)
//====================================================================================
#pragma once
#include <string>

#include "Engine/Engine/App/Project.h" // ProjectStartContent
#include "Engine/Engine/Demo/ShowcaseScenes.h"

namespace mye {

struct EngineContext;

// デモ用の内容 (デモ材質の登録とデモシーンの生成) を使うかを決める入力
struct DemoContentQuery {
    bool hasProject = false;        // false = プロジェクト指定なしの起動 (リポジトリ直下のデモ・テスト)
    bool showcaseRequested = false; // --*-demo の指定あり
    ProjectStartContent startContent = ProjectStartContent::Unspecified;
    bool sceneExists = false;       // これから開くシーンファイルがあるか
    bool demoTextureExists = false; // DemoGroundTextureExists の結果
};

// 判定の本体 (ファイルには触れない)。
// Unspecified は「開くシーンもデモの地面テクスチャも無い」ときだけ false。
// 保存済みシーンはデモ材質を名前で参照しうるので、シーンがあれば登録を続ける
bool UsesDemoContent(const DemoContentQuery& query);

// ctx と開くシーンのパスから入力を集めて UsesDemoContent を返す。Editor / Runtime 共用
bool ProjectUsesDemoContent(const EngineContext& ctx, const std::wstring& scenePath,
                            bool showcaseRequested);

// カメラとライトだけの開始シーンを組む (デモを使わないプロジェクトでシーンが無いとき)
void BuildEmptyStartScene(EngineContext& ctx);

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
