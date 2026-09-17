//====================================================================================
//                          ModalAudio.cpp
//  MyEngine/ 秋田蓮音                                                      09/16/2026
//                                          接触 → PendingModalImpact 抽出と cell 選択 → 合成の橋渡し
//====================================================================================
#include "Engine/Engine/Audio/ModalAudio.h"

#include <algorithm>
#include <cmath>

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Acoustic/AcousticField.h" // acoustic::RestingImpulse / kImpactMinImpulse
#include "Engine/Engine/Audio/ModalSynth.h"
#include "Engine/Engine/Audio/VoicePolicy.h" // LinearToDb
#include "Engine/Engine/Physics/PhysMatLibrary.h"
#include "Engine/Engine/Physics/PhysicsSystem.h" // SolidContact

using namespace DirectX;

namespace mye {

const char* ModalShotResultName(ModalShotResult r)
{
    switch (r) {
    case ModalShotResult::Played:
        return "Played";
    case ModalShotResult::NotReady:
        return "NotReady";
    case ModalShotResult::NoModel:
        return "NoModel";
    case ModalShotResult::Cooldown:
        return "Cooldown";
    case ModalShotResult::BelowMin:
        return "BelowMin";
    case ModalShotResult::PoolFull:
        return "PoolFull";
    case ModalShotResult::PlayFailed:
        return "PlayFailed";
    }
    return "Unknown";
}

float ModalImpulseCurve(float excessImpulse)
{
    if (excessImpulse <= 0.0f) {
        return 0.0f;
    }
    return acoustic::kImpactRefImpulse
        * std::pow(excessImpulse / acoustic::kImpactRefImpulse, kModalImpulseExponent);
}

AssetID ResolveModalMesh(World& world, EntityID e, const ModalSoundComponent& comp)
{
    if (!comp.mesh.IsNull()) {
        return comp.mesh;
    }
    if (const auto* mr = world.GetComponent<MeshRendererComponent>(e)) {
        return mr->mesh;
    }
    return AssetID{};
}

float ModalWorldScaleOfLongestAxis(const modal::VoxelFrame& frame, const XMFLOAT4X4& worldMatrix)
{
    const float ex = frame.aabbMax[0] - frame.aabbMin[0];
    const float ey = frame.aabbMax[1] - frame.aabbMin[1];
    const float ez = frame.aabbMax[2] - frame.aabbMin[2];
    int axis = 0;
    float best = ex;
    if (ey > best) {
        best = ey;
        axis = 1;
    }
    if (ez > best) {
        axis = 2;
    }
    // スケール近似: ワールド行列の各行ベクトル長 (Physics/Shapes.cpp::MakePoseFromMatrix と同じ式)
    const XMFLOAT4X4& m = worldMatrix;
    switch (axis) {
    case 0:
        return std::sqrt(m._11 * m._11 + m._12 * m._12 + m._13 * m._13);
    case 1:
        return std::sqrt(m._21 * m._21 + m._22 * m._22 + m._23 * m._23);
    default:
        return std::sqrt(m._31 * m._31 + m._32 * m._32 + m._33 * m._33);
    }
}

void CollectModalImpacts(World& world, const std::vector<SolidContact>& contacts, float dt,
                         uint64_t tick, std::vector<PendingModalImpact>& out)
{
    out.clear();
    if (contacts.empty()) {
        return;
    }

    // ModalSoundComponent を 1 つも持たないシーンでは、索引表の構築・sort を丸ごと省く
    // (reviewer round 1 指摘 6。opt-in 機能の費用を opt-out できるようにする —
    // AcousticField::DrainImpacts が同 tick に同型の索引表を作るので、ここを削っても
    // 衝突そのもののコストは変わらない)。ForEachArchetype はクエリキャッシュ経由なので
    // この判定自体は軽い
    {
        bool anyModalSound = false;
        const ComponentTypeId modalReq[] = { ModalSoundComponent::sTypeId };
        world.ForEachArchetype(modalReq, [&](Archetype&) { anyModalSound = true; });
        if (!anyModalSound) {
            return;
        }
    }

    // index → EntityID の表。AcousticField::DrainImpacts と全く同じ作り方
    // (接触は必ずコライダ同士なので、ColliderComponent を持つ全エンティティを 1 回だけ列挙してソートする)
    std::vector<std::pair<uint32_t, EntityID>> byIndex;
    const ComponentTypeId creq[] = { ColliderComponent::sTypeId };
    world.ForEachArchetype(creq, [&](Archetype& arch) {
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            byIndex.emplace_back(e.index, e);
        }
    });
    std::sort(byIndex.begin(), byIndex.end(),
              [](const std::pair<uint32_t, EntityID>& a, const std::pair<uint32_t, EntityID>& b) {
                  return a.first < b.first;
              });
    const auto lookup = [&byIndex](uint32_t index) {
        const auto it = std::lower_bound(
            byIndex.begin(), byIndex.end(), index,
            [](const std::pair<uint32_t, EntityID>& p, uint32_t v) { return p.first < v; });
        return (it != byIndex.end() && it->first == index) ? it->second : kNullEntity;
    };

