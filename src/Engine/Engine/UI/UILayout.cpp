#include "Engine/Engine/UI/UILayout.h"

#include <cmath>

#include "Engine/Core/ComponentRegistry.h" // kComponentScriptState (UI 専用判定)
#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/RenderSystem.h" // PrevWorldStore (描画補間 M36b)
#include "Engine/Platform/Input.h" // InputSnapshot (M75b: CanvasOfInput)

namespace mye {
namespace uilayout {
namespace {

// 壊れたデータ (親循環など) でも必ず停止する上限。正常なシーンの UI 階層はこれより浅い
constexpr int kMaxDepth = 64;

// UI ノードか (RectTransform か UIElement を持つ)。M75a で RectTransform だけの空ノード
// (Unity の「空の RectTransform」= グループ用コンテナ) も階層の基準になれるようにした
bool IsUiNode(World& world, EntityID e)
{
    return world.GetComponent<UIElementComponent>(e) != nullptr
        || world.GetComponent<RectTransformComponent>(e) != nullptr;
}

// 最寄りの UI ノード祖先 (間の非 UI ノードは読み飛ばす)。無ければ kNullEntity
EntityID FindUIParent(World& world, EntityID e)
{
    EntityID p = world.GetParent(e);
    for (int guard = 0; guard < kMaxDepth && p != kNullEntity; ++guard) {
        if (IsUiNode(world, p)) {
            return p;
        }
        p = world.GetParent(p);
    }
    return kNullEntity;
}

} // namespace

// 「UI 専用オブジェクト」判定 (完全自動追従の基準)。
// ★全エンティティは基本アーキタイプ (Name/LocalTransform/WorldMatrix/Hierarchy) を持つので
//   「Transform の有無」では判定できない (最初の実装で全 screen UI が追従して全滅した)。
// 許容 = 基本 4 種 + エディタ帳簿 (FileId/Active/Prefab*) + スクリプト状態
// (kComponentScriptState — ボタンにロジックを付けても UI 専用のまま) + **kComponentUiAux**
// (M75a: UIElement / RectTransform / 以後の UI コンポーネント群。型名の列挙をやめてフラグに
// したのは、UI コンポーネントを足すたびにここへ 1 行足し忘れると screen UI が丸ごと
// ワールド追従に落ちて消えるため)。
// それ以外 (メッシュ/コライダー/ライト/スキーマ等の実体コンポーネント) を 1 つでも持てば
// 「3D オブジェクト」= その上の UIElement はオブジェクトに追従する。
bool IsUiOnlyEntity(World& world, EntityID e)
{
    const Archetype* arch = world.GetArchetype(e);
    if (!arch) {
        return true;
    }
    const ComponentRegistry& reg = ComponentRegistry::Get();
    for (const ComponentTypeId t : arch->Types()) {
        if (t == NameComponent::sTypeId || t == LocalTransform::sTypeId
            || t == WorldMatrixComponent::sTypeId || t == HierarchyComponent::sTypeId
            || t == FileIdComponent::sTypeId || t == ActiveComponent::sTypeId
            || t == PrefabInstanceComponent::sTypeId || t == PrefabLinkComponent::sTypeId) {
            continue;
        }
        if (t < reg.Count()
            && (reg.Desc(t).flags & (kComponentScriptState | kComponentUiAux)) != 0) {
            continue; // C++/C# スクリプト状態と UI の脇役は UI ロジックの一部扱い
        }
        return false;
    }
    return true;
}

namespace {

// ワールド追従の基準点を解決して base (0 サイズ矩形 = 射影点) と out.scale を書く。
// 戻り値: 追従したか (false = 従来の screen 基準へ)。追従したが描けない
// (コンテキスト無し / カメラ背面でクランプ OFF) ときは out.visible=false。
// ★射影は scalar 演算のみ — sim レーン (UIHitTest / FocusNav) が同じ経路を通るため
//   SIMD (XMMatrix*) を混ぜると Debug/Release でビットが割れる
// el は UIElement (無ければ nullptr = RectTransform だけのノード。クランプ/距離スケール無し)
bool ResolveWorldBase(World& world, EntityID e, const UIElementComponent* el, int screenW,
                      int screenH, const UIWorldContext* wc, UIRect& base, UIResolved& out)
{
    if (IsUiOnlyEntity(world, e)) {
        return false; // UI 専用オブジェクト = 従来どおり画面 UI
    }
    const auto* wm = world.GetComponent<WorldMatrixComponent>(e);
    if (!wm) {
        return false;
    }
    if (!wc) {
        out.visible = false; // カメラ情報なし = world UI は描けない/押せない
        return true;
    }
    float px = wm->value._41;
    float py = wm->value._42;
    float pz = wm->value._43;
    // 描画補間 (M36b): メッシュと同じ prevWorld と alpha で位置だけ lerp する
    // (射影が食うのは平行移動のみ)。sim レーンは prevWorld=nullptr = 補間なし
    if (wc->prevWorld && wc->alpha < 1.0f) {
        if (const DirectX::XMFLOAT4X4* pm = wc->prevWorld->Get(e)) {
            px = pm->_41 + (px - pm->_41) * wc->alpha;
            py = pm->_42 + (py - pm->_42) * wc->alpha;
            pz = pm->_43 + (pz - pm->_43) * wc->alpha;
        }
    }
    const DirectX::XMFLOAT4X4& m = wc->viewProj;
    const float cx = px * m._11 + py * m._21 + pz * m._31 + m._41;
    const float cy = px * m._12 + py * m._22 + pz * m._32 + m._42;
    const float cw = px * m._14 + py * m._24 + pz * m._34 + m._44;
    const bool behind = cw <= 1e-4f; // ほぼカメラ面上もまとめて背面扱い (ゼロ除算防止)
    const bool clamp = el && el->clampToScreen;
    if (behind && !clamp) {
        out.visible = false;
        return true;
    }
    // 背面は |w| で射影すると中心対称の裏側に出るので反転して「後ろ方向の画面端」へ向ける
    // (クランプ ON の背面はオフスクリーンインジケータ的に端へ貼り付く)
    const float aw = behind ? -cw : cw;
    float fx = cx / aw;
    float fy = cy / aw;
    if (behind) {
        fx = -fx;
        fy = -fy;
    }
    base.x = (fx * 0.5f + 0.5f) * static_cast<float>(screenW);
    base.y = (1.0f - (fy * 0.5f + 0.5f)) * static_cast<float>(screenH); // NDC は y 上向き
    base.w = 0.0f; // 0 サイズ矩形 = anchor 9-grid はどれも射影点そのもの
    base.h = 0.0f;
    if (el && el->distanceScale) {
        const float refD = (el->distanceRef > 0.0f) ? el->distanceRef : 1.0f;
        const float d = (cw > 1e-3f) ? cw : 1e-3f; // 透視射影の w = ビュー空間深度 ~ 距離
        out.scale = refD / d;
    }
    return true;
}

UIResolved ResolveImpl(World& world, EntityID e, int screenW, int screenH,
                       const UIWorldContext* wc, int depth)
{
    UIResolved out;
    const auto* el = world.GetComponent<UIElementComponent>(e);
    const auto* rtp = world.GetComponent<RectTransformComponent>(e);
    if (!el && !rtp) {
        out.visible = false;
        return out;
    }
    // RectTransform 無し (スクリプトが UIElement だけ AddComponent した等) は既定値で解く —
    // 既定は旧 UIElement の既定と同値 (左上・pivot 0・160x40) なので M75a 以前と同じ絵になる
    static const RectTransformComponent kDefaultRt = {};
    const RectTransformComponent& rt = rtp ? *rtp : kDefaultRt;
    UIRect base = { 0, 0, static_cast<float>(screenW), static_cast<float>(screenH) };
    bool worldRoot = false;
    bool parentResolved = false;
    UIResolved parent;
    if (rt.basis == 0 && depth < kMaxDepth) {
        const EntityID p = FindUIParent(world, e);
        if (p != kNullEntity) {
            parent = ResolveImpl(world, p, screenW, screenH, wc, depth + 1);
            if (!parent.visible) {
                out.visible = false; // 親 (world 追従) が背面 → 子ごと消える
                return out;
            }
            base = parent.rect; // 親の**未回転**矩形。回転は親の xform が持つ
            out.scale = parent.scale; // 距離スケールは子のオフセット/サイズにも掛かる
            parentResolved = true;
        }
    }
    if (!parentResolved) {
        // basis=0 で UI 祖先なし、または basis=1 (キャンバス) —
        // 「UI 専用でないオブジェクト」に付いた UIElement はそのオブジェクトへ追従する
        worldRoot = ResolveWorldBase(world, e, el, screenW, screenH, wc, base, out);
        if (!out.visible) {
            return out;
        }
    }
    UIRect r = RectFromTransform(rt, base, out.scale);
    if (worldRoot && el && el->clampToScreen) {
        // 矩形が画面内へ収まるよう平行移動 (画面より大きい軸は左/上端起点)。
        // 子 (basis=0) は親の解決済み矩形基準なので一緒に付いてくる
        const float sw = static_cast<float>(screenW);
        const float sh = static_cast<float>(screenH);
        if (r.x + r.w > sw) {
            r.x = sw - r.w;
        }
        if (r.y + r.h > sh) {
            r.y = sh - r.h;
        }
        if (r.x < 0.0f) {
            r.x = 0.0f;
        }
        if (r.y < 0.0f) {
            r.y = 0.0f;
        }
    }
    out.rect = r;

    // ---- 回転 / スケール (M75a)。恒等ゲート: 自分も祖先も恒等なら xform を一切作らない ----
    if (parentResolved && parent.hasXform) {
        out.hasXform = true;
        out.xform = parent.xform;
    }
    const bool identity = rt.rotation == 0.0f && rt.scale.x == 1.0f && rt.scale.y == 1.0f;
    if (!identity) {
        // ローカル: pivot を中心に S → R。y 下向きなので標準の回転行列がそのまま
        // 「画面上で時計回り」になる。sin/cos は <cmath> の float 版 (BuildSimWorldContext の
        // std::tan と同じ扱い = CRT 依存だが Debug/Release で同じ関数を呼ぶ)
        const float rad = rt.rotation * (3.14159265358979323846f / 180.0f);
        const float cs = std::cos(rad);
        const float sn = std::sin(rad);
        UIXform l;
        l.a = cs * rt.scale.x;
        l.b = sn * rt.scale.x;
        l.c = -sn * rt.scale.y;
        l.d = cs * rt.scale.y;
        const float px = r.x + rt.pivot.x * r.w;
        const float py = r.y + rt.pivot.y * r.h;
        l.tx = px - (l.a * px + l.c * py);
        l.ty = py - (l.b * px + l.d * py);
        if (out.hasXform) {
            // 合成 = 親 ∘ ローカル (ローカルを先に掛ける)
            const UIXform& pm = out.xform;
            UIXform m;
            m.a = pm.a * l.a + pm.c * l.b;
            m.b = pm.b * l.a + pm.d * l.b;
            m.c = pm.a * l.c + pm.c * l.d;
            m.d = pm.b * l.c + pm.d * l.d;
            m.tx = pm.a * l.tx + pm.c * l.ty + pm.tx;
            m.ty = pm.b * l.tx + pm.d * l.ty + pm.ty;
            out.xform = m;
        } else {
            out.xform = l;
            out.hasXform = true;
        }
    }
    return out;
}

} // namespace

bool HasUiAncestor(World& world, EntityID e)
{
    return FindUIParent(world, e) != kNullEntity;
}

RectTransformComponent FromLegacyRect(int anchor, float x, float y, float w, float h, int space,
                                      bool hasUiAncestor)
{
    RectTransformComponent rt;
    const int a = (anchor < 0) ? 0 : (anchor > 8) ? 8 : anchor;
    float ax = 0.0f, ay = 0.0f;
    AnchorPreset(a, ax, ay);
    rt.anchorMin = { ax, ay };
    rt.anchorMax = { ax, ay };
    rt.pivot = { 0.0f, 0.0f }; // 旧意味論: 左上をアンカー点 + オフセットへ
    rt.anchoredPosition = { x, y };
    rt.sizeDelta = { w, h };
    rt.rotation = 0.0f;
    rt.scale = { 1.0f, 1.0f };
    // space=1 (親矩形基準) → 親。space=0 (画面基準) → UI 祖先の下にいるなら「親ではなく
    // キャンバス」を明示 (basis=1)。ルートなら basis=0 でも結果はキャンバスなので Unity 風の
    // 既定 (親基準) に寄せておく = 後で親の下へ動かしたときに付いてくる
    rt.basis = (space == 1) ? 0 : (hasUiAncestor ? 1 : 0);
    return rt;
}

UIRect RectFromTransform(const RectTransformComponent& rt, const UIRect& base, float scale)
{
    // ★加算順が正本 (UILayout.h の説明)。一致アンカー・pivot 0 では
    //   w = 0 + sizeDelta*scale、x = (base.x + base.w*a) + pos*scale - 0 で旧式と同ビット
    UIRect r;
    r.w = base.w * (rt.anchorMax.x - rt.anchorMin.x) + rt.sizeDelta.x * scale;
    r.h = base.h * (rt.anchorMax.y - rt.anchorMin.y) + rt.sizeDelta.y * scale;
    r.x = (base.x + base.w * rt.anchorMin.x) + rt.anchoredPosition.x * scale
        - rt.pivot.x * (rt.sizeDelta.x * scale);
    r.y = (base.y + base.h * rt.anchorMin.y) + rt.anchoredPosition.y * scale
        - rt.pivot.y * (rt.sizeDelta.y * scale);
    return r;
}

bool InvertXform(const UIXform& m, UIXform& out)
{
    const float det = m.a * m.d - m.b * m.c;
    if (std::fabs(det) < 1e-12f) {
        out = UIXform{};
        return false;
    }
    const float id = 1.0f / det;
    out.a = m.d * id;
    out.b = -m.b * id;
    out.c = -m.c * id;
    out.d = m.a * id;
    out.tx = -(out.a * m.tx + out.c * m.ty);
    out.ty = -(out.b * m.tx + out.d * m.ty);
    return true;
}

UIRect XformAabb(const UIXform& m, const UIRect& r)
{
    float xs[4], ys[4];
    XformPoint(m, r.x, r.y, xs[0], ys[0]);
    XformPoint(m, r.x + r.w, r.y, xs[1], ys[1]);
    XformPoint(m, r.x + r.w, r.y + r.h, xs[2], ys[2]);
    XformPoint(m, r.x, r.y + r.h, xs[3], ys[3]);
    float x0 = xs[0], x1 = xs[0], y0 = ys[0], y1 = ys[0];
    for (int i = 1; i < 4; ++i) {
        x0 = (xs[i] < x0) ? xs[i] : x0;
        x1 = (xs[i] > x1) ? xs[i] : x1;
        y0 = (ys[i] < y0) ? ys[i] : y0;
        y1 = (ys[i] > y1) ? ys[i] : y1;
    }
    return { x0, y0, x1 - x0, y1 - y0 };
}

CanvasInfo CanvasSize(int screenW, int screenH)
{
    CanvasInfo c;
    if (screenW <= 0 || screenH <= 0) {
        return c; // 退化した画面 (最小化など) は基準解像度そのままに倒す
    }
    const float sx = static_cast<float>(screenW) / static_cast<float>(kCanvasRefW);
    const float sy = static_cast<float>(screenH) / static_cast<float>(kCanvasRefH);
    c.scale = (sx < sy) ? sx : sy; // Expand = min。★16:9 では sx と sy が**同じ float** になる
    c.w = static_cast<int>(std::lroundf(static_cast<float>(screenW) / c.scale));
    c.h = static_cast<int>(std::lroundf(static_cast<float>(screenH) / c.scale));
    return c;
}

CanvasInfo CanvasOfInput(const InputSnapshot& in)
{
    // 未確定 (0) は CanvasSize の退化経路へそのまま流す = 基準解像度 + scale 1。
    // M70b の読み手の「canvasW == 0 なら kCanvasRefW」と同じ答えになる
    return CanvasSize(in.surfW, in.surfH);
}

UIResolved Resolve(World& world, EntityID e, int screenW, int screenH, const UIWorldContext* wc)
{
    return ResolveImpl(world, e, screenW, screenH, wc, 0);
}

bool BuildSimWorldContext(World& world, int screenW, int screenH, UIWorldContext& out)
{
    // カメラ選択は RenderSystem と同一規則: 走査順で最初のカメラ、isPrimary が出たら確定
    // (アーキタイプ内 return は RenderSystem の実装と同じ = 同じ結果を選ぶ)
    bool found = false;
    CameraComponent cam = {};
    DirectX::XMFLOAT4X4 cw = {};
    const ComponentTypeId req[] = { CameraComponent::sTypeId, WorldMatrixComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ci = arch.FindTypeIndex(CameraComponent::sTypeId);
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const auto* c = static_cast<const CameraComponent*>(arch.GetPtr(ci, row));
            if (!found || c->isPrimary != 0) {
                cam = *c;
                cw = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
                found = true;
                if (c->isPrimary != 0) {
                    return;
                }
            }
        }
    });
    if (!found) {
        return false;
    }
    // view = inverse(cameraWorld)。scalar の一般 3x3 逆行列 (スケール/シア込みで正しい) +
    // 平行移動は -t·inv3。SIMD (XMMatrixInverse) は使わない (sim レーンの決定論)
    const float a11 = cw._11, a12 = cw._12, a13 = cw._13;
    const float a21 = cw._21, a22 = cw._22, a23 = cw._23;
    const float a31 = cw._31, a32 = cw._32, a33 = cw._33;
    const float det = a11 * (a22 * a33 - a23 * a32) - a12 * (a21 * a33 - a23 * a31)
        + a13 * (a21 * a32 - a22 * a31);
    if (std::fabs(det) < 1e-12f) {
        return false; // 縮退カメラ行列
    }
    const float id = 1.0f / det;
    const float v11 = (a22 * a33 - a23 * a32) * id;
    const float v12 = (a13 * a32 - a12 * a33) * id;
    const float v13 = (a12 * a23 - a13 * a22) * id;
    const float v21 = (a23 * a31 - a21 * a33) * id;
    const float v22 = (a11 * a33 - a13 * a31) * id;
    const float v23 = (a13 * a21 - a11 * a23) * id;
    const float v31 = (a21 * a32 - a22 * a31) * id;
    const float v32 = (a12 * a31 - a11 * a32) * id;
    const float v33 = (a11 * a22 - a12 * a21) * id;
    const float tx = cw._41, ty = cw._42, tz = cw._43;
    const float vtx = -(tx * v11 + ty * v21 + tz * v31);
    const float vty = -(tx * v12 + ty * v22 + tz * v32);
    const float vtz = -(tx * v13 + ty * v23 + tz * v33);
    // PerspectiveFovLH (XMMatrixPerspectiveFovLH と同式) を scalar で
    const float fovY = cam.fovYDeg * (3.14159265358979323846f / 180.0f);
    const float ys = 1.0f / std::tan(fovY * 0.5f);
    const float aspect = (screenH > 0)
        ? static_cast<float>(screenW) / static_cast<float>(screenH) : 1.0f;
    const float xs = ys / aspect;
    const float fRange = cam.farZ / (cam.farZ - cam.nearZ);
    // viewProj = view * proj。proj の疎性 (列 1=xs / 2=ys / 3=fRange,+w / 4=z) を手展開
    DirectX::XMFLOAT4X4& o = out.viewProj;
    o._11 = v11 * xs; o._12 = v12 * ys; o._13 = v13 * fRange;                  o._14 = v13;
    o._21 = v21 * xs; o._22 = v22 * ys; o._23 = v23 * fRange;                  o._24 = v23;
    o._31 = v31 * xs; o._32 = v32 * ys; o._33 = v33 * fRange;                  o._34 = v33;
    o._41 = vtx * xs; o._42 = vty * ys; o._43 = vtz * fRange - cam.nearZ * fRange; o._44 = vtz;
    out.prevWorld = nullptr; // sim レーンは補間しない (決定論)
    out.alpha = 1.0f;
    return true;
}

