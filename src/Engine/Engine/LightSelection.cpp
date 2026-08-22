#include "Engine/Engine/LightSelection.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace DirectX;

namespace mye {

bool SphereInFrustum(const Frustum& f, const XMFLOAT3& center, float radius)
{
    for (int i = 0; i < 6; ++i) {
        const XMFLOAT4& p = f.planes[i];
        const float len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        if (len <= 0.0f) {
            // 退化した平面 (単位行列から作った視錐台などで起きる)。
            // ここで false を返すと「カメラが無い = 全ライト消滅」になるので判定から外す
            continue;
        }
        const float dist = (p.x * center.x + p.y * center.y + p.z * center.z + p.w) / len;
        if (dist < -radius) {
            return false; // 球が丸ごとこの平面の外側
        }
    }
    return true;
}

LightSelection SelectLights(const LightCandidate* cands, int count, const Frustum* frustum,
                            int maxLights, int maxShadowLights)
{
    LightSelection out;
    maxLights = (std::min)(maxLights, kMaxLights);
    maxLights = (std::max)(maxLights, 0);
    maxShadowLights = (std::max)(maxShadowLights, 0);
    if (cands == nullptr) {
        count = 0;
    }
    count = (std::max)(count, 0);

    // ---- 1. 範囲球 × 視錐台カリング ----
    // 平行光は無限遠 = 位置も範囲も意味を持たないのでカリング対象外。
    // 点/スポットは影響球が視錐台に触れなければ、可視領域のどの画素にも寄与しない
    // (スポットは円錐だが球で保守的に代用する — 落とし過ぎるよりは残す)。
    std::vector<int> keep;
    keep.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const LightCandidate& c = cands[i];
        if (c.light.type != 0 && frustum != nullptr
            && !SphereInFrustum(*frustum, c.light.position, c.light.range)) {
            ++out.culled;
            continue;
        }
        keep.push_back(i);
    }

    // ---- 2. 決定論キー (type → sortKey 昇順) でソート ----
    // sortKey = entity.index はエンティティ毎に一意なので (type, sortKey) は全順序になり、
    // std::sort が不安定であることは結果に出ない。第 3 キーの入力添字は、呼び出し側が
    // 同じ sortKey を二度渡してしまった場合でも順序が非決定にならないための保険
    std::sort(keep.begin(), keep.end(), [cands](int a, int b) {
        const LightCandidate& la = cands[a];
        const LightCandidate& lb = cands[b];
        if (la.light.type != lb.light.type) {
            return la.light.type < lb.light.type;
        }
        if (la.sortKey != lb.sortKey) {
            return la.sortKey < lb.sortKey;
        }
        return a < b;
    });

    // ---- 3. 上限で切り詰め + 影スロットの割り当て ----
    for (size_t i = 0; i < keep.size(); ++i) {
        if (out.count >= maxLights) {
            out.overflow = static_cast<int>(keep.size() - i);
            break;
        }
        const LightCandidate& c = cands[keep[i]];
        SelectedLight& s = out.lights[out.count++];
        s.light = c.light;
        s.sortKey = c.sortKey;
        // 平行光の影は既存の CSM (ShadowPass) が担当する。M54c 以降のアトラスは局所ライト専用。
        // 枠はソート後の順に前詰め = 同じシーンなら frame をまたいでも同じ枠に落ちる
        if (c.castShadow != 0 && c.light.type != 0 && out.shadowCount < maxShadowLights) {
            s.shadowSlot = out.shadowCount++;
        }
    }

    // ---- 4. ライトが 1 つも無いシーンの既定平行光 (従来挙動) ----
    // ★条件は「候補が 0 件」であって「選別後が 0 件」ではない。全部カリングで落ちたときに
    //   補うと、画面外の点光源だけのシーンでカメラを振った瞬間に太陽が湧いて消える
    if (count == 0 && out.count < maxLights) {
        SelectedLight& s = out.lights[out.count++];
        XMStoreFloat3(&s.light.direction, XMVector3Normalize(XMVectorSet(0.3f, -0.8f, 0.5f, 0)));
        s.light.type = 0;
    }

    return out;
}

} // namespace mye