    // 「載っているだけ」の力積を測るための重力 (AcousticField::DrainImpacts と同じ取り方)
    const PhysicsEnvironmentComponent* env = ResolvePhysicsEnvironment(world);
    const float gx = (env != nullptr) ? env->gravity.x : 0.0f;
    const float gy = (env != nullptr) ? env->gravity.y : -9.81f;
    const float gz = (env != nullptr) ? env->gravity.z : 0.0f;
    const float gMag = std::sqrt(gx * gx + gy * gy + gz * gz);

    // 1 side ぶんの処理 (ea/eb のどちらが発音元 E か、法線の向きを決めて呼ぶ)
    const auto emitSide = [&](EntityID src, float nx, float ny, float nz, EntityID ea, EntityID eb,
                              const SolidContact& c) {
        const auto* comp = world.GetComponent<ModalSoundComponent>(src);
        if (comp == nullptr) {
            return; // ModalSound を持たない側は従来経路のまま (opt-in)
        }
        const AssetID mesh = ResolveModalMesh(world, src, *comp);
        if (mesh.IsNull()) {
            return; // 焼く対象のメッシュが無い
        }
        const float rest = acoustic::RestingImpulse(world, ea, eb, gMag, dt);
        const float excess = c.impulse - rest;
        if (excess <= acoustic::kImpactMinImpulse) {
            return; // 擦り扱い (spec §4.1)
        }
        const auto* wm = world.GetComponent<WorldMatrixComponent>(src);
        if (wm == nullptr) {
            return; // ワールド行列が無いと局所化できない (通常起きない)
        }
        const XMMATRIX world_ = XMLoadFloat4x4(&wm->value);
        const XMMATRIX inv = XMMatrixInverse(nullptr, world_);
        // 法線をローカル方向へ (回転 + スケールの逆変換の後、単位長へ戻す。
        // nE は元々単位法線なので、|k| = excess を保つには再正規化が要る)
        XMVECTOR nLocal = XMVector3TransformNormal(XMVectorSet(nx, ny, nz, 0.0f), inv);
        const float nLenSq = XMVectorGetX(XMVector3LengthSq(nLocal));
        nLocal = (nLenSq > 1e-12f) ? XMVector3Normalize(nLocal) : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        XMFLOAT3 nLocalF;
        XMStoreFloat3(&nLocalF, nLocal);

        const XMVECTOR pWorld = XMVectorSet(c.px, c.py, c.pz, 1.0f);
        const XMVECTOR pLocal = XMVector3TransformCoord(pWorld, inv);
        XMFLOAT3 pLocalF;
        XMStoreFloat3(&pLocalF, pLocal);

        PendingModalImpact impact;
        impact.source = src;
        impact.mesh = mesh;
        impact.worldPoint[0] = c.px;
        impact.worldPoint[1] = c.py;
        impact.worldPoint[2] = c.pz;
        impact.localPoint[0] = pLocalF.x;
        impact.localPoint[1] = pLocalF.y;
        impact.localPoint[2] = pLocalF.z;
        // spec sub-10 A: k は生の力積ではなく圧縮カーブ C(J) を通した値 (BuildModes は
        // k をそのまま振幅へ使うので、ここで圧縮しておかないと較正 (ampScale) の前提が崩れる)。
        // excessImpulse は生の J のまま (ログ/UI/dBFS×J 表は圧縮前の値で読む)
        const float compressed = ModalImpulseCurve(excess);
        impact.k[0] = compressed * nLocalF.x;
        impact.k[1] = compressed * nLocalF.y;
        impact.k[2] = compressed * nLocalF.z;
        impact.excessImpulse = excess;
        impact.tick = tick;
        impact.key = c.key;
        out.push_back(impact);
    };

