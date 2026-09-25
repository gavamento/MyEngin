//====================================================================================
//                          FractureSystem.cpp
//  MyEngin/ 秋田蓮音                                                     09/25/2026
//                                          接着の破断・塊の分離・kinematicルートの実装
//====================================================================================
#include "Engine/Engine/FractureSystem.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Engine/Core/AssetGuidResolver.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Physics/FractureLibrary.h"
#include "Engine/Engine/Physics/PhysicsSystem.h"

using namespace DirectX;

namespace mye {
namespace {

// ---- scalar クォータニオン演算 (PhysicsSystem.cpp / RagdollBuilder.cpp と同じ式) ----
void QuatMul(float ax, float ay, float az, float aw, float bx, float by, float bz, float bw,
             float& ox, float& oy, float& oz, float& ow)
{
    ox = aw * bx + ax * bw + ay * bz - az * by;
    oy = aw * by - ax * bz + ay * bw + az * bx;
    oz = aw * bz + ax * by - ay * bx + az * bw;
    ow = aw * bw - ax * bx - ay * by - az * bz;
}

void QuatRotate(float qx, float qy, float qz, float qw, float vx, float vy, float vz, float& ox,
                float& oy, float& oz)
{
    const float tx = 2.0f * (qy * vz - qz * vy);
    const float ty = 2.0f * (qz * vx - qx * vz);
    const float tz = 2.0f * (qx * vy - qy * vx);
    ox = vx + qw * tx + (qy * tz - qz * ty);
    oy = vy + qw * ty + (qz * tx - qx * tz);
    oz = vz + qw * tz + (qx * ty - qy * tx);
}

uint64_t EntityKey(EntityID e)
{
    return (static_cast<uint64_t>(e.index) << 32) | e.generation;
}

// e の親チェーンから合成したワールドスケール成分積 (PhysicsSystem.cpp の WorldFrame と同じ
// 「シアーは無視」の近似)。ComposeEntityWorldPose (位置/回転) と対で使う
XMFLOAT3 ComposeWorldScale(World& world, EntityID e)
{
    XMFLOAT3 s{ 1.0f, 1.0f, 1.0f };
    for (EntityID cur = e; !cur.IsNull(); cur = world.GetParent(cur)) {
        const auto* lt = world.GetComponent<LocalTransform>(cur);
        if (lt == nullptr) {
            break;
        }
        s.x *= lt->scale.x;
        s.y *= lt->scale.y;
        s.z *= lt->scale.z;
    }
    return s;
}

// child の現在のワールド姿勢を保ったまま newParent の下でのローカル TRS を書いてから
// SetParent する。**XMMatrixInverse/XMMatrixDecompose は使わない** — どちらも構成間で
// ビットが割れうる (PartFollowSystem.h の DecomposeRowMajorTRS 導入の理由と同じ)。
// SetParent 自体は World::SetParent が常に tick 末のコマンドバッファへ積む
void ReparentKeepWorld(World& world, EntityID child, EntityID newParent)
{
    float cpx, cpy, cpz, cqx, cqy, cqz, cqw;
    ComposeEntityWorldPose(world, child, cpx, cpy, cpz, cqx, cqy, cqz, cqw);
    const XMFLOAT3 cscale = ComposeWorldScale(world, child);

    float ppx = 0.0f, ppy = 0.0f, ppz = 0.0f, pqx = 0.0f, pqy = 0.0f, pqz = 0.0f, pqw = 1.0f;
    XMFLOAT3 pscale{ 1.0f, 1.0f, 1.0f };
    if (!newParent.IsNull()) {
        ComposeEntityWorldPose(world, newParent, ppx, ppy, ppz, pqx, pqy, pqz, pqw);
        pscale = ComposeWorldScale(world, newParent);
    }

    const float dx = cpx - ppx, dy = cpy - ppy, dz = cpz - ppz;
    float rx, ry, rz;
    QuatRotate(-pqx, -pqy, -pqz, pqw, dx, dy, dz, rx, ry, rz);
    const float isx = pscale.x != 0.0f ? 1.0f / pscale.x : 1.0f;
    const float isy = pscale.y != 0.0f ? 1.0f / pscale.y : 1.0f;
    const float isz = pscale.z != 0.0f ? 1.0f / pscale.z : 1.0f;

    float lqx, lqy, lqz, lqw;
    QuatMul(-pqx, -pqy, -pqz, pqw, cqx, cqy, cqz, cqw, lqx, lqy, lqz, lqw);

    if (auto* lt = world.GetComponent<LocalTransform>(child)) {
        lt->position = { rx * isx, ry * isy, rz * isz };
        lt->rotation = { lqx, lqy, lqz, lqw };
        lt->scale = { cscale.x * isx, cscale.y * isy, cscale.z * isz };
    }
    world.SetParent(child, newParent);
}

// shapeImpulses (entity.index 昇順が契約、ShapeImpulse 参照) から entity の合計法線インパルスを引く
float FindShapeImpulse(const std::vector<ShapeImpulse>& v, EntityID e)
{
    const auto it = std::lower_bound(v.begin(), v.end(), e.index,
                                     [](const ShapeImpulse& s, uint32_t idx) { return s.entity.index < idx; });
    if (it != v.end() && it->entity == e) {
        return it->impulse;
    }
    return 0.0f;
}

// 決定的 Union-Find (常に小さい index を根にする — 破片 index 昇順の結合を保証する)
struct UnionFind {
    std::vector<int32_t> parent;
    explicit UnionFind(int32_t n) : parent(static_cast<size_t>(n))
    {
        for (int32_t i = 0; i < n; ++i) {
            parent[static_cast<size_t>(i)] = i;
        }
    }
    int32_t Find(int32_t x)
    {
        while (parent[static_cast<size_t>(x)] != x) {
            parent[static_cast<size_t>(x)] = parent[static_cast<size_t>(parent[static_cast<size_t>(x)])];
            x = parent[static_cast<size_t>(x)];
        }
        return x;
    }
    void Union(int32_t a, int32_t b)
    {
        a = Find(a);
        b = Find(b);
        if (a == b) {
            return;
        }
        if (a < b) {
            parent[static_cast<size_t>(b)] = a;
        } else {
            parent[static_cast<size_t>(a)] = b;
        }
    }
};

// 資産の解決: ファイル資産 (guid → 現在パス → LoadFromFile) を先に試し、無ければメモリ登録
// (--fracture-demo 等、guid 相当のハッシュだけで引く FindByAssetId) を試す
const FractureAssetHandle* ResolveFractureAsset(AssetID assetId)
{
    if (assetId.IsNull()) {
        return nullptr;
    }
    FractureLibrary* lib = fracturelib::Library();
    if (lib == nullptr) {
        return nullptr;
    }
    const std::wstring path = assetguid::ResolvePath(assetId.value);
    if (!path.empty()) {
        if (const FractureAssetHandle* h = lib->LoadFromFile(path)) {
            return h;
        }
    }
    return lib->FindByAssetId(assetId);
}

struct PieceEntry {
    EntityID entity;
    EntityID parent; // 現在の直接の親 (root かリーダー)
    int32_t index;
};

// world 中の全 FracturePieceComponent を root ごとにバケットする (存在ゲート済みの
// 呼び出し元 (Update) でしか呼ばれない)
void CollectAllPieces(World& world, std::unordered_map<uint64_t, std::vector<PieceEntry>>& outByRoot)
{
    const ComponentTypeId req[] = { FracturePieceComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int fi = arch.FindTypeIndex(FracturePieceComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            const auto* fp = static_cast<const FracturePieceComponent*>(arch.GetPtr(fi, row));
            outByRoot[EntityKey(fp->root)].push_back({ e, world.GetParent(e), fp->index });
        }
    });
}

// RigidbodyComponent の質量・速度・角速度以外の欄を写す (spec §4.1 破断 4 「その他の欄は
// 元の塊の Rigidbody を写す」)
void CopyOtherRigidbodyFields(const RigidbodyComponent& src, RigidbodyComponent& dst)
{
    dst.linearDamping = src.linearDamping;
    dst.restitution = src.restitution;
    dst.gravityScale = src.gravityScale;
    dst.angularDamping = src.angularDamping;
    dst.freezeRotation = src.freezeRotation;
    dst.useDensity = src.useDensity;
    dst.gyroscopic = src.gyroscopic;
    dst.centerOfMass = src.centerOfMass;
    dst.ccd = src.ccd;
}

// 1 個の Destructible を処理する: 荷重→接着の破断→塊ごとの連結成分の作り直し→分かれた
// 成分の昇格 (spec §4.1 破断 1〜7)。myPieces は fp->root==root な現在の全破片 (順不同)
void ProcessRoot(World& world, EntityID root, DestructibleComponent& dc, const FractureAssetHandle& asset,
                 double avgNeighborArea, const std::vector<PieceEntry>& myPieces,
                 const std::vector<ShapeImpulse>& shapeImpulses, float dt)
{
    const int32_t n = static_cast<int32_t>(asset.pieces.size());
    if (n <= 1) {
        return; // 破片 1 つ (相当) は破断しない
    }
    std::vector<EntityID> entityOf(static_cast<size_t>(n), kNullEntity);
    std::vector<EntityID> parentOf(static_cast<size_t>(n), kNullEntity);
    for (const PieceEntry& p : myPieces) {
        if (p.index >= 0 && p.index < n) {
            entityOf[static_cast<size_t>(p.index)] = p.entity;
            parentOf[static_cast<size_t>(p.index)] = p.parent;
        }
    }

    // ---- 荷重 L_i = C_i + damage_i (spec §4.1 破断 1〜2) ----
    std::vector<float> load(static_cast<size_t>(n), 0.0f);
    for (int32_t i = 0; i < n; ++i) {
        if (entityOf[static_cast<size_t>(i)].IsNull()) {
            continue;
        }
        const float c = FindShapeImpulse(shapeImpulses, entityOf[static_cast<size_t>(i)]) / dt;
        float damage = 0.0f;
        if (const auto* fp = world.GetComponent<FracturePieceComponent>(entityOf[static_cast<size_t>(i)])) {
            damage = fp->damage;
        }
        load[static_cast<size_t>(i)] = c + damage;
    }

    // ---- 接着の破断: i 昇順・隣接表順、i<j のペアだけ処理し両側のビットを立てる ----
    bool anyBroken = false;
    for (int32_t i = 0; i < n; ++i) {
        if (entityOf[static_cast<size_t>(i)].IsNull()) {
            continue;
        }
        const auto& neighbors = asset.pieces[static_cast<size_t>(i)].neighbors;
        for (size_t k = 0; k < neighbors.size(); ++k) {
            const int32_t j = neighbors[k].pieceIndex;
            if (j <= i || j < 0 || j >= n || entityOf[static_cast<size_t>(j)].IsNull()) {
                continue;
            }
            auto* fpI = world.GetComponent<FracturePieceComponent>(entityOf[static_cast<size_t>(i)]);
            if (fpI == nullptr || (fpI->brokenBonds & (1u << k)) != 0) {
                continue;
            }
            const double ratio = avgNeighborArea > 0.0
                                      ? static_cast<double>(neighbors[k].area) / avgNeighborArea
                                      : 1.0;
            const double clamped = (std::max)(0.25, (std::min)(4.0, ratio));
            const float threshold = dc.strength * static_cast<float>(clamped);
            if ((std::max)(load[static_cast<size_t>(i)], load[static_cast<size_t>(j)]) < threshold) {
                continue;
            }
            fpI->brokenBonds |= (1u << k);
            if (auto* fpJ = world.GetComponent<FracturePieceComponent>(entityOf[static_cast<size_t>(j)])) {
                const auto& jNeighbors = asset.pieces[static_cast<size_t>(j)].neighbors;
                for (size_t m = 0; m < jNeighbors.size(); ++m) {
                    if (jNeighbors[m].pieceIndex == i) {
                        fpJ->brokenBonds |= (1u << m);
                        break;
                    }
                }
            }
            anyBroken = true;
        }
    }
    if (!anyBroken) {
        return;
    }

    // ---- 塊 (Rigidbody を持つ最も近い祖先 = root かリーダー) ごとに束ねる ----
    struct OwnerGroup {
        EntityID owner;
        std::vector<int32_t> localIdx; // owner を直接の親に持つ破片 (owner 自身は含まない)
    };
    std::vector<OwnerGroup> owners;
    for (int32_t i = 0; i < n; ++i) {
        if (entityOf[static_cast<size_t>(i)].IsNull()) {
            continue;
        }
        const EntityID p = parentOf[static_cast<size_t>(i)];
        OwnerGroup* found = nullptr;
        for (OwnerGroup& og : owners) {
            if (og.owner == p) {
                found = &og;
                break;
            }
        }
        if (found == nullptr) {
            owners.push_back({ p, {} });
            found = &owners.back();
        }
        found->localIdx.push_back(i);
    }

    const EntityID rootParent = world.GetParent(root);
    int32_t newLeaders = 0;

    for (OwnerGroup& og : owners) {
        const bool isRootOwner = (og.owner == root);
        int32_t ownerPieceIndex = -1;
        if (!isRootOwner) {
            for (int32_t t = 0; t < n; ++t) {
                if (entityOf[static_cast<size_t>(t)] == og.owner) {
                    ownerPieceIndex = t;
                    break;
                }
            }
            if (ownerPieceIndex < 0) {
                continue; // fracture 系ではない親に紛れた破片 (想定外) — 触らない
            }
        }
        auto* ownerRb = world.GetComponent<RigidbodyComponent>(og.owner);
        if (ownerRb == nullptr) {
            continue; // 塊のリーダー/ルートに Rigidbody が無い (想定外)
        }

        std::vector<int32_t> nodeSet = og.localIdx;
        if (!isRootOwner) {
            nodeSet.push_back(ownerPieceIndex);
        }
        if (nodeSet.size() < 2) {
            continue;
        }
        std::sort(nodeSet.begin(), nodeSet.end());

        // 未切断の接着だけで連結成分を作り直す (index 昇順の union-find)
        UnionFind uf(n);
        for (int32_t i : nodeSet) {
            const auto& neighbors = asset.pieces[static_cast<size_t>(i)].neighbors;
            for (size_t k = 0; k < neighbors.size(); ++k) {
                const int32_t j = neighbors[k].pieceIndex;
                if (j < 0 || j >= n || !std::binary_search(nodeSet.begin(), nodeSet.end(), j)) {
                    continue;
                }
                const auto* fpI = world.GetComponent<FracturePieceComponent>(entityOf[static_cast<size_t>(i)]);
                if (fpI != nullptr && (fpI->brokenBonds & (1u << k)) == 0) {
                    uf.Union(i, j);
                }
            }
        }

        std::vector<std::pair<int32_t, std::vector<int32_t>>> comps; // (代表 index, 昇順メンバー)
        std::unordered_map<int32_t, size_t> compIndexOf;
        for (int32_t i : nodeSet) {
            const int32_t r = uf.Find(i);
            const auto it = compIndexOf.find(r);
            if (it == compIndexOf.end()) {
                compIndexOf[r] = comps.size();
                comps.push_back({ r, { i } });
            } else {
                comps[it->second].second.push_back(i);
            }
        }
        if (comps.size() <= 1) {
            continue; // この塊は今回は分かれていない
        }

        auto compVolume = [&](const std::vector<int32_t>& members) {
            double v = 0.0;
            for (int32_t idx : members) {
                v += asset.pieces[static_cast<size_t>(idx)].volume;
            }
            return v;
        };
        auto compCenter = [&](const std::vector<int32_t>& members) {
            double vx = 0.0, vy = 0.0, vz = 0.0, vsum = 0.0;
            for (int32_t idx : members) {
                float px, py, pz, qx, qy, qz, qw;
                ComposeEntityWorldPose(world, entityOf[static_cast<size_t>(idx)], px, py, pz, qx, qy, qz, qw);
                const double vol = asset.pieces[static_cast<size_t>(idx)].volume;
                vx += vol * px;
                vy += vol * py;
                vz += vol * pz;
                vsum += vol;
            }
            XMFLOAT3 c{ 0.0f, 0.0f, 0.0f };
            if (vsum > 0.0) {
                c = { static_cast<float>(vx / vsum), static_cast<float>(vy / vsum), static_cast<float>(vz / vsum) };
            }
            return c;
        };

        // 残留成分の決定: root は体積最大 (先着優先で同値は index 最小側)、
        // リーダーはリーダー自身を含む成分
        size_t stayIdx = 0;
        if (isRootOwner) {
            double bestVol = -1.0;
            for (size_t c = 0; c < comps.size(); ++c) {
                const double v = compVolume(comps[c].second);
                if (v > bestVol) {
                    bestVol = v;
                    stayIdx = c;
                }
            }
        } else {
            for (size_t c = 0; c < comps.size(); ++c) {
                if (std::binary_search(comps[c].second.begin(), comps[c].second.end(), ownerPieceIndex)) {
                    stayIdx = c;
                    break;
                }
            }
        }

        const double ownerOldVolume = compVolume(nodeSet);
        const XMFLOAT3 ownerOldCenter = compCenter(nodeSet);
        const XMFLOAT3 ownerOldVel = ownerRb->velocity;
        const XMFLOAT3 ownerOldOmega = ownerRb->angularVelocity;
        const float ownerOldMass = ownerRb->mass;
        const bool ownerUseDensity = ownerRb->useDensity;
        const RigidbodyComponent ownerOldRb = *ownerRb; // 他欄コピー用の分離前スナップショット

        // 分離した瞬間の点の速度: v_owner + ω × (center − ownerCenter) (剛体の速度場)
        auto velocityAt = [&](const XMFLOAT3& center) {
            const float dx = center.x - ownerOldCenter.x;
            const float dy = center.y - ownerOldCenter.y;
            const float dz = center.z - ownerOldCenter.z;
            const float ox = ownerOldOmega.y * dz - ownerOldOmega.z * dy;
            const float oy = ownerOldOmega.z * dx - ownerOldOmega.x * dz;
            const float oz = ownerOldOmega.x * dy - ownerOldOmega.y * dx;
            return XMFLOAT3{ ownerOldVel.x + ox, ownerOldVel.y + oy, ownerOldVel.z + oz };
        };

        // ---- 残留成分: 質量と速度だけ更新する (owner 自身・ワールド姿勢は不変) ----
        // ★速度は「体積比で軽くなった分だけ mass は変わるが v はそのまま」ではない —
        //   RigidbodyComponent.velocity は複合の重心の速度 (PhysicsSystem.cpp の位置積分参照)
        //   なので、重心が動いた分だけここでも同じ式で引き直さないと運動量が保存しない
        {
            const double stayVol = compVolume(comps[stayIdx].second);
            if (!ownerUseDensity && ownerOldVolume > 0.0) {
                ownerRb->mass = static_cast<float>(ownerOldMass * (stayVol / ownerOldVolume));
            }
            const XMFLOAT3 stayCenter = compCenter(comps[stayIdx].second);
            ownerRb->velocity = velocityAt(stayCenter);
        }

        // ---- 分かれた成分ごとに index 最小の破片をリーダーへ昇格する ----
        for (size_t c = 0; c < comps.size(); ++c) {
            if (c == stayIdx) {
                continue;
            }
            const std::vector<int32_t>& members = comps[c].second; // 昇順
            const int32_t leaderIndex = members.front();
            const EntityID leaderEntity = entityOf[static_cast<size_t>(leaderIndex)];

            const double compVol = compVolume(members);
            const XMFLOAT3 newVel = velocityAt(compCenter(members));

            ReparentKeepWorld(world, leaderEntity, rootParent);
            auto* leaderRb = world.AddComponent<RigidbodyComponent>(leaderEntity);
            CopyOtherRigidbodyFields(ownerOldRb, *leaderRb);
            leaderRb->velocity = newVel;
            leaderRb->angularVelocity = ownerOldOmega;
            leaderRb->isKinematic = false; // 分かれた塊は常に dynamic (spec §4.1 破断 6)
            leaderRb->compoundColliders = members.size() >= 2;
            if (!ownerUseDensity && ownerOldVolume > 0.0) {
                leaderRb->mass = static_cast<float>(ownerOldMass * (compVol / ownerOldVolume));
            }

            for (int32_t m : members) {
                if (m != leaderIndex) {
                    ReparentKeepWorld(world, entityOf[static_cast<size_t>(m)], leaderEntity);
                }
            }

            if (auto* leaderFp = world.GetComponent<FracturePieceComponent>(leaderEntity)) {
                leaderFp->releaseTicks = 0;
            }
            ++newLeaders;
        }
    }

    if (newLeaders > 0) {
        dc.broken = true;
        dc.detachedCount += newLeaders;
    }
}

// 今のワールド状態 (myPieces = fp->root==root な全破片。呼び出し側が階層を見ずに集めたもの) が
// 資産と整合するか。**階層は一切見ない** — 分離済みの破片はルートの親の下へ移った別エンティティ
// の子になっているため、階層を辿ると割れた後の破片を見失う。
// broken==false: index が重複せず範囲内で、個数が資産の破片数と一致すること (まだ何も
// 分かれていないので全部そろっているはず)。
// broken==true: index が重複せず範囲内であればよい (割れた後の後始末で塊が Destroy され、
// 個数が減ってよい)。食い違えば理由を outReason へ書いて false を返す
bool PiecesMatchAssetNow(const FractureAssetHandle& asset, const std::vector<PieceEntry>* myPieces,
                         bool broken, std::string* outReason)
{
    const size_t n = asset.pieces.size();
    std::vector<bool> seen(n, false);
    const size_t count = myPieces ? myPieces->size() : 0;
    for (size_t i = 0; i < count; ++i) {
        const int32_t idx = (*myPieces)[i].index;
        if (idx < 0 || static_cast<size_t>(idx) >= n || seen[static_cast<size_t>(idx)]) {
            if (outReason != nullptr) {
                *outReason = "fracture piece index is out of range or duplicated";
            }
            return false;
        }
        seen[static_cast<size_t>(idx)] = true;
    }
    if (!broken && count != n) {
        if (outReason != nullptr) {
            *outReason = "piece count mismatch";
        }
        return false;
    }
    return true;
}

} // namespace

