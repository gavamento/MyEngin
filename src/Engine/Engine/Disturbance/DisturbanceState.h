//====================================================================================
//                          DisturbanceState.h
//  MyEngin/ 秋田蓮音                                                       11/08/2026
//                                          かき乱しフィールド (M96) の sim 状態と、1 tick の step・読み取りの入口
//====================================================================================
#pragma once

#include <cstdint>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Ecs/EntityID.h"

namespace mye {

class World;
class Scene;
class ByteWriter;
class ByteReader;
struct DebugLineCmd;

// 同時に sim する場の数。entity index の昇順で先頭 4 個だけ (5 個目以降は sim しない)
inline constexpr uint32_t kMaxDisturbanceFields = 4;
// 配送待ちインパルスのキューの長さ
inline constexpr uint32_t kMaxDisturbanceQueue = 256;
// 1 つの場のマス数の上限
inline constexpr int32_t kMaxDisturbanceCells = 262144;
// 1 辺のマス数の範囲
inline constexpr int32_t kMinDisturbanceSide = 2;
inline constexpr int32_t kMaxDisturbanceSide = 512;
// 波紋の高さの上限 [m]
inline constexpr float kMaxRippleHeight = 2.0f;
// 波紋のクーラン数の上限 (2D の安定条件 1/sqrt(2) 未満)
inline constexpr float kMaxRippleCourant = 0.7f;
// 圧力投影の反復回数の上限と、風の倍率の上限
inline constexpr int32_t kMaxPressureIterations = 64;
inline constexpr float kMaxWindScale = 10.0f;
// 浮力の上限 [m/s^2 / 濃さ]
inline constexpr float kMaxBuoyancy = 20.0f;
// sim の周波数。cellSize × この値が流れの速さの上限 (1 tick に 1 マス)
inline constexpr float kDisturbanceTickRate = 60.0f;
// 霧の嵩 (静止状態 1) の上限。押し寄せた縁でもこれ以上は積もらない
inline constexpr float kMaxFogMass = 4.0f;
// 嵩の重力の係数の上限 [1/tick]。広がりの係数は massGravity × 嵩なので、陽解法の拡散の安定条件
// (係数 <= 0.25) を嵩の上限でも守る値 = 0.25 / kMaxFogMass
inline constexpr float kMaxMassGravity = 0.25f / kMaxFogMass;
// 嵩チャネルの列の層の厚さの下限 [m] (嵩 0 の列の p(y) で 0 で割らない)。
// assets\shaders\disturbance_common.hlsli の MYE_DFLD_MIN_COLUMN_THICKNESS と一致させる (FieldSample と描画で同じ式)
inline constexpr float kMinColumnThickness = 0.001f;

enum DisturbanceChannel : uint32_t {
    kDisturbanceChannelDensity = 1u,
    kDisturbanceChannelTrail = 2u,
    kDisturbanceChannelRipple = 4u,
    kDisturbanceChannelMass = 8u, // 霧の嵩 (保存して運ばれ、重力で平らに戻る。GroundLayer だけ)
    kDisturbanceChannelMask = 15u,
};

enum DisturbanceShape : int32_t {
    kDisturbanceGroundLayer = 0,
    kDisturbanceVolume = 1,
};

enum FieldImpulseShape : uint32_t {
    kFieldImpulsePoint = 0,  // dir 向きの一様な流れ
    kFieldImpulseRadial = 1, // 点から外向き (中心で 0)
    kFieldImpulseCone = 2,   // dir を軸とする円錐の中だけの Point
};

// 配送待ちのインパルス 1 件。全欄 4 バイトで詰め物が無い (ハッシュとスナップショットは生バイトで扱う)
struct FieldImpulse {
    float pos[3] = { 0.0f, 0.0f, 0.0f };
    float dir[3] = { 0.0f, 0.0f, 1.0f };
    float radius = 1.0f;   // [m]
    float speed = 0.0f;    // [m/s]
    uint32_t shape = kFieldImpulsePoint;
    float coneCos = 0.0f;  // Cone の半角の cos
    float density = 0.0f;  // [-1,1] 負 = 晴らす / 正 = 足す
    float trail = 0.0f;    // [0,1]
    float ripple = 0.0f;   // [m]
};
static_assert(sizeof(FieldImpulse) == 52, "FieldImpulse: no implicit padding");

// 場 1 つの状態。配列は index = x + cellsX * (z + cellsZ * y) の並びで、有効なチャネルの分だけ確保する。
// GroundLayer は y = 0 だけ。セル中心のワールド座標は (originCell + (x, y, z) + 0.5) * cellSize
struct DisturbanceSlot {
    EntityID entity = kNullEntity;
    int32_t shape = kDisturbanceGroundLayer;
    int32_t cellsX = 0;
    int32_t cellsY = 0;
    int32_t cellsZ = 0;
    uint32_t channels = 0;
    float cellSize = 0.0f;
    int32_t originCell[3] = { 0, 0, 0 }; // 格子の最小角のセル番号 (整数。追従で整数セル単位にずらす)
    float baseY = 0.0f;                  // GroundLayer の層の下端の高さ [m]
    std::vector<float> flowX;            // 流れ [m/s]
    std::vector<float> flowZ;
    std::vector<float> flowY;            // Volume だけ (GroundLayer は空)。y 方向の流れ [m/s]
    // 地形に沿う GroundLayer (followTerrain) だけ。列 (x, z) ごとの層の下端の高さ [m、絶対]。空 = 平らな層 (baseY 一定)。
    // 地形のコライダーから焼いた値 (作り直しと追従で入ってきた列だけ焼く。以後は地形を動かしても追わない)
    std::vector<float> columnBaseY;
    std::vector<float> density;          // [0,1]
    std::vector<float> trail;            // [0,1]
    std::vector<float> ripple;           // 波紋の高さ h [m]
    std::vector<float> ripplePrev;       // 1 tick 前の h
    // 霧の嵩 m [0, kMaxFogMass] (静止状態 1)。列の層の厚さ = thickness × m。流れで保存して運ばれ (押しのけた分が縁に積もる)、
    // 重力で高い所から低い所へ流れて平らに戻る。他のチャネルと違い静止状態へ「戻す」項は持たない
    std::vector<float> mass;

