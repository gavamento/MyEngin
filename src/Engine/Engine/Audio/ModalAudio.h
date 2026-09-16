//====================================================================================
//                          ModalAudio.h
//  MyEngine/ 秋田蓮音                                                      09/16/2026
//                                          衝突接触 → Deep-Modal 一発再生への橋渡し (純関数 + POD)
//====================================================================================
#pragma once
#include <cstdint>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/EntityID.h"
#include "Engine/Engine/Audio/AudioClip.h"
#include "Engine/Engine/Audio/AudioSystem.h" // AudioSpatial
#include "Engine/Engine/Modal/ModalFeatureMap.h"
#include "Engine/Engine/Modal/ModalTypes.h"
#include "Engine/Engine/Modal/Voxelizer.h" // modal::VoxelFrame

namespace mye {

class World;
struct SolidContact;    // Engine/Engine/Physics/PhysicsSystem.h
struct ModalSoundComponent; // Engine/Core/Components.h
struct PhysMat;          // Engine/Engine/Physics/PhysMatLibrary.h

// TickRunner の !ts.resim ブロックが今 tick の接触から積む 1 件 (spec §4.1「経路 (ランタイム)」)。
// ★接触点は**ワールド**と**発音元ローカル**の両方を持つ。ワールドは AudioSpatial.position に、
//   ローカルは cell 選択 (modal::LocalPointToCell) に使う。ワールド行列の逆変換は
//   CollectModalImpacts (今 tick の WorldMatrix が手に入る場所) で 1 回だけ行う。
struct PendingModalImpact {
    EntityID source = kNullEntity; // ModalSound を持つ側 (発音元)
    AssetID mesh = {};             // 解決済み (ModalSound.mesh か同 entity の MeshRenderer.mesh)
    float worldPoint[3] = {};      // 接触点 (ワールド)
    float localPoint[3] = {};      // 接触点 (発音元ローカル。cell 選択に使う)
    float k[3] = {};               // J_excess * nE_local (ローカル軸の力ベクトル、符号付き)
    float excessImpulse = 0.0f;    // J_excess = impulse - RestingImpulse
    uint64_t tick = 0;
    uint64_t key = 0;              // SolidContact.key (デバッグ / 順序確認用)
};

// CollectModalImpacts / drain の累積統計。EngineLoop 終了時の summary と [modal] t= ログの
// 両方が参照する (AcousticAudioStats と同じ役割)。**フィールドの追加は末尾のみ**
// ([modal] summary の printf 表と 1 対 1)
struct ModalAudioStats {
    int32_t impacts = 0;    // キューから取り出した (= 検査した) 件数
    int32_t played = 0;
    int32_t notReady = 0;   // メッシュ未焼き (Missing/Baking/Failed) + NoModel をまとめて数える
    int32_t cooldown = 0;
    int32_t belowMin = 0;
    int32_t dropped = 0;    // PushModalImpact のキュー溢れ (kMaxPendingModalImpacts 超過)
    int32_t poolFull = 0;
    int32_t playFailed = 0;
    int32_t bakes = 0;      // --modal-sync-bake が実際に BakeSync を呼んだ回数
    float bakeMsTotal = 0.0f;