bool AnyDestructibles(World& world)
{
    bool any = false;
    const ComponentTypeId req[] = { DestructibleComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& a) { any = any || a.Count() != 0; });
    return any;
}

bool ShouldHideUnbrokenFracturePiece(const DestructibleComponent* rootDestructible)
{
    return rootDestructible != nullptr && !rootDestructible->broken;
}

void FractureSystem::Update(World& world, float dt, const std::vector<ShapeImpulse>& shapeImpulses)
{
    if (dt <= 0.0f) {
        return;
    }
    // ★AddComponent<RigidbodyComponent> を tick 末のコマンドバッファへ積ませるため、処理本体を
    //   ForEachArchetype のコールバック内 (= World::IsIterating()==true) で走らせる。
    //   ここで使う型は何でもよい — DestructibleComponent が非空であることが「呼ぶ価値がある」
    //   の判定そのものなので、ゲートと iteration の確保を 1 回の走査に相乗りさせている
    const ComponentTypeId gateReq[] = { DestructibleComponent::sTypeId };
    bool ran = false;
    world.ForEachArchetype(gateReq, [&](Archetype&) {
        if (ran) {
            return;
        }
        ran = true;
        UpdateImpl(world, dt, shapeImpulses);
    });
}

void FractureSystem::UpdateImpl(World& world, float dt, const std::vector<ShapeImpulse>& shapeImpulses)
{
    std::unordered_map<uint64_t, std::vector<PieceEntry>> piecesByRoot;
    CollectAllPieces(world, piecesByRoot);

    struct RootJob {
        EntityID root;
        DestructibleComponent* dc;
    };
    std::vector<RootJob> jobs;
    const ComponentTypeId req[] = { DestructibleComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int di = arch.FindTypeIndex(DestructibleComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (!IsEntityActive(world, e)) {
                continue;
            }
            jobs.push_back({ e, static_cast<DestructibleComponent*>(arch.GetPtr(di, row)) });
        }
    });

    static const std::vector<PieceEntry> kNoPieces;
    for (RootJob& job : jobs) {
        const uint64_t key = EntityKey(job.root);

        // ---- 資産の解決だけキャッシュする (資産参照から一意に決まる値なので安全) ----
        const FractureAssetHandle* handle = nullptr;
        const auto assetIt = assetCache_.find(key);
        if (assetIt != assetCache_.end()) {
            handle = assetIt->second.handle;
        } else {
            handle = ResolveFractureAsset(job.dc->fractureAsset);
            if (handle != nullptr) {
                AssetCache cache;
                cache.handle = handle;
                double sum = 0.0;
                int64_t count = 0;
                for (const auto& piece : handle->pieces) {
                    for (const auto& nb : piece.neighbors) {
                        sum += nb.area;
                        ++count;
                    }
                }
                cache.avgNeighborArea = count > 0 ? sum / static_cast<double>(count) : 0.0;
                assetCache_.emplace(key, cache);
            }
        }
        if (handle == nullptr) {
            if (erroredOnce_.insert(key).second) {
                MYE_LOG_ERROR("[fracture] %s: fracture asset is not loaded, this destructible will not break",
                             world.GetName(job.root));
            }
            continue;
        }

        // ---- ここから先は毎 tick、今のワールド状態から検証する (階層は見ない) ----
        const auto pieceIt = piecesByRoot.find(key);
        const std::vector<PieceEntry>& myPieces = (pieceIt != piecesByRoot.end()) ? pieceIt->second : kNoPieces;
        std::string reason;
        const bool piecesOk = PiecesMatchAssetNow(*handle, &myPieces, job.dc->broken, &reason);
        const bool hasRigidbody = world.GetComponent<RigidbodyComponent>(job.root) != nullptr;
        if (!piecesOk || !hasRigidbody) {
            if (erroredOnce_.insert(key).second) {
                if (!piecesOk) {
                    MYE_LOG_ERROR("[fracture] %s: %s (asset pieces=%zu, entities=%zu) - this destructible "
                                 "will not break",
                                 world.GetName(job.root), reason.c_str(), handle->pieces.size(),
                                 myPieces.size());
                } else {
                    MYE_LOG_ERROR("[fracture] %s: root has no Rigidbody, this destructible will not break",
                                 world.GetName(job.root));
                }
            }
            continue;
        }

        const double avgNeighborArea = assetCache_.find(key)->second.avgNeighborArea;
        ProcessRoot(world, job.root, *job.dc, *handle, avgNeighborArea, myPieces, shapeImpulses, dt);
    }
}

