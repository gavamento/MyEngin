#pragma once
#include <vector>

#include "Engine/Core/EntityID.h"
#include "Engine/Engine/Physics/PhysicsSystem.h" // SolidContact

namespace mye {

class World;
class ScriptHost;
class ManagedHost;

// 衝突イベント配信 (M7 トリガー / M28a 形状拡張 / M28c ソリッドイベント)。
// - トリガー: 毎 tick、Transform 確定後に総当たりで重なり判定し、前 tick とのペア差分から
//   OnTriggerEnter / OnTriggerExit を配信する。**ペアの少なくとも片方が isTrigger** のものだけ
//   (M28c で是正 — ソリッド同士は OnCollision 系に移管)。
// - ソリッド: PhysicsSystem が出力した接触ペア列 (key 昇順) を前 tick と差分し、
//   OnCollisionEnter (法線付き) / OnCollisionStay / OnCollisionExit を配信する。
// 配信順: トリガー Enter → Exit → ソリッド Enter → Stay → Exit、各リスト key 昇順 (決定論)。
// C++ (ScriptHost) と C# (ManagedHost) の両レーンへ配信。形状判定は Physics/Shapes.cpp に統合
class CollisionSystem {
public:
    // managed 非 null のとき C# スクリプトにも配信する (別レーン、記録/検証中は null)。
    // solidContacts は PhysicsSystem::Update の出力 (物理が走らない場合は null で可)
    void Update(World& world, ScriptHost* scripts, ManagedHost* managed = nullptr,
                const std::vector<SolidContact>* solidContacts = nullptr);
    void Reset()
    {
        prevPairs_.clear();
        prevSolidPairs_.clear();
    }

    // テスト / デバッグ用の観測点 (非ハッシュ・決定論)。直近 Update で配信したキー列
    const std::vector<uint64_t>& LastTriggerEnter() const { return trigEnter_; }
    const std::vector<uint64_t>& LastTriggerExit() const { return trigExit_; }
    const std::vector<uint64_t>& LastCollisionEnter() const { return solidEnter_; }
    const std::vector<uint64_t>& LastCollisionStay() const { return solidStay_; }
    const std::vector<uint64_t>& LastCollisionExit() const { return solidExit_; }

private:
    std::vector<uint64_t> prevPairs_;      // トリガー: (aIdx<<32)|bIdx, aIdx<bIdx、昇順
    std::vector<uint64_t> prevSolidPairs_; // ソリッド: 同上 (前 tick の接触ペア)
    // 直近 Update の配信内容 (観測用)
    std::vector<uint64_t> trigEnter_, trigExit_;
    std::vector<uint64_t> solidEnter_, solidStay_, solidExit_;
};

} // namespace mye