    uint32_t CellCount() const
    {
        return static_cast<uint32_t>(cellsX) * static_cast<uint32_t>(cellsY) * static_cast<uint32_t>(cellsZ);
    }
};

// Scene が持つ sim 状態 (WorldHash 対象の内容ゲート・SimSnapshot の DFLD 節)。
// 場のコンポーネントを持つエンティティから step が作り、消えたら捨てる。Play の開始 / 終了とシーン切り替えで Clear する
class DisturbanceState {
public:
    // 全消去 (配送待ちも、警告済みの記録も)
    void Clear();

    // 何か入っている状態か (= ハッシュに節を畳むか)。場のスロットも配送待ちも無ければ偽
    bool HasHashableState() const { return !slots_.empty() || !queue_.empty(); }

    // 配送待ちに積む。満杯または値が不正なら積まず false (満杯の WARN は step ごとに 1 回)
    bool QueueImpulse(const FieldImpulse& impulse);

    // 流れのソルバの作業領域 (圧力・発散・移流の書き込み先)。状態ではない。毎 tick 確保し直さないよう持ち続ける
    struct SolverScratch {
        std::vector<float> flowX;
        std::vector<float> flowZ;
        std::vector<float> pressureA;
        std::vector<float> pressureB;
        std::vector<float> divergence;
        std::vector<float> zeroRow;    // 格子外の圧力 (0) を行として渡すための行
        // Radial のインパルスがこの step で注入した速度の差分 (場ごと)。投影の右辺から発散を引く発散源 (爆発)。
        // radialActive が偽の場は使わない (配列は触らない)
        std::vector<float> radialX[kMaxDisturbanceFields];
        std::vector<float> radialZ[kMaxDisturbanceFields];
        std::vector<float> radialY[kMaxDisturbanceFields]; // Volume だけ
        std::vector<float> flowY;                          // Volume の移流の書き込み先
        std::vector<float> targetRowY;                     // 格子外の流れ (目標速度の y) を行として渡すための行
        bool radialActive[kMaxDisturbanceFields] = {};
        std::vector<float> targetRowZ; // 格子外の流れ (目標速度の z) を行として渡すための行
        std::vector<float> density;
        std::vector<float> mass;      // 嵩の移流・重力の書き込み先
        std::vector<float> massLimit; // 嵩の移流: マスごとの流出の縮め率 (出る量が嵩を超えないように)
    };

