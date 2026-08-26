//====================================================================================
//                          XpbdBackend.h
//  MyEngine/ 秋田蓮音                                                      08/27/2026
//                                          XPBD 変形体の粒子池（ECS 外 sim 状態の 2 例目）
//====================================================================================
#pragma once
#include <cstdint>
#include <vector>

#include "Engine/Core/EntityID.h"

namespace mye {

class World;

// XPBD 変形体 (ロープ / 布 / ソフトボディ) の粒子池 (M60'b、計画 supple-weaving-loom)。
//
// ★「ECS 外の sim 状態」の 2 例目 (1 例目 = CpuParticleBackend)。粒子数がオーサリングに
//   依存して可変なので、コンポーネント常駐 (M59h の家風) には物理的に置けない —
//   これが予約事項 1 の「ステートフルバックエンドの箱を開ける」の実体。
// ★sim 状態は必ず 3 点セットで運ぶこと:
//   池 (ここ) + ハッシュ節 (WorldHasher の HashXpbdPools) + snapshot 節 (SimSnapshot v4)。
//   片方だけ足すと「リプレイは通るのに巻き戻し/ロールバックで割れる」型のバグになる。
// M60'b は器と配線のみ (何もシミュしない)。ソルバは M60'c の XpbdSolver が持つ。
class XpbdBackend {
public:
    // 池の種別 (オーサリング元のコンポーネント)。値はハッシュと blob に生で入るので
    // 変更・並べ替え不可 (TypeId と同じ「追加は末尾」規約)
    enum class PoolKind : uint32_t { Rope = 0, Cloth = 1, SoftBody = 2 };

    // 1 コンポーネント = 1 池。粒子は SoA (ソルバの走査が列単位なので)。
    // ★ここに member を足したら HashXpbdPools と SimSnapshot の Write/ReadXpbd の
    //   両方へ同時に足すこと (3 点セット契約)。blob レイアウトが変わるので
    //   kSimSnapshotVersion の bump も忘れない
    struct Pool {
        EntityID owner = kNullEntity;
        uint32_t kind = 0; // PoolKind。enum 直置ちにしないのは blob へ生バイトで書くため
        // 粒子状態 (全てハッシュ + snapshot 対象)
        std::vector<float> px, py, pz;          // 位置 (ワールド)
        std::vector<float> vx, vy, vz;          // 速度
        std::vector<float> prevX, prevY, prevZ; // substep 冒頭の位置 (XPBD の速度更新用)
        std::vector<float> invMass;             // 逆質量。0 = ピン留め
        // 距離拘束 (M60'c から使用)。rest は塑性 (M60'l) が書き換えるので導出値ではなく状態
        std::vector<uint32_t> ca, cb; // 粒子 index のペア
        std::vector<float> rest;      // 自然長
    };

    // コンポーネントの有無から池を生成/破棄する (owner.index 昇順を維持)。
    // CpuParticleBackend::SyncEmitters と同じ立ち位置。M60'b 時点では対象
    // コンポーネントが存在しないので何もしない (M60'c の Rope から実装)
    void Sync(World& world);

    // シーン遷移時の全消し (TickRunner の LoadScene 反映ブロックから呼ばれる)
    void Reset() { pools_.clear(); }

    const std::vector<Pool>& Pools() const { return pools_; }
    // sim スナップショット / セルフテスト用の可変アクセス。**SimSnapshot と selftest
    // 以外から書き換えないこと** (CpuParticleBackend::PoolsForSnapshot と同じ契約)
    std::vector<Pool>& PoolsForSnapshot() { return pools_; }

private:
    std::vector<Pool> pools_; // owner.index 昇順 (決定論)
};

} // namespace mye
