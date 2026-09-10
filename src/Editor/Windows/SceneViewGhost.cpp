// SceneView の分岐ゴースト (M72e)。SceneViewWindow.cpp が 1600 行を超えているので分けた。
//
// 分岐 (= いまライブではないレーン) の未来を、GhostTrack の tick ごとのワールド行列から
// **同じ tick の**半透明メッシュ (M72i、GhostMeshPass) として重ね描く。メッシュが引けない物は
// ワイヤ箱。加えて前後のトレイル (平行移動の折れ線) を引く。
// ★描くのは EditorLinePass / GhostMeshPass = SceneView の RT だけ。GameView / 撮影経路には
//   出ない = golden 不変。
// ★ゴーストは sim 状態ではない (GhostTrack.h)。ここは読むだけ
#include "Editor/Windows/SceneViewWindow.h"

#include <cstdint>

#include "Editor/Windows/TimelineWindow.h" // kBranchLaneColors (Timeline のレーン色と同じ表)
#include "Engine/Engine/Replay/GhostTrack.h"
#include "Engine/Engine/Replay/TimeTravel.h"
#include "Engine/Renderer/GpuResources.h"

using namespace DirectX;

namespace mye {
namespace {

// トレイルの範囲 (tick)。過去は短く、未来は長く — 「これからどう違うか」を見せたい
constexpr uint64_t kTrailBack = 60;
constexpr uint64_t kTrailAhead = 180;
constexpr uint32_t kMeshAlpha = 0x66u; // 半透明メッシュの不透明度 (0x66 = 40%)

uint32_t WithAlpha(uint32_t rgba, uint32_t alpha)
{
    return (rgba & 0xFFFFFF00u) | (alpha & 0xFFu);
}

} // namespace

void SceneViewWindow::BuildGhostOverlay(EngineContext& ctx)
{
    if (!showGhosts_ || ctx.timeTravel == nullptr || !ctx.timeTravel->Enabled()) {
        return;
    }
    const uint64_t now = ctx.tickIndex;
    for (const TimeTravelBranch& b : ctx.timeTravel->Branches()) {
        if (!b.ghostBaked || !b.ghostVisible || b.ghost.entities.empty()) {
            continue;
        }
        // 分岐点より前 / ゴーストの終端より後には比べる相手が無い
        if (now < b.ghost.firstTick || now > b.ghost.lastTick) {
            continue;
        }
        const uint32_t rgba = BranchLaneColor(b.id);
        const uint32_t past = WithAlpha(rgba, 0x70u);
        const uint32_t ahead = WithAlpha(rgba, 0xB0u);
        for (size_t i = 0; i < b.ghost.entities.size(); ++i) {
            const GhostEntityTrack& t = b.ghost.entities[i];
            if (t.keys.size() < 2) {
                continue; // 動いていない物は描かない (ライブと同じ場所に同じ箱が重なるだけ)
            }
            const GhostKey* k = b.ghost.KeyAt(i, now);
            if (k == nullptr || k->alive == 0) {
                continue;
            }
            // ---- 同じ tick のメッシュ (M72i: 半透明の実メッシュ)。無ければワイヤ箱 ----
            XMFLOAT4X4 world;
            GhostTrack::ToMatrix(*k, world);
            const Mesh* mesh = ctx.resources != nullptr ? ctx.resources->meshes.Get(t.mesh) : nullptr;
            if (mesh != nullptr) {
                ghostMesh_.Add(mesh, world, WithAlpha(rgba, kMeshAlpha));
            } else {
                XMFLOAT3 lo = { -0.5f, -0.5f, -0.5f };
                XMFLOAT3 hi = { 0.5f, 0.5f, 0.5f };
                const XMFLOAT3 center = { (lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f,
                                          (lo.z + hi.z) * 0.5f };
                const XMFLOAT3 half = { (hi.x - lo.x) * 0.5f, (hi.y - lo.y) * 0.5f,
                                        (hi.z - lo.z) * 0.5f };
                const XMMATRIX boxWorld =
                    XMMatrixTranslation(center.x, center.y, center.z) * XMLoadFloat4x4(&world);
                XMFLOAT4X4 bw;
                XMStoreFloat4x4(&bw, boxWorld);
                lines_.AddWireBox(bw, half, rgba, /*onTop*/ true);
            }

            // ---- トレイル: [now - back, now + ahead] の平行移動を結ぶ ----
            const uint64_t from = (now > kTrailBack) ? now - kTrailBack : 0;
            const uint64_t to = now + kTrailAhead;
            bool havePrev = false;
            XMFLOAT3 prev = {};
            for (const GhostKey& key : t.keys) {
                if (key.tick < from) {
                    continue;
                }
                if (key.tick > to) {
                    break;
                }
                if (key.alive == 0) {
                    havePrev = false; // 消えた先は結ばない
                    continue;
                }
                const XMFLOAT3 p = { key.m[9], key.m[10], key.m[11] };
                if (havePrev) {
                    lines_.AddLine(prev, p, key.tick <= now ? past : ahead);
                }
                prev = p;
                havePrev = true;
            }
        }
    }
}

} // namespace mye
