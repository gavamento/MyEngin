#include "Engine/Engine/ShowcaseScenes.h"

#include "Engine/Engine/DemoContent.h"

namespace mye {
namespace {

// 値を取らない組み立て関数を表の型へ合わせる
template <void (*Build)(EngineContext&)>
void BuildPlain(EngineContext& ctx, const ShowcaseOptions&)
{
    Build(ctx);
}

void BuildTerrain(EngineContext& ctx, const ShowcaseOptions& options)
{
    BuildTerrainShowcaseScene(ctx, options.terrainLodDistance, options.terrainSkirtDepth);
}

// ★上の行ほど優先 (--*-demo を複数渡したとき)。並びは Editor の従来の判定順。
// ★cache\ の行はコードから毎回組む。保存先を専用にしてあるのは、万一 Ctrl+S されても既定デモシーン
//   (main.scene.json = golden.rep の入力) を潰さないため。保存済みが残っているとロードする側へ落ちるので、
//   shot_verify / replay_verify は撮影・記録の前に消している
const ShowcaseDef kShowcases[] = {
    // M46i: コーネル箱。ブートシーンと別枠で、保存済みがあればそれを読み、無ければコードから組む
    { L"--rt-demo", true, L"scenes\\rt_showcase.scene.json", nullptr, &BuildPlain<&BuildRtShowcaseScene>, false },
    // M48g: 部位追従のリプレイ被覆シーン。版管理された唯一の正解は BuildPartsShowcaseScene (コード) 側
    { L"--parts-demo", false, L"cache\\parts_showcase.scene.json", nullptr, &BuildPlain<&BuildPartsShowcaseScene>,
      true },
    // M51j: ゲームフロー統合デモのタイトル。ファイルは EnsureFlowShowcaseScenes が assets\scenes\ に作る —
    // タイトル⇄ゲームの遷移が LoadScene("scenes/flow_*.scene.json") = assets 相対解決なので cache\ には置けない
    { L"--flow-demo", true, L"scenes\\flow_title.scene.json", &EnsureFlowShowcaseScenes, nullptr, true },
    { L"--local-demo", false, L"cache\\local_players.scene.json", nullptr, &BuildPlain<&BuildLocalPlayersScene>,
      false }, // M52g
    { L"--net-demo", false, L"cache\\net_duel.scene.json", nullptr, &BuildPlain<&BuildNetDuelScene>, false }, // M52i
    { L"--render-demo", false, L"cache\\render_showcase.scene.json", nullptr, &BuildPlain<&BuildRenderShowcaseScene>,
      false }, // M54a
    { L"--terrain-demo", false, L"cache\\terrain_showcase.scene.json", nullptr, &BuildTerrain, false }, // M58c / M58e
    { L"--physics-demo", false, L"cache\\physics_showcase.scene.json", nullptr,
      &BuildPlain<&BuildPhysicsShowcaseScene>, false }, // M59d
    { L"--joint-demo", false, L"cache\\joint_showcase.scene.json", nullptr, &BuildPlain<&BuildJointShowcaseScene>,
      false }, // M60i
    { L"--fog-demo", false, L"cache\\fog_showcase.scene.json", nullptr, &BuildPlain<&BuildFogShowcaseScene>,
      false }, // M57追補
    { L"--particle-demo", false, L"cache\\particle_showcase.scene.json", nullptr,
      &BuildPlain<&BuildParticleShowcaseScene>, false }, // M63a
    { L"--acoustic-demo", false, L"cache\\acoustic_showcase.scene.json", nullptr,
      &BuildPlain<&BuildAcousticShowcaseScene>, false }, // M65b
    { L"--ui-demo", false, L"cache\\ui_showcase.scene.json", nullptr, &BuildPlain<&BuildUiShowcaseScene>,
      false }, // M75c
};

} // namespace

const ShowcaseDef* FindShowcase(const std::wstring& flag, bool editor)
{
    for (const ShowcaseDef& s : kShowcases) {
        if (flag == s.flag && (editor || !s.editorOnly)) {
            return &s;
        }
    }
    return nullptr;
}

const ShowcaseDef* PickShowcase(const ShowcaseDef* current, const ShowcaseDef* candidate)
{
    if (current == nullptr) {
        return candidate;
    }
    if (candidate == nullptr) {
        return current;
    }
    return (candidate < current) ? candidate : current; // どちらも kShowcases の中 = 位置が上のほうが勝つ
}

std::wstring ShowcaseScenePath(const ShowcaseDef& showcase, const std::wstring& assetsRoot)
{
    return showcase.inAssets ? assetsRoot + L"\\" + showcase.scenePath : std::wstring(showcase.scenePath);
}

} // namespace mye