UIRect ResolveRect(World& world, EntityID e, int screenW, int screenH, const UIWorldContext* wc)
{
    const UIResolved r = Resolve(world, e, screenW, screenH, wc);
    if (!r.visible) {
        return UIRect{}; // 非表示は {0,0,0,0} = 従来の「隠れている」表現
    }
    return r.hasXform ? XformAabb(r.xform, r.rect) : r.rect;
}

UIRect ResolveClipRect(World& world, EntityID e, int screenW, int screenH,
                       const UIWorldContext* wc)
{
    UIRect clip = { 0, 0, static_cast<float>(screenW), static_cast<float>(screenH) };
    EntityID p = FindUIParent(world, e);
    for (int guard = 0; guard < kMaxDepth && p != kNullEntity; ++guard) {
        const auto* el = world.GetComponent<UIElementComponent>(p);
        if (el && el->clipChildren != 0) {
            clip = Intersect(clip, ResolveRect(world, p, screenW, screenH, wc));
        }
        p = FindUIParent(world, p);
    }
    return clip;
}

UIRect ResolveVisibleRect(World& world, EntityID e, int screenW, int screenH,
                          const UIWorldContext* wc)
{
    return Intersect(ResolveRect(world, e, screenW, screenH, wc),
                     ResolveClipRect(world, e, screenW, screenH, wc));
}

} // namespace uilayout
} // namespace mye
