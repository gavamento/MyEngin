//====================================================================================
//                          AcousticNav.cpp
//  MyEngine/ 秋田蓮音                                                      09/01/2026
//                                          航法グリッドの構築と流れ場（毎 tick 全再計算）
//====================================================================================
#include "Engine/Engine/Acoustic/AcousticNav.h"

#include <algorithm>
#include <cmath>

#include "Engine/Core/Profiler.h"
#include "Engine/Engine/Acoustic/AcousticField.h"

namespace mye {
namespace {

constexpr uint16_t kUnreached = 0xFFFFu;

inline int64_t NavIndex(const AcousticGridDesc& g, int32_t cx, int32_t cy, int32_t cz)
{
    return acoustic::CellIndex(g, cx, cy, cz);
}

} // namespace

void AcousticNav::Reset()
{
    nav_ = AcousticGridDesc{};
    navSolid_.clear();
    navSolid_.shrink_to_fit();
    srcGrid_ = AcousticGridDesc{};
    sourceSig_ = 0;
    sourceRatio_ = 0;
    fields_.clear();
    buckets_.clear();
}

void AcousticNav::Sync(const AcousticField& field)
{
    if (!field.HasVolume()) {
        if (!navSolid_.empty()) {
            Reset();
        }
        return;
    }
    const AcousticGridDesc& src = field.Grid();
    const int32_t ratio = (field.NavCellRatio() > 0) ? field.NavCellRatio() : 1;
    // ★署名が同じなら焼き直さない。**これはキャッシュではない** — 粗占有は
    //   (細占有, ratio) の純関数で、署名が変わらない = 入力が変わっていない、が保証されている
    //   (流れ場のほうは毎 tick 捨てる。判断 6 が禁じているのはそちら)
    if (sourceSig_ == field.StaticSignature() && sourceRatio_ == ratio && !navSolid_.empty()
        && acoustic::SameGrid(srcGrid_, src)) {
        return;
    }
    srcGrid_ = src;

    // 切り上げ = 端の半端なセルも 1 個持つ。切り捨てるとグリッドの端が航法から消えて
    // 「壁でもないのに敵が入れない帯」ができる
    nav_.dimX = (src.dimX + ratio - 1) / ratio;
    nav_.dimY = (src.dimY + ratio - 1) / ratio;
    nav_.dimZ = (src.dimZ + ratio - 1) / ratio;
    nav_.cellSize = src.cellSize * static_cast<float>(ratio);
    nav_.minX = src.minX;
    nav_.minY = src.minY;
    nav_.minZ = src.minZ;
    sourceSig_ = field.StaticSignature();
    sourceRatio_ = ratio;
    fields_.clear();

    navSolid_.assign(static_cast<size_t>(nav_.CellCount()), 1u);
    for (int32_t cz = 0; cz < nav_.dimZ; ++cz) {
        for (int32_t cy = 0; cy < nav_.dimY; ++cy) {
            for (int32_t cx = 0; cx < nav_.dimX; ++cx) {
                // ★**サブセルの過半が開なら開**。「1 個でも開なら開」だと壁の角が
                //   繋がって敵が壁をすり抜け、「全部開でなければ閉」だと 1m 幅の廊下が
                //   丸ごと消える。過半なら両方とも起きない
                int open = 0;
                int total = 0;
                for (int32_t sz = 0; sz < ratio; ++sz) {
                    for (int32_t sy = 0; sy < ratio; ++sy) {
                        for (int32_t sx = 0; sx < ratio; ++sx) {
                            const int32_t fx = cx * ratio + sx;
                            const int32_t fy = cy * ratio + sy;
                            const int32_t fz = cz * ratio + sz;
                            if (!acoustic::InBounds(field.Grid(), fx, fy, fz)) {
                                continue; // 端の半端 = 総数に数えない
                            }
                            ++total;
                            open += field.IsSolid(fx, fy, fz) ? 0 : 1;
                        }
                    }
                }
                const bool solid = (total == 0) || (open * 2 <= total);
                navSolid_[static_cast<size_t>(NavIndex(nav_, cx, cy, cz))] = solid ? 1u : 0u;
            }
        }
    }
}

bool AcousticNav::IsSolid(int32_t cx, int32_t cy, int32_t cz) const
{
    if (!acoustic::InBounds(nav_, cx, cy, cz)) {
        return true; // グリッド外は壁扱い (敵が箱の外へ出ない保証を型で持つ)
    }
    return navSolid_[static_cast<size_t>(NavIndex(nav_, cx, cy, cz))] != 0;
}

void AcousticNav::BeginTick()
{
    // ★**ここが判断 6 の実体**。前 tick の場を 1 本も残さない = 場は毎 tick
    //   (占有, 目標セル) の純関数で、履歴が存在しない
    fields_.clear();
}

void AcousticNav::BuildDistance(Field& f) const
{
    const size_t n = static_cast<size_t>(nav_.CellCount());
    f.dist.assign(n, kUnreached);

    // ★バケットの本数は「最短路がセルを 2 度通らない」ことから来る上界を uint16 で
    //   切ったもの。番兵 (kUnreached) の 1 つ手前まで。これを超える距離のセルは
    //   到達不能として扱う = 敵が動かないだけ (安全側に倒れる)
    const int64_t bound = static_cast<int64_t>(n) * static_cast<int64_t>(acoustic::kFaceCost);
    const uint32_t maxDist = static_cast<uint32_t>((bound < 65534) ? bound : 65534);
    if (buckets_.size() < static_cast<size_t>(maxDist) + 1u) {
        buckets_.resize(static_cast<size_t>(maxDist) + 1u);
    }
    // ★**先頭で全部 clear しない**。下のループは処理したバケットをその場で空にするので、
    //   前回の走査が完走していれば全部空のまま残っている (数万本の clear を毎 tick
    //   回すのが流れ場のコストの大半になってしまう)。capacity は使い回される

    const int64_t start = NavIndex(nav_, f.tx, f.ty, f.tz);
    f.dist[static_cast<size_t>(start)] = 0;
    buckets_[0].push_back(static_cast<int32_t>(start));
    size_t pending = 1;

    // 伝播と同じ Dial 法。★重みが同じだから「音が回り込んだ道」と
    //   「敵が歩く道」が同じ形になる (別の距離を使うと、聞こえた所へ別の道で来る)
    for (uint32_t d = 0; d <= maxDist && pending > 0; ++d) {
        std::vector<int32_t>& bucket = buckets_[d];
        for (size_t k = 0; k < bucket.size(); ++k) {
            const int32_t li = bucket[k];
            --pending;
            if (f.dist[static_cast<size_t>(li)] != static_cast<uint16_t>(d)) {
                continue; // より短い距離で上書きされた古いエントリ
            }
            const int32_t x = li % nav_.dimX;
            const int32_t y = (li / nav_.dimX) % nav_.dimY;
            const int32_t z = li / (nav_.dimX * nav_.dimY);
            for (int i = 0; i < acoustic::kNeighborCount; ++i) {
                const acoustic::Neighbor& nb = acoustic::kNeighbors[i];
                const int32_t nx = x + nb.dx;
                const int32_t ny = y + nb.dy;
                const int32_t nz = z + nb.dz;
                if (IsSolid(nx, ny, nz)) {
                    continue;
                }
                const uint32_t nd = d + nb.cost;
                if (nd > maxDist) {
                    continue;
                }
                const int32_t ni = static_cast<int32_t>(NavIndex(nav_, nx, ny, nz));
                if (nd >= f.dist[static_cast<size_t>(ni)]) {
                    continue;
                }
                f.dist[static_cast<size_t>(ni)] = static_cast<uint16_t>(nd);
                // ★挿入先は必ず d+11 以上 = 現区間の外なので、処理済みのバケットへ
                //   書き戻されることが構造的に起きない (伝播側と同じ性質)
                buckets_[nd].push_back(ni);
                ++pending;
            }
        }
        bucket.clear();
    }
    // ★ここに「残骸を掃除する」ループは要らない。pending は「まだ処理していない
    //   エントリ数」そのもので、押し込む距離は必ず maxDist 以下だから、
    //   pending == 0 で抜けた時点でどのバケットも空である (= 次回は素の状態で始まる)。
    //   数万本の clear を毎 tick 回すのは流れ場のコストの大半になるので、
    //   この不変条件に乗るのが正しい
}

int AcousticNav::BuildFlowField(float wx, float wy, float wz)
{
    if (!Valid()) {
        return -1;
    }
    int32_t tx = 0, ty = 0, tz = 0;
    if (!acoustic::WorldToCell(nav_, wx, wy, wz, tx, ty, tz)) {
        return -1; // グリッドの外を目標にはできない
    }
    if (IsSolid(tx, ty, tz)) {
        // 目標が閉セル (壁の中で鳴った音など) は 26 近傍の**表の順**に開セルへ寄せる。
        // 見つからなければ諦める — 寄せ先を距離で選ぶと float が順序に入る
        bool moved = false;
        for (int i = 0; i < acoustic::kNeighborCount; ++i) {
            const acoustic::Neighbor& nb = acoustic::kNeighbors[i];
            if (!IsSolid(tx + nb.dx, ty + nb.dy, tz + nb.dz)) {
                tx += nb.dx;
                ty += nb.dy;
                tz += nb.dz;
                moved = true;
                break;
            }
        }
        if (!moved) {
            return -1;
        }
    }
    for (size_t i = 0; i < fields_.size(); ++i) {
        if (fields_[i].tx == tx && fields_[i].ty == ty && fields_[i].tz == tz) {
            return static_cast<int>(i); // 同じ粗セルを指す要求は 1 本を共有する
        }
    }
    if (static_cast<int>(fields_.size()) >= kMaxFields) {
        return -1;
    }
    MYE_PROFILE_SCOPE("acoustic.nav");
    Field f;
    f.tx = tx;
    f.ty = ty;
    f.tz = tz;
    BuildDistance(f);
    fields_.push_back(std::move(f));
    return static_cast<int>(fields_.size()) - 1;
}

bool AcousticNav::SampleDirection(int field, float wx, float wy, float wz, float& outDx,
                                  float& outDz) const
{
    outDx = 0.0f;
    outDz = 0.0f;
    if (field < 0 || field >= static_cast<int>(fields_.size())) {
        return false;
    }
    const Field& f = fields_[static_cast<size_t>(field)];
    int32_t cx = 0, cy = 0, cz = 0;
    if (!acoustic::WorldToCell(nav_, wx, wy, wz, cx, cy, cz)) {
        return false;
    }
    // 自分のセルが閉 (壁にめり込んでいる) なら、開いている隣へ**表の順**に逃がす
    if (IsSolid(cx, cy, cz)) {
        bool moved = false;
        for (int i = 0; i < acoustic::kNeighborCount; ++i) {
            const acoustic::Neighbor& nb = acoustic::kNeighbors[i];
            if (!IsSolid(cx + nb.dx, cy + nb.dy, cz + nb.dz)) {
                cx += nb.dx;
                cy += nb.dy;
                cz += nb.dz;
                moved = true;
                break;
            }
        }
        if (!moved) {
            return false;
        }
    }
    const uint16_t here = f.dist[static_cast<size_t>(NavIndex(nav_, cx, cy, cz))];
    if (here == kUnreached) {
        return false; // 目標とは繋がっていない (別の部屋に閉じ込められている等)
    }
    if (here == 0) {
        return false; // もう目標セルに居る
    }
    // ★**厳密に小さい隣だけを採る**。「以下」にすると同距離のセルの間を往復しうる。
    //   同点は表の順で先に見つかったほうが勝つ = 決定論のタイブレーク (規則 7)
    uint16_t best = here;
    int bestIdx = -1;
    for (int i = 0; i < acoustic::kNeighborCount; ++i) {
        const acoustic::Neighbor& nb = acoustic::kNeighbors[i];
        const int32_t nx = cx + nb.dx;
        const int32_t ny = cy + nb.dy;
        const int32_t nz = cz + nb.dz;
        if (IsSolid(nx, ny, nz)) {
            continue;
        }
        const uint16_t d = f.dist[static_cast<size_t>(NavIndex(nav_, nx, ny, nz))];
        if (d < best) {
            best = d;
            bestIdx = i;
        }
    }
    if (bestIdx < 0) {
        return false;
    }
    // 水平成分だけを返す (縦の移動は CharacterController の重力に任せる)。
    // 真上/真下だけの隣が選ばれると水平成分が 0 になるので、その場合は「進めない」
    const acoustic::Neighbor& nb = acoustic::kNeighbors[bestIdx];
    const float dx = static_cast<float>(nb.dx);
    const float dz = static_cast<float>(nb.dz);
    const float len = std::sqrt(dx * dx + dz * dz);
    if (!(len > 0.0f)) {
        return false;
    }
    outDx = dx / len;
    outDz = dz / len;
    return true;
}

bool AcousticNav::ReachedTarget(int field, float wx, float wy, float wz) const
{
    if (field < 0 || field >= static_cast<int>(fields_.size())) {
        return false;
    }
    const Field& f = fields_[static_cast<size_t>(field)];
    int32_t cx = 0, cy = 0, cz = 0;
    if (!acoustic::WorldToCell(nav_, wx, wy, wz, cx, cy, cz)) {
        return false;
    }
    // 同じ粗セルに入ったら到着。距離で測らないのは、そこだけ float の比較が
    // 「状態が遷移したか」を決めることになるため (セルの一致は整数比較)
    return cx == f.tx && cy == f.ty && cz == f.tz;
}

} // namespace mye
