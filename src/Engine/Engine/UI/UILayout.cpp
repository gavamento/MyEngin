#include "Engine/Engine/UI/UILayout.h"

#include <cmath>

#include "Engine/Core/ComponentRegistry.h" // kComponentScriptState (UI 専用判定)
#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/RenderSystem.h" // PrevWorldStore (描画補間 M36b)

namespace mye {
namespace uilayout {
namespace {

// 壊れたデータ (親循環など) でも必ず停止する上限。正常なシーンの UI 階層はこれより浅い
constexpr int kMaxDepth = 64;

// 最寄りの UIElement 持ち祖先 (間の非 UI ノードは読み飛ばす)。無ければ kNullEntity
EntityID FindUIParent(World& world, EntityID e)
{
    EntityID p = world.GetParent(e);
    for (int guard = 0; guard < kMaxDepth && p != kNullEntity; ++guard) {
        if (world.GetComponent<UIElementComponent>(p)) {
            return p;
        }
        p = world.GetParent(p);
    }
    return kNullEntity;
}

// 「UI 専用オブジェクト」判定 (完全自動追従の基準)。
// ★全エンティティは基本アーキタイプ (Name/LocalTransform/WorldMatrix/Hierarchy) を持つので
//   「Transform の有無」では判定できない (最初の実装で全 screen UI が追従して全滅した)。
// 許容 = 基本 4 種 + UIElement + エディタ帳簿 (FileId/Active/Prefab*) + スクリプト状態
// (kComponentScriptState — ボタンにロジックを付けても UI 専用のまま)。
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
            || t == UIElementComponent::sTypeId || t == FileIdComponent::sTypeId
            || t == ActiveComponent::sTypeId || t == PrefabInstanceComponent::sTypeId
            || t == PrefabLinkComponent::sTypeId) {
            continue;
        }
        if (t < reg.Count() && (reg.Desc(t).flags & kComponentScriptState) != 0) {
            continue; // C++/C# スクリプト状態は UI ロジックの脇役扱い
        }
        return false;
    }
    return true;
}

// ワールド追従の基準点を解決して base (0 サイズ矩形 = 射影点) と out.scale を書く。
// 戻り値: 追従したか (false = 従来の screen 基準へ)。追従したが描けない
// (コンテキスト無し / カメラ背面でクランプ OFF) ときは out.visible=false。
// ★射影は scalar 演算のみ — sim レーン (UIHitTest / FocusNav) が同じ経路を通るため
//   SIMD (XMMatrix*) を混ぜると Debug/Release でビットが割れる
bool ResolveWorldBase(World& world, EntityID e, const UIElementComponent& el, int screenW,
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
    if (behind && !el.clampToScreen) {
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
    if (el.distanceScale) {
        const float refD = (el.distanceRef > 0.0f) ? el.distanceRef : 1.0f;
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
    if (!el) {
        out.visible = false;
        return out;
    }
    UIRect base = { 0, 0, static_cast<float>(screenW), static_cast<float>(screenH) };
    bool worldRoot = false;
    bool parentResolved = false;
    if (el->space == 1 && depth < kMaxDepth) {
        const EntityID p = FindUIParent(world, e);
        if (p != kNullEntity) {
            const UIResolved parent = ResolveImpl(world, p, screenW, screenH, wc, depth + 1);
            if (!parent.visible) {
                out.visible = false; // 親 (world 追従) が背面 → 子ごと消える
                return out;
            }
            base = parent.rect;
            out.scale = parent.scale; // 距離スケールは子のオフセット/サイズにも掛かる
            parentResolved = true;
        }
    }
    if (!parentResolved) {
        // space=0、または space=1 で UI 親なし (従来は screen フォールバック) —
        // 「UI 専用でないオブジェクト」に付いた UIElement はそのオブジェクトへ追従する
        worldRoot = ResolveWorldBase(world, e, *el, screenW, screenH, wc, base, out);
        if (!out.visible) {
            return out;
        }
    }
    UIRect r;
    AnchorOrigin(el->anchor, base, r.x, r.y);
    r.x += el->x * out.scale;
    r.y += el->y * out.scale;
    r.w = el->w * out.scale;
    r.h = el->h * out.scale;
    if (worldRoot && el->clampToScreen) {
        // 矩形が画面内へ収まるよう平行移動 (画面より大きい軸は左/上端起点)。
        // 子 (space=1) は親の解決済み矩形基準なので一緒に付いてくる
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
    return out;
}

} // namespace

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
    return r.visible ? r.rect : UIRect{}; // 非表示は {0,0,0,0} = 従来の「隠れている」表現
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
