//====================================================================================
//                          FractureSystem.h
//  MyEngin/ 秋田蓮音                                                     09/25/2026
//                                          接着の破断・塊の分離・kinematicルート (M80g)
//====================================================================================
#pragma once
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/EntityID.h"

namespace mye {

class World;
class ScriptHost;
class ManagedHost;
struct ShapeImpulse;
struct FractureAssetHandle;
struct DestructibleComponent;

// 分かれた塊 1 つぶんの onBreak 通知 (spec §4.1 破断 8、M80l)。point/impulse はその塊で
// 荷重最大の破片の原点 (ワールド) と荷重 [N]。非ハッシュ・決定論の観測値
// (LastBreakEvents 用) — スクリプトへの実配信は別途 ScriptHost/ManagedHost::DispatchBreak
struct FractureBreakEvent {
    EntityID root;                  // Destructible のルート
    EntityID leader;                // 分かれた塊の新リーダー (index 最小の破片)
    DirectX::XMFLOAT3 point{ 0.0f, 0.0f, 0.0f };
    float impulse = 0.0f;
};

// 接着の破断と塊の剛体化 (spec §4.1「破断」1〜7、M80g)。TickRunner が
// collisionSystem::Update の後・tick 末の ApplyStructuralChanges より前に 1 回呼ぶ。
//
// 荷重: 破片 i の C_i = 形状単位インパルス(shapeImpulses から引く)/dt、L_i = C_i + damage_i。
// 接着 (i,j) は i 昇順・隣接表順に見て max(L_i,L_j) >= S_ij で両側の brokenBonds ビットを立てる。
// 塊 (Rigidbody を持つ最も近い祖先 = ルートか、以前に昇格したリーダー) ごとに、未切断の
// 接着で連結成分を作り直し (index 昇順の union-find)、2 つ以上に分かれたら分かれた成分ごとに
// index 最小の破片をリーダーへ昇格して root の親の下へ移す (ワールド姿勢を保つ)。
// 質量・速度は「元の塊 (root かリーダー) の値 × 体積比／モーメント」から導く (§4.1 5〜6)。
//
// 構造変更 (SetParent / AddComponent<RigidbodyComponent>) はすべて tick 末のコマンドバッファへ
// 積む — Update() 自身が ForEachArchetype のコールバック内で処理するため、呼び出し中に
// アーキタイプは一切移動しない (同 tick の他 Destructible の処理中に取ったポインタも無効化されない)。
//
// 存在ゲート: ワールドに DestructibleComponent が 1 つも無ければ何もしない。
// 資産が解決できない / 破片数が資産と食い違う / ルートに Rigidbody が無い Destructible は、
// 最初に検出した tick で ERROR を 1 回だけ出す。**「無効かどうか」自体はキャッシュしない**
// — 分離済みの破片はルートの親の下へ移った別エンティティの子になるため、検証は毎 tick
// 階層を見ずに今のワールド状態から行う (spec §2/§4.4)。キャッシュしてよいのは資産
// (fractureAsset) から一意に決まる値 (解決したハンドル、資産全体の隣接面積の平均) だけ。
//
// 割れた後の後始末 (M80h、spec §4.1「割れた後」): 分かれた塊のリーダー (FracturePieceComponent.
// releaseTicks >= 0) ごとに releaseTicks を毎 tick +1 し、Destructible.afterBreak (0..5) に
// 従って Destroy / Collider.mask クリア / scale 縮小 / Rigidbody 除去 / 上限超過削除を行う。
// **今回新しく分かれたリーダー (releaseTicks が今 tick に 0 になったもの) は対象外** — その
// 塊の階層 (メンバーの再親付け) はまだ tick 末のコマンドバッファに積まれただけで、
// World::GetParent 経由の走査に反映されるのは ApplyStructuralChanges の後。判定を次 tick から
// 始めることで、常に階層が揃った状態の塊だけを辿る。
class FractureSystem {
public:
    // scripts/managed が非 null なら、分かれた塊ごとに root にあるスクリプトへ onBreak を
    // 配信する (ルート index → 新リーダー index 昇順、spec §4.1 破断 8)。どちらも省略可
    // (--fracture-bench 等のヘッドレス計測はスクリプトを持たないため)
    void Update(World& world, float dt, const std::vector<ShapeImpulse>& shapeImpulses,
               ScriptHost* scripts = nullptr, ManagedHost* managed = nullptr);

    // シーン切替時にキャッシュを捨てる (PartFollowSystem::Reset と同じ流儀)。
    // assetCache_ は資産参照から再計算すれば同じ値に戻るだけの記録、erroredOnce_ は
    // ログの抑止だけなので、どちらを捨てても sim 結果は変わらない
    void Reset()
    {
        assetCache_.clear();
        erroredOnce_.clear();
    }

    // 直近 Update で発行した onBreak の記録 (観測用。非ハッシュ・決定論)。
    // SelfTest がスクリプトを経由せずに「誰が・どこで・どれだけの荷重で分かれたか」を検算する
    const std::vector<FractureBreakEvent>& LastBreakEvents() const { return lastBreakEvents_; }

private:
    struct AssetCache {
        const FractureAssetHandle* handle = nullptr; // 資産参照から一意に決まる (解決結果)
        double avgNeighborArea = 0.0;                // 資産全体の隣接面積の平均
    };
    // ForEachArchetype のコールバック内 (= 構造変更が tick 末まで遅延される状態) から呼ぶ本体
    void UpdateImpl(World& world, float dt, const std::vector<ShapeImpulse>& shapeImpulses,
                    ScriptHost* scripts, ManagedHost* managed);

    std::unordered_map<uint64_t, AssetCache> assetCache_; // root -> 解決済み資産 (解決できた分だけ)
    std::unordered_set<uint64_t> erroredOnce_;             // ERROR を 1 回だけ出すためのログ抑止 (判定には使わない)
    std::vector<FractureBreakEvent> lastBreakEvents_;      // 直近 Update の onBreak 記録 (観測用)
};

// ワールドに DestructibleComponent が 1 つでもあるか。**存在ゲート**専用 — false なら
// 呼び側は形状単位インパルスの出力ポインタを渡さず (null)、FractureSystem::Update も呼ばない
bool AnyDestructibles(World& world);

// root proxy の可視規則 (RenderSystem::CollectDrawables と共有)。rootDestructible (破片の
// root が持つ Destructible、見つからなければ nullptr) が非 null かつ未破断のときだけ
// 「まだ割れていないので破片側を隠す」。rootDestructible が見つからない (ルートが Destroy
// された / 参照切れ) ときは**隠す対象が無いので描く側**にする — 逆にすると、分離後に
// スクリプトがルートを消しただけで分かれた破片まで消える
bool ShouldHideUnbrokenFracturePiece(const DestructibleComponent* rootDestructible);

// スクリプトの ApplyFractureDamage が呼ぶ Engine 層の内部関数 (ABI 経由ではまだ呼べない)。
// entityOrPiece は Destructible のルートか、その破片のどちらでもよい。root 配下の全破片から
// point (ワールド座標) から radius 以内のものへ amount*(1-d/radius) を加算する。
// radius<=0 は最寄りの1破片へ amount をそのまま加算する。呼んだ瞬間に damage へ書く (即時・同期)
void ApplyFractureDamage(World& world, EntityID entityOrPiece, const DirectX::XMFLOAT3& point,
                          float radius, float amount);

} // namespace mye