void ApplyFractureDamage(World& world, EntityID entityOrPiece, const XMFLOAT3& point, float radius,
                         float amount)
{
    if (!world.IsAlive(entityOrPiece)) {
        return;
    }
    EntityID root = kNullEntity;
    if (world.GetComponent<DestructibleComponent>(entityOrPiece) != nullptr) {
        root = entityOrPiece;
    } else if (const auto* fp = world.GetComponent<FracturePieceComponent>(entityOrPiece)) {
        root = fp->root;
    } else {
        return;
    }
    if (root.IsNull() || !world.IsAlive(root)) {
        return;
    }

    struct Cand {
        EntityID entity;
        float dist;
    };
    std::vector<Cand> cands;
    const ComponentTypeId req[] = { FracturePieceComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int fi = arch.FindTypeIndex(FracturePieceComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            const auto* fp = static_cast<const FracturePieceComponent*>(arch.GetPtr(fi, row));
            if (fp->root != root) {
                continue;
            }
            float px, py, pz, qx, qy, qz, qw;
            ComposeEntityWorldPose(world, e, px, py, pz, qx, qy, qz, qw);
            const float dx = px - point.x, dy = py - point.y, dz = pz - point.z;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (radius <= 0.0f || d <= radius) {
                cands.push_back({ e, d });
            }
        }
    });
    if (cands.empty()) {
        return;
    }
    if (radius <= 0.0f) {
        // 最寄りの 1 つへ amount をそのまま (同値は entity index 昇順で決定的に選ぶ)
        size_t best = 0;
        for (size_t i = 1; i < cands.size(); ++i) {
            if (cands[i].dist < cands[best].dist
                || (cands[i].dist == cands[best].dist && cands[i].entity.index < cands[best].entity.index)) {
                best = i;
            }
        }
        if (auto* fp = world.GetComponent<FracturePieceComponent>(cands[best].entity)) {
            fp->damage += amount;
        }
        return;
    }
    for (const Cand& c : cands) {
        const float falloff = 1.0f - (c.dist / radius);
        if (auto* fp = world.GetComponent<FracturePieceComponent>(c.entity)) {
            fp->damage += amount * falloff;
        }
    }
}

} // namespace mye
