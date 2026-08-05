#pragma once
#include <cstdint>
#include <unordered_set>

namespace mye {

class World;
struct RenderResources;

// 部位のボーン追従 (M48g)。`PartComponent.joint` が非空の部位を、供給元 SkinnedMesh の
// そのジョイントへ毎 tick 貼り付ける。**LocalTransform を書くので sim 状態 = ハッシュ対象**。
//
// ---- 追従の式 (M48a の実測結論) ----
// 部位のワールド = `jointGlobal * sourceWorld`。一方 ECS では
// 部位のワールド = `partLocal * parentWorld` なので、**部位が source の直子**であれば
//   partLocal = jointGlobal
// で閉じる。ワールド行列を一切読まずに済むのが要点 —
// WorldMatrixComponent は前 tick の TransformSystem が書いた値で、このシステムは
// TransformSystem より前に走る。**読んだ瞬間に 1 tick 古い値への依存**が生まれ、
// 順序を動かすと結果が変わる脆い実装になる。だから v1 は「直子に置く」規約で回避する。
//
// ---- v1 の規約と挙動 ----
//   - 部位は source エンティティの **直子** に置くこと。そうでなければ warn して skip
//   - `PartComponent.source` が未指定なら **最も近い SkinnedMesh 祖先** を使う
//     (= 直子規約のもとでは親そのもの)
//   - `joint` が空 = 静的ソケット → 何もしない (親に対して固定)
//   - モデル未登録 / ジョイント名が無い → 前回値を保持して skip (初回だけ warn)
//   - 編集中は動かさない (EngineLoop の `simulateScripts` ゲート。Animator と同じ家風)
class PartFollowSystem {
public:
    // skinningSystem.Update の直後・物理と TransformSystem の前に呼ぶ
    // (同じ tick の WorldMatrix に反映させるため)
    void Update(World& world, const RenderResources& resources);

    // シーン切替時に「warn 済み」の記録を捨てる (別シーンで同じ問題は再度知らせたい)
    void Reset() { warned_.clear(); }

private:
    std::unordered_set<uint64_t> warned_; // 警告済みキー (entity index | 理由) — ログ抑制のみ
};

} // namespace mye
