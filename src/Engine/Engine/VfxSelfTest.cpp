#include "Engine/Engine/VfxSelfTest.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/RenderSystem.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/SceneSerializer.h"
#include "Engine/Engine/Vfx/VfxRenderer.h"
#include "Engine/Renderer/PostProcess.h"

using namespace DirectX;

namespace mye {
namespace {

// WorldMatrix の平行移動成分を直接書く (TransformSystem 不要のヘッドレステスト用)
void SetWorldPos(World& world, EntityID e, float x, float y, float z)
{
    auto* wm = world.GetComponent<WorldMatrixComponent>(e);
    wm->value._41 = x;
    wm->value._42 = y;
    wm->value._43 = z;
}

} // namespace

bool RunVfxSelfTest()
{
    MYE_LOG_INFO("==== Vfx self test ====");
    RegisterBuiltinComponents();
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- (1) TrailStore: 点の追加 / minVertexDistance / 寿命 / 容量 / 同期 ----
    {
        Scene s;
        GameObject go = s.CreateGameObjectTracked("Trail");
        auto* tr = go.AddComponent<TrailRendererComponent>();
        tr->duration = 0.1f; // 6 tick で失効
        tr->minVertexDistance = 0.05f;
        s.GetWorld().ApplyStructuralChanges();
        World& w = s.GetWorld();
        tr = go.GetComponent<TrailRendererComponent>();

        TrailStore ts;
        SetWorldPos(w, go.Id(), 0, 0, 0);
        ts.Update(w, 0);
        check(ts.Buffers().size() == 1 && ts.Buffers()[0].pts.size() == 1,
              "trail: first update adds one point");

        SetWorldPos(w, go.Id(), 0.01f, 0, 0); // 0.05 未満 → 追加なし
        ts.Update(w, 1);
        check(ts.Buffers()[0].pts.size() == 1, "trail: sub-threshold move adds nothing");

        SetWorldPos(w, go.Id(), 0.2f, 0, 0); // 0.05 以上 → 追加
        ts.Update(w, 2);
        check(ts.Buffers()[0].pts.size() == 2, "trail: move beyond threshold adds a point");

        // 寿命失効: tick 0/2 の点は tick 10 で両方 6 tick 超え。移動なし → 新規追加なし…
        // ではなく点が空になったら次の emitting 追加が入る (仕様)。emitting を切って検証
        tr->emitting = 0;
        ts.Update(w, 10);
        check(ts.Buffers()[0].pts.empty(), "trail: points expire after duration");

        // emitting=0 では追加されない
        SetWorldPos(w, go.Id(), 1.0f, 0, 0);
        ts.Update(w, 11);
        check(ts.Buffers()[0].pts.empty(), "trail: emitting=0 adds nothing");

        // 容量上限: 毎 tick 閾値以上動かして 300 回 → kMaxPoints で頭打ち
        tr->emitting = 1;
        tr->duration = 30.0f; // 失効させない
        for (int i = 0; i < 300; ++i) {
            SetWorldPos(w, go.Id(), 1.0f + 0.1f * i, 0, 0);
            ts.Update(w, 20 + i);
        }
        check(ts.Buffers()[0].pts.size() == static_cast<size_t>(TrailStore::kMaxPoints),
              "trail: point count capped at kMaxPoints");

        // エンティティ破棄 → バッファ消滅
        w.DestroyEntity(go.Id());
        w.ApplyStructuralChanges();
        ts.Update(w, 400);
        check(ts.Buffers().empty(), "trail: destroyed entity buffer is removed");
    }

    // ---- (2) BuildTextQuadsLocal: 頂点数 / 中央揃え / 色 ----
    {
        VfxGlyph glyphs[128] = {};
        for (unsigned char c : { 'A', 'B' }) {
            VfxGlyph& g = glyphs[c];
            g.u0 = 0.0f; g.v0 = 0.0f; g.u1 = 0.5f; g.v1 = 0.5f;
            g.w = 8.0f; g.h = 8.0f; g.advance = 8.0f;
            g.valid = true;
        }
        std::vector<VfxVertex> out;
        const XMFLOAT4 col = { 0.2f, 0.4f, 0.6f, 0.8f };
        const int n = vfx::BuildTextQuadsLocal("AB", glyphs, 1.0f, col, out);
        check(n == 12 && out.size() == 12, "text: 2 glyphs -> 12 vertices");
        float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
        bool colOk = true;
        for (const VfxVertex& v : out) {
            minX = (std::min)(minX, v.pos.x);
            maxX = (std::max)(maxX, v.pos.x);
            minY = (std::min)(minY, v.pos.y);
            maxY = (std::max)(maxY, v.pos.y);
            if (v.color.x != col.x || v.color.w != col.w) {
                colOk = false;
            }
        }
        // 総幅 16px × kVfxWorldPerPx、中央揃え → ±半分で対称
        const float half = 16.0f * kVfxWorldPerPx * 0.5f;
        check(std::fabs(minX + half) < 1e-4f && std::fabs(maxX - half) < 1e-4f
                  && std::fabs(minY + maxY) < 1e-4f,
              "text: quads centered at origin");
        check(colOk, "text: vertex color passthrough");
        // 無効文字のみ → 0 頂点
        std::vector<VfxVertex> none;
        check(vfx::BuildTextQuadsLocal("\t\x7f", glyphs, 1.0f, col, none) == 0 && none.empty(),
              "text: invalid-only string builds nothing");
    }

    // ---- (3) BuildTrailRibbon: 頂点数 / 幅テーパ ----
    {
        // X 軸に並ぶ 3 点 (tick 0/5/10)、now=10、寿命 10 tick
        const vfx::TrailPoint pts[3] = {
            { { 0, 0, 0 }, 0 },  // 最古 (age 1.0 → 幅 0)
            { { 1, 0, 0 }, 5 },  // age 0.5
            { { 2, 0, 0 }, 10 }, // 最新 (age 0 → 幅 max)
        };
        std::vector<VfxVertex> out;
        const XMFLOAT4 c0 = { 1, 1, 1, 1 };
        const XMFLOAT4 c1 = { 1, 1, 1, 0 };
        const int n = vfx::BuildTrailRibbon(pts, 3, 10, 10.0f, 1.0f, c0, c1,
                                            { 0.0f, 5.0f, 0.0f }, out);
        check(n == 12 && out.size() == 12, "ribbon: 3 points -> 2 segments -> 12 vertices");
        // 最新端 (x=2) の頂点は z 方向 (progress×toCam の cross) に ±0.5、最古端 (x=0) は幅 0
        float newestSpread = 0.0f, oldestSpread = 0.0f;
        for (const VfxVertex& v : out) {
            if (std::fabs(v.pos.x - 2.0f) < 1e-4f) {
                newestSpread = (std::max)(newestSpread, std::fabs(v.pos.z));
            }
            if (std::fabs(v.pos.x - 0.0f) < 1e-4f) {
                oldestSpread = (std::max)(oldestSpread, std::fabs(v.pos.z));
            }
        }
        check(std::fabs(newestSpread - 0.5f) < 1e-3f && oldestSpread < 1e-3f,
              "ribbon: width tapers from newest (w/2) to oldest (0)");
        // 2 点未満 → 0
        std::vector<VfxVertex> none;
        check(vfx::BuildTrailRibbon(pts, 1, 10, 10.0f, 1.0f, c0, c1, { 0, 5, 0 }, none) == 0,
              "ribbon: fewer than 2 points builds nothing");
    }

    // ---- (3.5) CollectEnvironment (M29d): 最初の active な Skybox/Fog が選ばれる ----
    {
        Scene s;
        GameObject sky1 = s.CreateGameObjectTracked("Sky1"); // index が小さい方が勝つ
        sky1.AddComponent<SkyboxComponent>()->topColor = { 0.1f, 0.2f, 0.3f, 1.0f };
        GameObject sky2 = s.CreateGameObjectTracked("Sky2");
        sky2.AddComponent<SkyboxComponent>()->topColor = { 0.9f, 0.9f, 0.9f, 1.0f };
        GameObject fog1 = s.CreateGameObjectTracked("Fog1");
        {
            auto* f = fog1.AddComponent<FogComponent>();
            f->mode = 1;
            f->density = 0.5f;
        }
        // 無効化された Skybox は飛ばされる (sky1 を無効化 → sky2 が選ばれる) 検証用
        s.GetWorld().ApplyStructuralChanges();

        RenderView view;
        CollectEnvironment(s.GetWorld(), view);
        check(view.skyMode == 0 && std::fabs(view.skyTop.x - 0.1f) < 1e-6f,
              "environment: first skybox (lowest index) is selected");
        check(view.fogMode == 1 && std::fabs(view.fogDensity - 0.5f) < 1e-6f,
              "environment: fog settings propagate to view");

        sky1.AddComponent<ActiveComponent>()->enabled = 0;
        s.GetWorld().ApplyStructuralChanges();
        RenderView view2;
        CollectEnvironment(s.GetWorld(), view2);
        check(view2.skyMode == 0 && std::fabs(view2.skyTop.x - 0.9f) < 1e-6f,
              "environment: inactive skybox is skipped");

        Scene empty;
        RenderView view3;
        CollectEnvironment(empty.GetWorld(), view3);
        check(view3.skyMode == -1 && view3.fogMode == -1,
              "environment: empty scene leaves sky/fog disabled");
    }

    // ---- (3.6) MergeCameraPostFx (M29e): 上書きマージと applyGamma 維持 ----
    {
        PostProcess::Settings base;
        base.exposure = 2.0f;
        base.applyGamma = true; // component は持たない → base 維持されること
        CameraPostFxComponent comp;
        comp.exposure = 3.0f;
        comp.tonemapMode = 2;
        comp.bloomOn = 0;
        comp.bloomThreshold = 0.5f;
        comp.bloomIntensity = 0.9f;
        comp.fxaaOn = 0;
        const PostProcess::Settings m = MergeCameraPostFx(base, comp);
        check(m.exposure == 3.0f && m.tonemap == 2 && !m.bloom && m.bloomThreshold == 0.5f
                  && m.bloomIntensity == 0.9f && !m.fxaa && m.applyGamma,
              "postfx merge: component overrides base, applyGamma preserved");
        // 範囲外は既定へクランプ
        comp.tonemapMode = 99;
        comp.exposure = -5.0f;
        const PostProcess::Settings c = MergeCameraPostFx(base, comp);
        check(c.tonemap == 1 && c.exposure == 0.0f,
              "postfx merge: out-of-range values are clamped");
    }

    // ---- (4) BillboardBasis: 各モードの基底 ----
    {
        XMFLOAT4X4 wm = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        const XMFLOAT3 camR = { 0.6f, 0, 0.8f };
        const XMFLOAT3 camU = { 0, 1, 0 };
        XMFLOAT3 r, u;
        // mode 0: カメラ基底そのまま
        vfx::BillboardBasis(0, wm, camR, camU, { 0, 0, -10.0f }, r, u);
        check(r.x == camR.x && r.z == camR.z && u.y == 1.0f, "billboard: mode0 uses camera basis");
        // mode 1: カメラ (0,0,-10)、スプライト原点 → right=(1,0,0) up=(0,1,0)
        vfx::BillboardBasis(1, wm, camR, camU, { 0, 0, -10.0f }, r, u);
        check(std::fabs(r.x - 1.0f) < 1e-4f && std::fabs(r.y) < 1e-4f && std::fabs(r.z) < 1e-4f
                  && u.y == 1.0f,
              "billboard: mode1 (Y-axis) faces camera with world up");
        // mode 2: ワールド基底 (スケール 2 を正規化)
        wm._11 = 2.0f; wm._22 = 2.0f;
        vfx::BillboardBasis(2, wm, camR, camU, { 0, 0, -10.0f }, r, u);
        check(std::fabs(r.x - 1.0f) < 1e-4f && std::fabs(u.y - 1.0f) < 1e-4f,
              "billboard: mode2 uses normalized world basis");
    }

    // ---- (5) M29 全 9 コンポーネントのシーン JSON round-trip ----
    // 登録 (FieldDesc) だけで serializer が面倒を見る設計の回帰テスト。
    // 特に Float2 (SpriteRenderer.size) は M29c が初使用なので必ず往復させる
    {
        Scene src;
        GameObject a = src.CreateGameObjectTracked("A");
        a.AddComponent<ConstantForceComponent>()->force = { 1.0f, 2.0f, 3.0f };
        GameObject b = src.CreateGameObjectTracked("B");
        {
            auto* sj = b.AddComponent<SpringJointComponent>();
            sj->connectedEntity = a.Id(); // EntityRef → fileId 保存
            sj->stiffness = 123.0f;
        }
        b.AddComponent<CharacterControllerComponent>()->radius = 0.45f;
        {
            auto* sp = b.AddComponent<SpriteRendererComponent>();
            sp->size = { 2.5f, 0.75f }; // Float2 往復
            sp->billboardMode = 1;
        }
        b.AddComponent<TrailRendererComponent>()->width = 0.33f;
        {
            auto* tm = b.AddComponent<TextMeshComponent>();
            snprintf(tm->text, sizeof(tm->text), "RT");
            tm->fontScale = 2.0f;
        }
        GameObject c = src.CreateGameObjectTracked("C");
        c.AddComponent<SkyboxComponent>()->topColor = { 0.5f, 0.6f, 0.7f, 1.0f };
        c.AddComponent<FogComponent>()->density = 0.125f;
        c.AddComponent<CameraPostFxComponent>()->exposure = 4.0f;
        src.GetWorld().ApplyStructuralChanges();

        const nlohmann::json j = SceneSerializer::SaveToJson(src);
        Scene dst;
        const bool loaded = SceneSerializer::LoadFromJson(dst, j);
        GameObject la = dst.Find("A");
        GameObject lb = dst.Find("B");
        GameObject lc = dst.Find("C");
        bool ok = loaded && la && lb && lc;
        if (ok) {
            const auto* cf = la.GetComponent<ConstantForceComponent>();
            const auto* sj = lb.GetComponent<SpringJointComponent>();
            const auto* cc = lb.GetComponent<CharacterControllerComponent>();
            const auto* sp = lb.GetComponent<SpriteRendererComponent>();
            const auto* tr = lb.GetComponent<TrailRendererComponent>();
            const auto* tm = lb.GetComponent<TextMeshComponent>();
            const auto* sb = lc.GetComponent<SkyboxComponent>();
            const auto* fg = lc.GetComponent<FogComponent>();
            const auto* px = lc.GetComponent<CameraPostFxComponent>();
            ok = cf && cf->force.y == 2.0f && sj && sj->stiffness == 123.0f
                 && sj->connectedEntity == la.Id() // fileId 経由で再解決される
                 && cc && cc->radius == 0.45f && sp && sp->size.x == 2.5f && sp->size.y == 0.75f
                 && sp->billboardMode == 1 && tr && tr->width == 0.33f && tm
                 && std::strcmp(tm->text, "RT") == 0 && tm->fontScale == 2.0f && sb
                 && sb->topColor.y == 0.6f && fg && fg->density == 0.125f && px
                 && px->exposure == 4.0f;
        }
        check(ok, "roundtrip: all 9 M29 components survive scene JSON save/load");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Vfx self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Vfx self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