    float BakeMsAvg() const
    {
        return bakes > 0 ? bakeMsTotal / static_cast<float>(bakes) : 0.0f;
    }
};

// MakeModalShotPlay / drain の結果。**Played 以外は 1 音も鳴らない**
// (WaveShotResult と同じ設計。summary とログ行の r= がこの名前を出す)
enum class ModalShotResult : int32_t {
    Played = 0,
    NotReady = 1,   // メッシュのモーダル特徴が未焼き。wave がそのまま鳴る (段階移行)
    NoModel = 2,    // .dmnet 未ロード
    Cooldown = 3,   // ModalSoundComponent.cooldownTicks 以内の再発音
    BelowMin = 4,   // 全帯域が mask/振幅で落ちて 1 モードも残らない (k=0 を含む)
    PoolFull = 5,   // 32 スロットの回転プールが (ラウンドロビンで選んだ 1 本が) 再生中
    PlayFailed = 6, // AudioSystem::Play が無効ハンドルを返した (voice 枯渇 / suspend)
};

const char* ModalShotResultName(ModalShotResult r);

// cooldown 判定の **純関数**。AudioSourceSystem::Update の drain がこれを呼ぶ。
// everShot==false (この発音元でまだ 1 度も鳴っていない) か cooldownTicks<=0 (無効化) なら
// 常に false (= cooldown ではない)。それ以外は「前回の発音 tick から cooldownTicks 未満しか
// 経っていないか」を返す。デバイス非依存で直接テストできるよう Update から切り出してある
inline bool ModalOnCooldown(bool everShot, uint64_t lastShotTick, int32_t cooldownTicks, uint64_t tick)
{
    if (!everShot || cooldownTicks <= 0) {
        return false;
    }
    return tick - lastShotTick < static_cast<uint64_t>(cooldownTicks);
}

// 衝突音 1 発の付帯情報 ([modal] ログ専用)。AcousticShapeInfo (遮蔽・回折) とは別物 —
// こちらはモーダル合成側 (選ばれた cell / モード数 / 周波数域 / PCM ピーク / 長さ) だけを持つ
struct ModalShotInfo {
    int32_t cellX = 0, cellY = 0, cellZ = 0; // 実際に特徴を読んだ cell (無効なら最寄りへ丸め済み)
    int32_t modeCount = 0;
    float f0Hz = 0.0f;  // 生き残ったモードのうち最低帯域のもの
    float f1Hz = 0.0f;  // 同、最高帯域
    float peakDb = 0.0f; // 合成 PCM のピーク [dBFS] (VoicePolicy::LinearToDb)
    float lenSec = 0.0f; // ModalClipSeconds と同値
};

// ModalSound.mesh (空なら同 entity の MeshRenderer.mesh) を解決する **唯一の規則**。
// CollectModalImpacts / ResolveWaveShotSound (口封じ) / Inspector プレビュー (sub-07) が共有する
// (MakeSourcePlay の「規則は 1 本」と同じ思想 — 2 本目を書くと必ずずれる)
AssetID ResolveModalMesh(World& world, EntityID e, const ModalSoundComponent& comp);

// 発音元のローカル軸のうち、メッシュのローカル AABB で最長の軸に対応するワールドスケールを返す
// (spec §4.1「σ3 の L_obj = ローカル最長辺 × その軸のワールドスケール × sizeScale」)。
// WorldMatrix の行ベクトル長でスケールを近似する式は Physics/Shapes.cpp::MakePoseFromMatrix と同じ
float ModalWorldScaleOfLongestAxis(const modal::VoxelFrame& frame,
                                   const DirectX::XMFLOAT4X4& worldMatrix);

// SolidContact (今 tick) から ModalSound を持つ側ごとに PendingModalImpact を作る **純関数**。
// index→EntityID の表は AcousticField::DrainImpacts と全く同じ作り方 (二分探索用に
// ColliderComponent を持つ全エンティティを 1 回だけ列挙してソートする)。
// contacts は key 昇順 (PhysicsSystem の出力規約) — out もその順で埋まる。
// 両側が ModalSound を持てば両方積む (小 index 側→大 index 側の順)。
// **RestingImpulse の評価順は 1 文字も変えない** (acoustic::RestingImpulse に委譲。
// AcousticField::DrainImpacts も同じ関数を呼ぶ — 規則は 1 本)
void CollectModalImpacts(World& world, const std::vector<SolidContact>& contacts, float dt,
                         uint64_t tick, std::vector<PendingModalImpact>& out);

// 1 件の PendingModalImpact → PCM + AudioSpatial (cell 選択 → BuildModes → ModalSynthRender)。
// **Inspector の面打ちプレビュー (sub-07) もこれを呼ぶ** — 衝突からの経路とプレビューが
// 同じ関数を通ることで「テストが見ている規則と実際に鳴る規則が別物」になるのを防ぐ
// (MakeSourcePlay / MakeWaveShotPlay と同じ設計)。
//
// mat が null なら参照材質 (E は BuildModes 側で σ1=1 に特別扱い。density/alpha/beta は
// hdr.refDensity/refAlpha/refBeta を渡すことで σ2=1・c=cRef になる)。
// outSpatial は position/spatialBlend/minDistance/maxDistance/rolloff/dopplerScale/pitch を
// 埋める。reverbSend は 0 のまま (AcousticAudio の有無をここでは知らないので、
// 呼び出し側が acOn のときだけ上書きする)。
// 返り値に NotReady/NoModel は現れない (呼び出し側が Request()/BakeSync() の結果で先に弾く)
ModalShotResult MakeModalShotPlay(const ModalFeatureMap& featureMap, const DmNetHeader& hdr,
                                  const PendingModalImpact& impact, const ModalSoundComponent& comp,
                                  const PhysMat* mat, float worldScaleOfLongestAxis, AudioClip& outClip,
                                  AudioSpatial& outSpatial, ModalShotInfo* info);

} // namespace mye