    const std::vector<DisturbanceSlot>& Slots() const { return slots_; }
    std::vector<DisturbanceSlot>& Slots() { return slots_; }
    const std::vector<FieldImpulse>& Queue() const { return queue_; }
    std::vector<FieldImpulse>& Queue() { return queue_; }

    // 描画用の通番 (状態ではない。ハッシュ・スナップショットに入れない)。step のたびに +1
    uint64_t RenderSerial() const { return renderSerial_; }

    // 置き換わる側 (previous) の通番を引き継いで +1 する。復元で新品へ読んだ状態を差し替える直前に呼び、
    // 中身の違う場なのに通番が前回と一致して描画の転送が省かれるのを防ぐ
    void InheritRenderSerial(const DisturbanceState& previous) { renderSerial_ = previous.renderSerial_ + 1; }

    // 5 個目以降の場の警告を出した回数 (selftest 用。状態ではない)
    uint32_t OverflowWarnCount() const { return overflowWarnCount_; }

    // スナップショットの DFLD 節の中身 (magic は呼び出し側)。Read は検証に失敗したら out を触らず false
    void Write(ByteWriter& w) const;
    static bool Read(ByteReader& r, DisturbanceState& out);

private:
    friend void DisturbanceStep(World& world, DisturbanceState& state);

    std::vector<DisturbanceSlot> slots_; // entity index 昇順
    std::vector<FieldImpulse> queue_;
    // ---- 以下は状態ではない (ハッシュ・スナップショットに入れない) ----
    uint64_t renderSerial_ = 0;
    uint32_t overflowWarnCount_ = 0;
    bool queueFullWarned_ = false;           // step ごとに戻す
    std::vector<EntityID> warnedFields_;     // 警告済みの場 (5 個目以降)
    std::vector<float> rippleScratch_;       // 波紋の次の h の作業領域
    std::vector<float> shiftScratch_;        // 追従のずらしの書き込み先 (配列 1 本ぶん)
    SolverScratch solver_; // 流れのソルバの作業領域
};

// 1 tick の step (フェーズ 4)。場の掃除と作り直し → 源の注入 → 配送待ちの注入 → 流れの移流・減衰・(Volume は浮力)・投影 → 濃さの移流 → 嵩の移流と重力 → 復元 → 波紋 → 範囲の強制。
// 移流・投影・復元・波紋は z 行 (Volume は y × z 行) で jobs::ParallelRanges に割る (注入は直列)。
// 場が 1 つも無く配送待ちも空なら走査だけで抜ける。World から読むのは Transform 確定後の WorldMatrix、
// CharacterController.velocity、Rigidbody.velocity と、場・源のコンポーネント、PhysicsEnvironment の風
void DisturbanceStep(World& world, DisturbanceState& state);

// デバッグ描画 (出力レーン)。drawDebug の場について、4 マスおきに流れの矢印と濃さの十字を out へ積む。
// 読むだけで状態は変えない。再シム中は呼ばない (呼び出し側が !ts.resim で守る)
void DisturbanceAppendDebugLines(World& world, const DisturbanceState& state, std::vector<DebugLineCmd>& out);

// 配送待ちにインパルスを積む (満杯で false + WARN)。積んだ後の最初の step で消費される
bool DisturbanceQueueImpulse(Scene& scene, const FieldImpulse& impulse);

// 読み取りの結果。無効なチャネルの値は 0
struct FieldSampleOut {
    float flow[3] = { 0.0f, 0.0f, 0.0f }; // GroundLayer は y = 0。Volume は 3 成分
    float density = 0.0f;
    float trail = 0.0f;
    float ripple = 0.0f;
    EntityID field = kNullEntity;
    bool vertical = false; // flow[1] が実測値か (Volume のみ。GroundLayer は y の流れを持たない)
};

// pos での場の値をセル中心の双線形で返す。field が null なら pos を含む場のうち entity index 最小。
// 返り値は「場の中か」 (外なら out は 0 埋め)。格子の外のマスは静止状態として補間する。
// 読むのは呼んだ時点の状態 (Update からは前 tick の結果、LateUpdate からはこの tick の結果)。
// 静止時の濃さと層の厚さはコンポーネントから引くので world が要る
bool DisturbanceSampleAt(const DisturbanceState& state, World& world, EntityID field,
                         const DirectX::XMFLOAT3& pos, FieldSampleOut& out);

} // namespace mye