    for (const SolidContact& c : contacts) { // ★key 昇順 (PhysicsSystem の出力規約)
        const EntityID ea = lookup(static_cast<uint32_t>(c.key >> 32));
        const EntityID eb = lookup(static_cast<uint32_t>(c.key & 0xFFFFFFFFu));
        if (ea.IsNull() || eb.IsNull()) {
            continue; // 1 tick 古い接触なので、消えたエンティティが混じりうる
        }
        // normal は大 index → 小 index (= eb → ea)。ea (小) が発音元なら nE = n、
        // eb (大) が発音元なら nE = -n (spec §4.1「法線」)
        emitSide(ea, c.nx, c.ny, c.nz, ea, eb, c);
        emitSide(eb, -c.nx, -c.ny, -c.nz, ea, eb, c);
    }
}

ModalShotResult MakeModalShotPlay(const ModalFeatureMap& featureMap, const DmNetHeader& hdr,
                                  const PendingModalImpact& impact, const ModalSoundComponent& comp,
                                  const PhysMat* mat, float worldScaleOfLongestAxis, AudioClip& outClip,
                                  AudioSpatial& outSpatial, ModalShotInfo* info)
{
    const uint16_t rawCell = modal::LocalPointToCell(featureMap.frame, impact.localPoint);
    ModalCellFeature feature;
    if (!featureMap.CellFeature(static_cast<int>(rawCell), feature)) {
        return ModalShotResult::NoModel; // validCount==0 (Ready のはずが特徴が無い異常系)
    }

    ModalPostParams post;
    post.young = (mat != nullptr) ? mat->youngsModulus : 0.0f; // 0 = BuildModes 内で σ1=1 特別扱い
    // 参照材質 (mat==null) は σ2=1 / c=cRef になるよう、hdr の参照値をそのまま渡す
    // (ModalPostParams 自身の既定値 1000/0/0 は単体テスト用のプレースホルダで、ここでは使わない)
    post.density = (mat != nullptr) ? mat->density : hdr.refDensity;
    const float sizeScale = (comp.sizeScale > 0.0f) ? comp.sizeScale : 1.0f;
    post.sizeL = featureMap.frame.longestEdge * worldScaleOfLongestAxis * sizeScale;
    post.alpha = (mat != nullptr) ? mat->rayleighAlpha : hdr.refAlpha;
    post.beta = (mat != nullptr) ? mat->rayleighBeta : hdr.refBeta;
    post.maskThreshold = comp.maskThreshold;
    post.gain = comp.gain;

    ModalModeSet modes;
    BuildModes(feature, impact.k, hdr, post, modes);
    if (modes.count == 0) {
        return ModalShotResult::BelowMin;
    }

    ModalSynthRender(modes, outClip);

    outSpatial = AudioSpatial{};
    outSpatial.position = AudioVec3{ impact.worldPoint[0], impact.worldPoint[1], impact.worldPoint[2] };
    outSpatial.spatialBlend = 1.0f;
    outSpatial.minDistance = 1.0f;
    outSpatial.maxDistance = comp.maxDistance;
    outSpatial.rolloff = 0;
    outSpatial.dopplerScale = 0.0f;
    outSpatial.reverbSend = 0.0f; // 呼び出し側 (acOn のとき) が AcousticAudio.waveReverbSend で上書き
    outSpatial.pitch = 1.0f;

    if (info != nullptr) {
        const uint16_t resolved = featureMap.cellSlot[rawCell];
        info->cellX = resolved % kModalMapN;
        info->cellY = (resolved / kModalMapN) % kModalMapN;
        info->cellZ = resolved / (kModalMapN * kModalMapN);
        info->modeCount = modes.count;
        info->f0Hz = modes.freqHz[0];
        info->f1Hz = modes.freqHz[modes.count - 1];
        info->lenSec = ModalClipSeconds(modes);
        int16_t peak = 0;
        for (const int16_t s : outClip.samples) {
            const int16_t mag = static_cast<int16_t>(s < 0 ? -s : s);
            if (mag > peak) {
                peak = mag;
            }
        }
        info->peakDb = LinearToDb(static_cast<float>(peak) / 32768.0f);
    }

    return ModalShotResult::Played;
}

} // namespace mye
