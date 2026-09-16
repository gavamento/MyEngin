//====================================================================================
//                          ModalAudioSelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      09/16/2026
//                                          衝突 → モーダル一発再生の橋渡しのヘッドレス検査
//====================================================================================
#include "Engine/Engine/Audio/ModalAudioSelfTest.h"

#include <cstring>
#include <vector>

#include <DirectXPackedVector.h>

#include "Engine/Core/Components.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Acoustic/AcousticGrid.h"
#include "Engine/Engine/Audio/AcousticAudio.h"
#include "Engine/Engine/Audio/AudioSourceSystem.h"
#include "Engine/Engine/Audio/ModalAudio.h"
#include "Engine/Engine/Audio/SoundAsset.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Modal/ModalSoundLibrary.h"
#include "Engine/Engine/Physics/PhysicsSystem.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/TransformSystem.h"

namespace mye {
namespace {

// 静的な床 (Rigidbody 無し = dynamicMass 0)
GameObject MakeFloor(Scene& s, const char* name, float x, float y, float z, float hx, float hy,
                     float hz)
{
    GameObject go = s.CreateGameObject(name);
    go.SetLocalPosition(x, y, z);
    auto* col = go.AddComponent<ColliderComponent>();
    col->shape = collidershape::kBox;
    col->halfExtents = { hx, hy, hz };
    return go;
}

// 動的な箱 (mass を直値で持つ = useDensity 無し。EffectiveMassWorld がそのまま mass を返す)
GameObject MakeBox(Scene& s, const char* name, float x, float y, float z, float mass)
{
    GameObject go = s.CreateGameObject(name);
    go.SetLocalPosition(x, y, z);
    auto* col = go.AddComponent<ColliderComponent>();
    col->shape = collidershape::kBox;
    col->halfExtents = { 0.5f, 0.5f, 0.5f };
    auto* rb = go.AddComponent<RigidbodyComponent>();
    rb->mass = mass;
    return go;
}

} // namespace

bool RunModalAudioSelfTest()
{
    MYE_LOG_INFO("==== ModalAudio (collision -> Deep-Modal shot) self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    constexpr float kDt = 1.0f / 60.0f;
    constexpr float kGMag = 9.81f; // シーンに PhysicsEnvironment を置かないので既定重力と一致させる

    // ---- (1) 接触 1 件 → impact 1 件。excessImpulse = impulse - RestingImpulse。
    //          箱を Y 90 度回転すると、水平方向の法線からできる k がローカル軸へ乗り移る ----
    {
        Scene scene;
        World& world = scene.GetWorld();
        TransformSystem xform;

        GameObject floor = MakeFloor(scene, "Floor", 0.0f, -0.5f, 0.0f, 4.0f, 0.5f, 4.0f);
        GameObject box = MakeBox(scene, "Box", 0.0f, 4.0f, 0.0f, 2.0f);
        box.AddComponent<ModalSoundComponent>()->mesh = AssetID{ 777 };
        xform.Update(world);

        const EntityID floorId = floor.Id();
        const EntityID boxId = box.Id();
        const EntityID ea = (floorId.index < boxId.index) ? floorId : boxId;
        const EntityID eb = (floorId.index < boxId.index) ? boxId : floorId;
        const float rest = acoustic::RestingImpulse(world, ea, eb, kGMag, kDt);

        SolidContact c;
        c.key = (static_cast<uint64_t>(ea.index) << 32) | eb.index;
        c.nx = 1.0f;
        c.ny = 0.0f;
        c.nz = 0.0f; // 水平方向 (回転で軸が動くことを見るため、垂直だと Y 回転で不変になってしまう)
        c.impulse = rest + 5.0f;
        c.px = 0.3f;
        c.py = 3.6f;
        c.pz = -0.2f;
        std::vector<SolidContact> contacts{ c };

        std::vector<PendingModalImpact> impacts;
        CollectModalImpacts(world, contacts, kDt, 42, impacts);
        check(impacts.size() == 1, "(1) a contact with one ModalSound side yields exactly 1 impact");
        if (impacts.size() == 1) {
            check(impacts[0].source == boxId, "(1) the impact's source is the ModalSound side");
            check(std::fabs(impacts[0].excessImpulse - 5.0f) < 1.0e-3f,
                  "(1) excessImpulse == impulse - RestingImpulse");
            // 回転前 (identity): ワールド (1,0,0) はローカルでも X 軸に乗る。
            // ★符号は E が ea (小 index) か eb (大 index) かで反転する (spec §4.1「法線」の
            //   nE = n / -n)。ここではどちらの割当でも通るよう絶対値で見る
            check(std::fabs(std::fabs(impacts[0].k[0]) - impacts[0].excessImpulse) < 1.0e-2f
                      && std::fabs(impacts[0].k[2]) < 1.0e-2f,
                  "(1) unrotated box: k lands on local X (matches world X)");
        }

        // Y 90 度回転 -> ワールド X 方向の法線はローカル Z (符号は問わない) へ乗り移る
        box.SetLocalRotationEuler(0.0f, 90.0f, 0.0f);
        xform.Update(world);
        std::vector<PendingModalImpact> impacts2;
        CollectModalImpacts(world, contacts, kDt, 42, impacts2);
        check(impacts2.size() == 1, "(1) rotating the box doesn't change the impact count");
        if (impacts2.size() == 1) {
            check(std::fabs(impacts2[0].k[0]) < 5.0e-2f
                      && std::fabs(std::fabs(impacts2[0].k[2]) - impacts2[0].excessImpulse) < 5.0e-2f,
                  "(1) after a 90 deg yaw, k follows the box's local axis (X -> Z)");
        }
    }

    // ---- (2) 両側 ModalSound -> 2 impact、key 昇順 ----
    {
        Scene scene;
        World& world = scene.GetWorld();
        TransformSystem xform;

        GameObject a = MakeBox(scene, "A", -1.0f, 2.0f, 0.0f, 1.0f);
        GameObject b = MakeBox(scene, "B", 1.0f, 2.0f, 0.0f, 1.0f);
        a.AddComponent<ModalSoundComponent>()->mesh = AssetID{ 1 };
        b.AddComponent<ModalSoundComponent>()->mesh = AssetID{ 2 };
        xform.Update(world);

        const EntityID ea = (a.Id().index < b.Id().index) ? a.Id() : b.Id();
        const EntityID eb = (a.Id().index < b.Id().index) ? b.Id() : a.Id();
        const float rest = acoustic::RestingImpulse(world, ea, eb, kGMag, kDt);

        SolidContact c;
        c.key = (static_cast<uint64_t>(ea.index) << 32) | eb.index;
        c.ny = 1.0f;
        c.impulse = rest + 5.0f;
        std::vector<SolidContact> contacts{ c };

        std::vector<PendingModalImpact> impacts;
        CollectModalImpacts(world, contacts, kDt, 7, impacts);
        check(impacts.size() == 2, "(2) both sides having ModalSound yields 2 impacts");
        if (impacts.size() == 2) {
            check(impacts[0].source == ea, "(2) the small-index side comes first (key ascending)");
            check(impacts[1].source == eb, "(2) the big-index side comes second");
        }
    }

    // ---- (3) RestingImpulse は抽出前の式とビット一致 ----
    {
        Scene scene;
        World& world = scene.GetWorld();
        GameObject a = MakeBox(scene, "A", 0.0f, 0.0f, 0.0f, 3.5f);
        GameObject b = MakeBox(scene, "B", 0.0f, 0.0f, 0.0f, 1.2f);

        const auto dynamicMassRef = [&world](EntityID e) {
            const auto* rb = world.GetComponent<RigidbodyComponent>(e);
            if (rb == nullptr || rb->isKinematic != 0) {
                return 0.0f;
            }
            return EffectiveMassWorld(world, e, *rb);
        };
        const float expect =
            (dynamicMassRef(a.Id()) + dynamicMassRef(b.Id())) * kGMag * kDt * acoustic::kImpactRestingMargin;
        const float actual = acoustic::RestingImpulse(world, a.Id(), b.Id(), kGMag, kDt);
        check(expect == actual, "(3) RestingImpulse matches the pre-extraction inline formula bit-for-bit");
    }

    // ---- (4) push 65 個目は dropped ----
    {
        AudioSourceSystem sources;
        PendingModalImpact impact;
        for (int i = 0; i < AudioSourceSystem::kMaxPendingModalImpacts; ++i) {
            sources.PushModalImpact(impact);
        }
        check(sources.PendingModalImpactCount()
                  == static_cast<size_t>(AudioSourceSystem::kMaxPendingModalImpacts),
              "(4) the queue fills up to the cap");
        sources.PushModalImpact(impact);
        check(sources.PendingModalImpactCount()
                      == static_cast<size_t>(AudioSourceSystem::kMaxPendingModalImpacts)
                  && sources.ModalStats().dropped == 1,
              "(4) the 65th push is dropped and counted");
    }

    // ---- (5) 合成マップ (全帯域 mask on): k=(1,0,0) -> count 32、k=0 -> BelowMin、
    //          AudioSpatial.maxDistance はコンポーネント値そのまま ----
    {
        DmNetHeader hdr;
        MelBandCenters(hdr.bandCenterHz);
        hdr.logAmpMin = 0.0f;
        hdr.logAmpMax = 0.0f; // amp を常に exp(0)=1 に固定 (手順 2 を定数化してテストを単純にする)
        hdr.maskThreshold = 0.5f;

        ModalFeatureMap fm;
        constexpr int kCell = modal::CellIndexOf(8, 8, 8);
        for (uint16_t& s : fm.cellSlot) {
            s = static_cast<uint16_t>(kCell); // 全 cell が唯一の有効 cell (kCell) を指す
        }
        fm.frame.origin[0] = fm.frame.origin[1] = fm.frame.origin[2] = -16.0f;
        fm.frame.voxelSize = 1.0f;
        fm.frame.longestEdge = hdr.refSizeL; // worldScale=1・sizeScale=1 で σ3=1 になるよう合わせる
        fm.validCount = 1;
        fm.feat.assign(static_cast<size_t>(kModalChannels), 0);
        for (int j = 0; j < 3; ++j) {
            for (int i = 0; i < kModalBands; ++i) {
                fm.feat[static_cast<size_t>(MaskCh(j, i))] =
                    DirectX::PackedVector::XMConvertFloatToHalf(10.0f); // 全帯域・全軸 mask on
                fm.feat[static_cast<size_t>(AmpCh(j, i))] =
                    DirectX::PackedVector::XMConvertFloatToHalf(1.0f);
            }
        }

        ModalSoundComponent comp;
        comp.maxDistance = 17.5f;

        PendingModalImpact impact;
        impact.worldPoint[0] = 1.0f;
        impact.worldPoint[1] = 2.0f;
        impact.worldPoint[2] = 3.0f;
        impact.localPoint[0] = 0.0f; // frame.origin=(-16,-16,-16) の voxel 16 (cell 8) の中心付近
        impact.localPoint[1] = 0.0f;
        impact.localPoint[2] = 0.0f;
        impact.k[0] = 1.0f;
        impact.k[1] = 0.0f;
        impact.k[2] = 0.0f;

        AudioClip clip;
        AudioSpatial spatial;
        ModalShotInfo info;
        const ModalShotResult r1 =
            MakeModalShotPlay(fm, hdr, impact, comp, nullptr, 1.0f, clip, spatial, &info);
        check(r1 == ModalShotResult::Played, "(5) k=(1,0,0) with all-mask-on produces a Played shot");
        check(info.modeCount == kModalBands, "(5) all 32 bands survive when every mask channel is on");
        check(spatial.maxDistance == comp.maxDistance,
              "(5) AudioSpatial.maxDistance mirrors the component value");

        impact.k[0] = 0.0f; // k=0 -> どの帯域も a<=0 で落ちる
        AudioClip clip2;
        AudioSpatial spatial2;
        ModalShotInfo info2;
        const ModalShotResult r2 =
            MakeModalShotPlay(fm, hdr, impact, comp, nullptr, 1.0f, clip2, spatial2, &info2);
        check(r2 == ModalShotResult::BelowMin, "(5) k=(0,0,0) leaves every band at zero amplitude");
    }

    // ---- (6) cooldown 判定の純関数 ----
    {
        check(!ModalOnCooldown(false, 0, 3, 100), "(6) never fired before -> not on cooldown");
        check(ModalOnCooldown(true, 100, 3, 101), "(6) a 2nd shot 1 tick later is within a 3-tick cooldown");
        check(ModalOnCooldown(true, 100, 3, 102), "(6) still within cooldown at +2 ticks");
        check(!ModalOnCooldown(true, 100, 3, 103), "(6) cooldown elapses exactly at +cooldownTicks");
        check(!ModalOnCooldown(true, 100, 0, 100), "(6) cooldownTicks<=0 disables the gate");
    }

    // ---- (7) ResolveWaveShotSound: 焼き上がった ModalSound は波を黙らせる。
    //          未焼きは従来の soundKey へ、muteWave=0 は明示オプトアウト ----
    {
        ModalSoundLibrary lib;
        ModalSoundLibrary* prevLib = modalsound::Library();
        lib.Register(AssetID{ 42 }, ModalFeatureMap{});
        modalsound::Install(&lib);

        Scene scene;
        World& world = scene.GetWorld();

        GameObject ready = scene.CreateGameObject("ReadyMuted");
        ready.AddComponent<ModalSoundComponent>()->mesh = AssetID{ 42 }; // muteWave は既定 1

        GameObject notReady = scene.CreateGameObject("NotReadyFallback");
        notReady.AddComponent<ModalSoundComponent>()->mesh = AssetID{ 999 }; // 未登録 = 未焼き
        auto* ws = notReady.AddComponent<WaveSoundComponent>();
        std::strncpy(ws->sound, "footstep_wood", sizeof(ws->sound) - 1);

        GameObject optOut = scene.CreateGameObject("ReadyOptOut");
        auto* optComp = optOut.AddComponent<ModalSoundComponent>();
        optComp->mesh = AssetID{ 42 };
        optComp->muteWave = 0;

        PendingWaveShot shot;
        ResolveWaveShotSound(world, ready.Id(), 0, shot);
        check(shot.mute == 1, "(7) a baked ModalSound mutes the wave shot");

        PendingWaveShot shot2;
        ResolveWaveShotSound(world, notReady.Id(), 0, shot2);
        check(shot2.mute == 0 && shot2.soundKey == HashStr(std::string_view("footstep_wood")),
              "(7) an unbaked ModalSound falls back to the WaveSound key");

        PendingWaveShot shot3;
        ResolveWaveShotSound(world, optOut.Id(), 0, shot3);
        check(shot3.mute == 0, "(7) muteWave==0 opts out even when the mesh is baked");

        modalsound::Install(prevLib);
    }

    // ---- (8) suspend 中の Update でキューが空になる (T18 と同じ「早期 return より前に空にする」規律) ----
    {
        AudioSourceSystem sources;
        PendingModalImpact impact;
        sources.PushModalImpact(impact);
        sources.PushModalImpact(impact);
        check(sources.PendingModalImpactCount() == 2, "(8) two impacts are queued");

        Scene scene;
        AudioSystem audio; // Init を呼ばない = IsReady() false (デバイス非依存)
        audio.SetSuspended(true);
        SoundLibrary sounds;
        sources.Update(scene.GetWorld(), audio, sounds, 1, 1.0f / 60.0f, true);
        check(sources.PendingModalImpactCount() == 0,
              "(8) Update empties the queue before the IsReady/suspended early-out");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== ModalAudio self test: ALL PASS ====");
    } else {
        MYE_LOG_ERROR("==== ModalAudio self test: %d FAILED ====", failCount);
    }
    return failCount == 0;
}

} // namespace mye
