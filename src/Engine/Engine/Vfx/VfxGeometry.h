#pragma once
// VFX ジオメトリ構築の純関数群 (M29c)。D3D 非依存 — VfxSelfTest がヘッドレスで検証する。
// Sprite/Trail/TextMesh は描画専用 (kComponentNoHash) なので、ここの float 演算は
// sim/リプレイの決定論契約とは無関係 (XMVECTOR 使用可)。
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <vector>

#include <DirectXMath.h>

namespace mye {

struct VfxVertex {
    DirectX::XMFLOAT3 pos;
    DirectX::XMFLOAT2 uv;
    DirectX::XMFLOAT4 color;
};

// UIRenderer のグリフ計測のコピー (VfxRenderer/TextMesh 用の D3D 非依存ビュー)
struct VfxGlyph {
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0; // アトラス UV
    float w = 0, h = 0;                   // グリフ寸法 (px)
    float advance = 0;                    // 送り幅 (px)
    bool valid = false;
};

// TextMesh のワールドスケール係数: フォント 1px が fontScale=1 で何ワールド単位か。
// 8px フォント × 0.0375 = 行高 ≈ 0.3 ユニット
inline constexpr float kVfxWorldPerPx = 0.0375f;

namespace vfx {

// トレイル点 (TrailStore が蓄積、BuildTrailRibbon が消費)
struct TrailPoint {
    DirectX::XMFLOAT3 pos;
    uint64_t tick = 0; // 追加時の tick (寿命判定)
};

// 中央揃えの単一行テキストをローカル XY 平面 (+X 右、+Y 上、原点=中心、z=0) の
// クアッド列として out に追加する。glyphs は 128 要素のテーブル。戻り値 = 追加頂点数。
inline int BuildTextQuadsLocal(const char* text, const VfxGlyph* glyphs, float scale,
                               const DirectX::XMFLOAT4& color, std::vector<VfxVertex>& out)
{
    if (!text || !glyphs || scale <= 0.0f) {
        return 0;
    }
    const float k = kVfxWorldPerPx * scale;
    float totalPx = 0.0f;
    float maxHPx = 0.0f;
    for (const char* p = text; *p; ++p) {
        const VfxGlyph& g = glyphs[static_cast<unsigned char>(*p) & 127];
        totalPx += (g.valid ? g.advance : 4.0f);
        if (g.valid && g.h > maxHPx) {
            maxHPx = g.h;
        }
    }
    if (totalPx <= 0.0f) {
        return 0;
    }
    int added = 0;
    float penX = -totalPx * 0.5f * k;
    const float topY = maxHPx * 0.5f * k;
    for (const char* p = text; *p; ++p) {
        const VfxGlyph& g = glyphs[static_cast<unsigned char>(*p) & 127];
        if (!g.valid) {
            penX += 4.0f * k;
            continue;
        }
        if (g.u1 > g.u0 && g.v1 > g.v0) {
            const float x0 = penX;
            const float x1 = penX + g.w * k;
            const float y0 = topY;          // 上端 (+Y)
            const float y1 = topY - g.h * k; // 下端。px は上原点なので反転
            const VfxVertex q[6] = {
                { { x0, y0, 0 }, { g.u0, g.v0 }, color }, { { x1, y0, 0 }, { g.u1, g.v0 }, color },
                { { x1, y1, 0 }, { g.u1, g.v1 }, color }, { { x0, y0, 0 }, { g.u0, g.v0 }, color },
                { { x1, y1, 0 }, { g.u1, g.v1 }, color }, { { x0, y1, 0 }, { g.u0, g.v1 }, color },
            };
            for (const VfxVertex& v : q) {
                out.push_back(v);
            }
            added += 6;
        }
        penX += g.advance * k;
    }
    return added;
}

// トレイルのリボン頂点を out に追加する。pts は追加順 (先頭=最古 → 末尾=最新)。
// 各点の age = (now − tick)/lifeTicks (0=新..1=古) で色を colorBegin→colorEnd、
// 幅を width→0 にテーパ。幅方向 = normalize(cross(進行方向, 視線方向))。戻り値 = 追加頂点数。
inline int BuildTrailRibbon(const TrailPoint* pts, int count, uint64_t nowTick, float lifeTicks,
                            float width, const DirectX::XMFLOAT4& colorBegin,
                            const DirectX::XMFLOAT4& colorEnd, const DirectX::XMFLOAT3& camPos,
                            std::vector<VfxVertex>& out)
{
    using namespace DirectX;
    if (!pts || count < 2 || lifeTicks <= 0.0f || width <= 0.0f) {
        return 0;
    }
    // 各点の side ベクトル (隣接セグメント方向 × 視線) と減衰係数を先に求める
    int added = 0;
    XMFLOAT3 sides[2];
    float fades[2];
    XMFLOAT4 cols[2];
    for (int i = 0; i + 1 < count; ++i) {
        for (int e = 0; e < 2; ++e) {
            const int pi = i + e;
            const XMFLOAT3& p = pts[pi].pos;
            // 進行方向: 端点は隣接セグメント、内部点は前後平均でなく後続方向 (簡易)
            const int a = (pi + 1 < count) ? pi : pi - 1;
            const int b = (pi + 1 < count) ? pi + 1 : pi;
            XMVECTOR dir = XMVectorSubtract(XMLoadFloat3(&pts[b].pos), XMLoadFloat3(&pts[a].pos));
            XMVECTOR toCam = XMVectorSubtract(XMLoadFloat3(&camPos), XMLoadFloat3(&p));
            XMVECTOR side = XMVector3Cross(dir, toCam);
            if (XMVectorGetX(XMVector3LengthSq(side)) < 1e-10f) {
                side = XMVectorSet(0, 1, 0, 0); // 縮退 (視線と平行等) は Y 固定
            } else {
                side = XMVector3Normalize(side);
            }
            const float age =
                (std::min)(1.0f, static_cast<float>(nowTick - pts[pi].tick) / lifeTicks);
            const float half = width * 0.5f * (1.0f - age);
            XMStoreFloat3(&sides[e], XMVectorScale(side, half));
            fades[e] = age;
            cols[e] = { colorBegin.x + (colorEnd.x - colorBegin.x) * age,
                        colorBegin.y + (colorEnd.y - colorBegin.y) * age,
                        colorBegin.z + (colorEnd.z - colorBegin.z) * age,
                        colorBegin.w + (colorEnd.w - colorBegin.w) * age };
        }
        const XMFLOAT3& p0 = pts[i].pos;
        const XMFLOAT3& p1 = pts[i + 1].pos;
        const XMFLOAT3 a0 = { p0.x + sides[0].x, p0.y + sides[0].y, p0.z + sides[0].z };
        const XMFLOAT3 b0 = { p0.x - sides[0].x, p0.y - sides[0].y, p0.z - sides[0].z };
        const XMFLOAT3 a1 = { p1.x + sides[1].x, p1.y + sides[1].y, p1.z + sides[1].z };
        const XMFLOAT3 b1 = { p1.x - sides[1].x, p1.y - sides[1].y, p1.z - sides[1].z };
        const VfxVertex q[6] = {
            { a0, { fades[0], 0 }, cols[0] }, { a1, { fades[1], 0 }, cols[1] },
            { b1, { fades[1], 1 }, cols[1] }, { a0, { fades[0], 0 }, cols[0] },
            { b1, { fades[1], 1 }, cols[1] }, { b0, { fades[0], 1 }, cols[0] },
        };
        for (const VfxVertex& v : q) {
            out.push_back(v);
        }
        added += 6;
    }
    return added;
}

// billboardMode からワールドの right/up 基底を決める。
// mode 0=Billboard (カメラ基底そのまま) / 1=BillboardY (ヨーのみカメラへ、up=+Y) /
// 2=World (ワールド行列の X/Y 基底を正規化)
inline void BillboardBasis(int mode, const DirectX::XMFLOAT4X4& world,
                           const DirectX::XMFLOAT3& camRight, const DirectX::XMFLOAT3& camUp,
                           const DirectX::XMFLOAT3& camPos, DirectX::XMFLOAT3& outRight,
                           DirectX::XMFLOAT3& outUp)
{
    using namespace DirectX;
    if (mode == 2) {
        XMVECTOR r = XMVectorSet(world._11, world._12, world._13, 0);
        XMVECTOR u = XMVectorSet(world._21, world._22, world._23, 0);
        r = XMVector3Normalize(r);
        u = XMVector3Normalize(u);
        XMStoreFloat3(&outRight, r);
        XMStoreFloat3(&outUp, u);
        return;
    }
    if (mode == 1) {
        const float tx = camPos.x - world._41;
        const float tz = camPos.z - world._43;
        const float len = std::sqrt(tx * tx + tz * tz);
        if (len > 1e-5f) {
            // right = (-toCam.z, 0, toCam.x) — 正面ビルボード (mode0) と同じ向きになる符号
            outRight = { -tz / len, 0.0f, tx / len };
            outUp = { 0.0f, 1.0f, 0.0f };
            return;
        } // 真上/真下からはカメラ基底へフォールバック
    }
    outRight = camRight;
    outUp = camUp;
}

} // namespace vfx
} // namespace mye
