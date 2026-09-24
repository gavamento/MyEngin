#include "Engine/Engine/WaterWaveSelfTest.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "Engine/Core/Components.h"
#include "Engine/Core/ComponentRegistry.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/WaveMath.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Physics/PhysicsSystem.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/SceneSerializer.h"

using namespace DirectX;

namespace mye {

bool RunWaterWaveSelfTest()
{
    MYE_LOG_INFO("==== WaterWave self test (Gerstner waves) ====");
    RegisterBuiltinComponents();
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // 1. 静水面テスト (波なし / スケール 0)
    {
        GerstnerWave waves[1] = { { 1.0f, 10.0f, 2.0f, 0.0f, 0.5f } };
        XMFLOAT3 pos;
        XMFLOAT3 norm;
        wave::SampleGerstnerWaves(waves, 1, 5.0f, 5.0f, 1.0f, 2.0f, 0.0f, 1.0f, pos, norm);
        check(std::fabs(pos.x - 5.0f) < 1e-6f && std::fabs(pos.z - 5.0f) < 1e-6f,
              "calm water: zero horizontal displacement when scale is 0");
        check(std::fabs(pos.y - 2.0f) < 1e-6f,
              "calm water: vertical position equals baseHeight when scale is 0");
        check(std::fabs(norm.x) < 1e-6f && std::fabs(norm.y - 1.0f) < 1e-6f && std::fabs(norm.z) < 1e-6f,
              "calm water: normal is strictly +Y (0, 1, 0)");
    }

    // 2. 単一正弦波の周期性 (wavelength = 20.0m)
    {
        GerstnerWave waves[1] = { { 0.5f, 20.0f, 3.0f, 0.0f, 0.0f } }; // +X 方向, 正弦波 (Q=0)
        const float t = 0.0f;
        const float baseH = 0.0f;

        const float h0 = wave::EvaluateWaveHeight(waves, 1, 0.0f, 0.0f, t, baseH, 1.0f, 1.0f);
        const float hPeriod = wave::EvaluateWaveHeight(waves, 1, 20.0f, 0.0f, t, baseH, 1.0f, 1.0f);
        const float hTwoPeriod = wave::EvaluateWaveHeight(waves, 1, 40.0f, 0.0f, t, baseH, 1.0f, 1.0f);

        check(std::fabs(h0 - hPeriod) < 1e-5f, "wave periodicity: height at x=0 equals height at x=lambda");
        check(std::fabs(h0 - hTwoPeriod) < 1e-5f, "wave periodicity: height at x=0 equals height at x=2*lambda");

        // 波高の範囲 [baseH - A, baseH + A]
        bool bounded = true;
        for (int i = 0; i < 20; ++i) {
            const float x = static_cast<float>(i);
            const float y = wave::EvaluateWaveHeight(waves, 1, x, 0.0f, t, baseH, 1.0f, 1.0f);
            if (y > 0.5f + 1e-5f || y < -0.5f - 1e-5f) {
                bounded = false;
                break;
            }
        }
        check(bounded, "wave amplitude bound: height is strictly bounded within [-A, +A]");
    }

    // 3. Gerstner波のトロコイド特性 (波頭の尖りと急峻度 Q)
    {
        GerstnerWave waves[1] = { { 1.0f, 20.0f, 2.0f, 0.0f, 0.8f } }; // Q = 0.8
        XMFLOAT3 crestPos;
        XMFLOAT3 crestNorm;
        // x = 0, t = 0 で cos(phi) = 1 (波頭の頂点)
        wave::SampleGerstnerWaves(waves, 1, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, crestPos, crestNorm);
        check(std::fabs(crestPos.y - 1.0f) < 1e-5f, "gerstner crest: max elevation matches amplitude");

        // 波頭直前 (x = -2m): sin(phi) < 0 なので dx = -Q*A*sin(phi) > 0 となり、+X (波頭側) へ引き寄せられる
        XMFLOAT3 flankPos;
        XMFLOAT3 flankNorm;
        wave::SampleGerstnerWaves(waves, 1, -2.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, flankPos, flankNorm);
        check(flankPos.x > -2.0f, "gerstner trochoid: water particles displace towards the crest");

        // 法線が単位ベクトルであること
        const float normLen = std::sqrt(flankNorm.x * flankNorm.x + flankNorm.y * flankNorm.y + flankNorm.z * flankNorm.z);
        check(std::fabs(normLen - 1.0f) < 1e-5f, "analytical normal: unit length (|N| == 1)");
    }

    // 4. 複数波の重ね合わせと決定論性 (Bit-level Determinism)
    {
        WaterWaveComponent comp;
        comp.waveCount = 4;
        comp.overallScale = 1.0f;
        comp.timeScale = 1.0f;

        GerstnerWave waveArray[4];
        comp.ExtractWaves(waveArray, 4);

        const float x = 12.345f;
        const float z = -67.890f;
        const float t = 3.14159f;

        XMFLOAT3 p1, n1;
        XMFLOAT3 p2, n2;
        wave::SampleGerstnerWaves(waveArray, 4, x, z, t, comp.baseHeight, comp.overallScale, comp.timeScale, p1, n1);
        wave::SampleGerstnerWaves(waveArray, 4, x, z, t, comp.baseHeight, comp.overallScale, comp.timeScale, p2, n2);

        check(p1.x == p2.x && p1.y == p2.y && p1.z == p2.z,
              "determinism: repeated evaluation produces bit-identical position");
        check(n1.x == n2.x && n1.y == n2.y && n1.z == n2.z,
              "determinism: repeated evaluation produces bit-identical normal");
    }

    // 5. 逆写像反復 (固定点反復) による厳密な水面高さ評価
    {
        GerstnerWave waves[2] = {
            { 0.5f, 15.0f, 2.0f,  0.0f, 0.5f },
            { 0.3f,  8.0f, 3.0f, 45.0f, 0.4f }
        };
        const float targetX = 5.0f;
        const float targetZ = 3.0f;
        const float t = 1.5f;

        const float fastH = wave::EvaluateWaveHeight(waves, 2, targetX, targetZ, t, 0.0f, 1.0f, 1.0f);
        const float iterH = wave::EvaluateWaveHeightIterative(waves, 2, targetX, targetZ, t, 0.0f, 1.0f, 1.0f, 3);

        // 適切な範囲に収まり、高速版との差が妥当であること
        check(std::isfinite(iterH), "iterative wave height: converges to a finite value");
        check(std::fabs(iterH - fastH) < 1.0f, "iterative wave height: within physical variance of fast height");
    }

    // 6. ECS 登録と ResolveActiveWaterWave の動作
    {
        check(WaterWaveComponent::sTypeId != kInvalidComponentType,
              "ecs registration: WaterWaveComponent has a valid TypeId");

        World world;
        check(ResolveActiveWaterWave(world) == nullptr,
              "resolve water wave: returns null when no WaterWave entity exists");

        EntityID e1 = world.CreateEntity("WaterSurface1");
        auto* w1 = world.AddComponent<WaterWaveComponent>(e1);
        w1->enabled = 1;
        w1->baseHeight = 1.5f;

        EntityID e2 = world.CreateEntity("WaterSurface2");
        auto* w2 = world.AddComponent<WaterWaveComponent>(e2);
        w2->enabled = 1;
        w2->baseHeight = 3.0f;

        const WaterWaveComponent* best = ResolveActiveWaterWave(world);
        check(best != nullptr && best->baseHeight == 1.5f,
              "resolve water wave: selects entity with smallest index");

        world.GetComponent<WaterWaveComponent>(e1)->enabled = 0;
        best = ResolveActiveWaterWave(world);
        check(best != nullptr && best->baseHeight == 3.0f,
              "resolve water wave: skips disabled component and selects next active one");
    }

    // 7. surfaceMaterial (M79 sub-05): 描画専用の差し替え口。
    //    保存/復元は従来どおり行うが、WorldHash / リプレイには一切畳み込まれないこと
    {
        const ComponentDesc& desc = ComponentRegistry::Get().Desc(WaterWaveComponent::sTypeId);
        const FieldDesc* smField = nullptr;
        for (const FieldDesc& f : desc.fields) {
            if (std::strcmp(f.name, "surfaceMaterial") == 0) {
                smField = &f;
                break;
            }
        }
        check(smField != nullptr, "surfaceMaterial: field is registered");
        if (smField != nullptr) {
            check((smField->flags & kFieldNoHash) != 0, "surfaceMaterial: kFieldNoHash flag is set");
            check((smField->flags & kFieldNoSerialize) == 0,
                  "surfaceMaterial: kFieldNoSerialize is NOT set (still saved to scene JSON)");
        }

        Scene sceneA;
        GameObject waterGo = sceneA.CreateGameObject("Water");
        auto* waveA = waterGo.AddComponent<WaterWaveComponent>();
        waveA->enabled = true;
        waveA->baseHeight = 0.5f;
        waveA->wave0Amplitude = 0.33f;
        sceneA.GetWorld().ApplyStructuralChanges();

        const uint64_t hashWithout = HashWorld(sceneA.GetWorld());
        waveA->surfaceMaterial = AssetID{ 0x0123456789ABCDEFull };
        const uint64_t hashWith = HashWorld(sceneA.GetWorld());
        check(hashWithout == hashWith,
              "surfaceMaterial: WorldHash is identical whether the field is set or not");

        const nlohmann::json saved = SceneSerializer::SaveToJson(sceneA);
        Scene sceneB;
        check(SceneSerializer::LoadFromJson(sceneB, saved), "surfaceMaterial: scene JSON reload succeeds");
        GameObject waterGoB = sceneB.Find("Water");
        auto* waveB = static_cast<bool>(waterGoB) ? waterGoB.GetComponent<WaterWaveComponent>() : nullptr;
        check(waveB != nullptr && waveB->surfaceMaterial == waveA->surfaceMaterial,
              "surfaceMaterial: round-trips through scene JSON save/load");
    }

    MYE_LOG_INFO("==== WaterWave self test: %s (fail count: %d) ====",
                 (failCount == 0 ? "ALL PASS" : "FAILED"), failCount);
    return failCount == 0;
}

} // namespace mye
