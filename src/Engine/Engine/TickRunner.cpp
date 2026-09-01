#include "Engine/Engine/TickRunner.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include "Engine/Core/AssetGuidResolver.h" // v8 PlayMusic の生クリップ経路 (GUID → 実パス)
#include "Engine/Core/Check.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Profiler.h"
#include "Engine/Core/Random.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Animation.h"
#include "Engine/Engine/AnimatorController.h"
#include "Engine/Engine/Acoustic/AcousticField.h"
#include "Engine/Engine/Audio/AudioMixer.h"
#include "Engine/Engine/Audio/AudioSourceSystem.h"
#include "Engine/Engine/Audio/AudioSystem.h"
#include "Engine/Engine/Audio/SoundAsset.h"
#include "Engine/Engine/CollisionSystem.h"
#include "Engine/Engine/DebugDraw.h"
#include "Engine/Engine/EffectSystem.h"
#include "Engine/Engine/PartFollowSystem.h"
#include "Engine/Engine/PlayerInputSystem.h"
#include "Engine/Engine/Particles/ParticleSystem.h"
#include "Engine/Engine/Physics/PhysicsDebugDraw.h"
#include "Engine/Engine/Physics/PhysicsSystem.h"
#include "Engine/Engine/Physics/XpbdBackend.h" // M60'b: シーン遷移時の Reset 用
#include "Engine/Engine/Prefab.h"
#include "Engine/Engine/RenderSystem.h"
#include "Engine/Engine/Replay/Replay.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/SaveGame.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/SceneSerializer.h"
#include "Engine/Engine/Script/ManagedHost.h"
#include "Engine/Engine/Script/ScriptHost.h"
#include "Engine/Engine/SkinningSystem.h"
#include "Engine/Engine/TransformSystem.h"
#include "Engine/Engine/Vfx/VfxRenderer.h"
#include "Engine/Platform/InputActions.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GpuResources.h"

namespace mye {
namespace {

// スクリプトが積んだオーディオイベント 1 件を実際の発音へ流す (M45g)。
// **必ずワールドハッシュの後に呼ぶこと** — ここから先は決定論レーンの外で、
// 記録/検証中は AudioSystem が suspend されているので個々の API が no-op になる。
// rng は AudioSystem 側の専用ストリーム。**world.Rng() は絶対に使わない** (hash 対象)。
void ApplyScriptAudioEvent(const ScriptAudioEvent& e, World& world, AudioSystem& audio,
                           const SoundLibrary& sounds, AudioSourceSystem& sources, Pcg32& rng)
{
    switch (e.op) {
    case ScriptAudioOp::PlayOneShot:
    case ScriptAudioOp::PlayAtPoint: {
        const bool at = e.op == ScriptAudioOp::PlayAtPoint;
        const ResolvedSound rs = ResolveSoundKey(audio, sounds, e.key);
        if (!rs.Valid()) {
            // ★黙って無音にしない — 綴り間違いは「壊れている」と見分けが付かないため
            MYE_LOG_WARN("[audio] unknown sound key (hash %016llx) from script",
                         static_cast<unsigned long long>(e.key));
            break;
        }
        PlayDesc d;
        AudioSpatial spatial;
        bool spatialize = false;
        if (rs.asset != nullptr) {
            if (rs.asset->stream) {
                // stream = BGM。ワンショットのレーンには載らないので BGM として鳴らす
                // (エディタ試聴 PreviewSound と同じ扱い。返したハンドルは無効のまま)
                SoundAsset a = *rs.asset;
                a.volume = std::clamp(a.volume * e.a, 0.0f, 1.0f);
                PlayMusicSound(audio, a, kMusicDefaultFadeSeconds);
                break;
            }
            const int index = PickVariationIndex(*rs.asset, rng.NextU32());
            if (index < 0) {
                break; // クリップが 1 つも割り当たっていないアセット
            }
            d = MakePlayDesc(*rs.asset, index, rng.Range(-1.0f, 1.0f), rng.Range(-1.0f, 1.0f),
                             audio);
            // スクリプト引数はアセット既定への**乗算**(コンポーネント上書きと同じ規約)
            d.volume = std::clamp(d.volume * e.a, 0.0f, 1.0f);
            d.pitch = std::clamp(d.pitch * e.b, 1.0f / AudioSystem::kMaxFreqRatio,
                                 AudioSystem::kMaxFreqRatio);
            spatial.spatialBlend = rs.asset->spatialBlend;
            spatial.minDistance = rs.asset->minDistance;
            spatial.maxDistance = rs.asset->maxDistance;
            spatial.rolloff = static_cast<int>(rs.asset->rolloff);
            spatial.dopplerScale = rs.asset->dopplerScale;
            spatial.reverbSend = rs.asset->reverbSend;
        } else {
            // 生クリップ (M19 からの PlaySound("beep") 経路)。アセットが無いので既定値
            d.clip = rs.clip;
            d.bus = audio.DefaultBus();
            d.volume = std::clamp(e.a, 0.0f, 1.0f);
            d.pitch = e.b;
            d.loop = e.i0 != 0;
        }
        if (at) {
            // ★PlaySoundAt は「その座標で鳴らせ」という明示指定なので、2D 設定の音でも
            //   3D に載せる。ここで落とすと座標を渡したのに定位しない = 一番分かりにくい
            spatial.position = AudioVec3{ e.pos.x, e.pos.y, e.pos.z };
            spatial.velocity = {}; // 置き音なので静止 (ドップラーは掛からない)
            if (spatial.spatialBlend <= 0.0f) {
                spatial.spatialBlend = 1.0f;
            }
            spatial.pitch = d.pitch;
            spatialize = true;
        }
        d.tag = e.handle; // 後の tick から StopVoice / SetVoiceVolume で引けるようにする
        d.spatial = spatialize ? &spatial : nullptr;
        audio.Play(d);
        break;
    }
    case ScriptAudioOp::StopVoice:
        audio.Stop(audio.FindByTag(e.handle), e.a);
        break;
    case ScriptAudioOp::SetVoiceVolume:
        audio.SetVoiceVolume(audio.FindByTag(e.handle), e.a);
        break;
    case ScriptAudioOp::SetVoicePitch:
        audio.SetVoicePitch(audio.FindByTag(e.handle), e.b);
        break;
    case ScriptAudioOp::PlaySource:
        sources.PlayEntity(world, audio, sounds, { e.entity.index, e.entity.generation });
        break;
    case ScriptAudioOp::StopSource:
        sources.StopEntity(world, audio, sounds, { e.entity.index, e.entity.generation }, e.a);
        break;
    case ScriptAudioOp::SetBusVolume: {
        const int bus = audio.FindBusHashed(e.key);
        if (bus >= 0) {
            audio.SetBusVolume(bus, e.a); // 未知のバス名は黙って無視 (既存の音量を壊さない)
        }
        break;
    }
    case ScriptAudioOp::PlayMusic: {
        const ResolvedSound rs = ResolveSoundKey(audio, sounds, e.key);
        if (rs.asset != nullptr) {
            SoundAsset a = *rs.asset;
            a.loop = e.i0 != 0;
            PlayMusicSound(audio, a, e.a);
        } else if (!rs.clip.IsNull()) {
            // .sound.json を作らずに素の .wav/.ogg を BGM 指定した場合。
            // GUID から実ファイルを引いて既定パラメータでストリーミングする
            MusicDesc d;
            d.path = assetguid::ResolvePath(rs.clip.value);
            d.key = rs.clip.value;
            const int bus = audio.FindBus("BGM");
            d.bus = bus >= 0 ? bus : audio.DefaultBus();
            d.fadeSeconds = e.a;
            d.loop = e.i0 != 0;
            if (!d.path.empty()) {
                audio.PlayMusic(d);
            }
        }
        break;
    }
    case ScriptAudioOp::StopMusic:
        audio.StopMusic(e.a);
        break;
    case ScriptAudioOp::SetListener:
        sources.SetListenerOverride({ e.entity.index, e.entity.generation });
        break;
    }
}

// M36b 描画補間: tick 頭のワールド行列を採取する。record/verify 中は補間しないので
// 呼び出し側 (TickServices::prevWorld) が null を渡して丸ごと省く
void CapturePrevWorld(PrevWorldStore& prevWorld, World& w)
{
    const ComponentTypeId req[] = { WorldMatrixComponent::sTypeId };
    w.ForEachArchetype(req, [&](Archetype& arch) {
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (e.index >= prevWorld.world.size()) {
                prevWorld.world.resize(e.index + 1);
                prevWorld.generation.resize(e.index + 1, 0);
            }
            prevWorld.world[e.index] =
                static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
            prevWorld.generation[e.index] = e.generation + 1;
        }
    });
}

} // namespace

// 固定 tick 1 回分 (engine_spec.md 5.3 のフェーズ 3/3.5/3.6/4/5/7 + tick 末の出力レーン)。
// EngineLoop / タイムトラベル / ロールバックの 3 経路が**この 1 実装だけ**を通る (決定台帳 2)
void RunOneTick(TickServices& ts)
{
    // 本体は元のフレームループから丸ごと持ってきたコードなので、参照名は当時のまま束ねる
    // (差分を「移動」に留めて、抽出そのものが挙動を変えていないことを読めるようにする)
    EngineContext& ctx = *ts.ctx;
    const EngineConfig& config = *ts.config;
    Scene& scene = *ts.scene;
    InputActions& inputActions = *ts.inputActions;
    InputSnapshot* prevTickInput = ts.prevTickInput; // kMaxPlayers 本のレーン配列 (M52g)
    ScriptHost& scriptHost = *ts.scriptHost;
    ManagedHost& managedHost = *ts.managedHost;
    AnimationSystem& animationSystem = *ts.animationSystem;
    AnimationLibrary& animLibrary = *ts.animLibrary;
    AnimatorControllerSystem& controllerSystem = *ts.controllerSystem;
    ControllerLibrary& controllerLibrary = *ts.controllerLibrary;
    SkinningSystem& skinningSystem = *ts.skinningSystem;
    PartFollowSystem& partFollowSystem = *ts.partFollowSystem;
    EffectSystem& effectSystem = *ts.effectSystem;
    PhysicsSystem& physicsSystem = *ts.physicsSystem;
    TransformSystem& transformSystem = *ts.transformSystem;
    CollisionSystem& collisionSystem = *ts.collisionSystem;
    ParticleSystem& particleSystem = *ts.particleSystem;
    VfxRenderer& vfxRenderer = *ts.vfxRenderer;
    RenderResources& resources = *ts.resources;
    std::vector<SolidContact>& solidContacts = *ts.solidContacts;
    std::vector<EffectSpawnRequest>& effectQueue = *ts.effectQueue;
    std::vector<DebugLineCmd>& debugLines = *ts.debugLines;
    std::vector<ScriptAudioEvent>& audioQueue = *ts.audioQueue;
    AudioSystem& audioSystem = *ts.audioSystem;
    AudioSourceSystem& audioSources = *ts.audioSources;
    SoundLibrary& soundLibrary = *ts.soundLibrary;
    Pcg32& audioScriptRng = *ts.audioScriptRng;
    uint64_t& audioHandleSeq = *ts.audioHandleSeq;
    std::wstring& pendingScene = *ts.pendingScene;
    int& pendingSaveSlot = *ts.pendingSaveSlot;
    int& pendingLoadSlot = *ts.pendingLoadSlot;
    PrefabLibrary& prefabLibrary = *ts.prefabLibrary;
    const std::wstring& assetsRoot = *ts.assetsRoot;
    const std::wstring& saveDir = *ts.saveDir;

    // ★毎回引き直す (キャッシュしない)。記録は最終 tick の途中で Finish() して
    //   IsActive() が落ちるので、tick 頭の値で判定すると挙動が変わってしまう
    const auto Recording = [&ts] { return ts.recorder != nullptr && ts.recorder->IsActive(); };
    const auto Verifying = [&ts] { return ts.player != nullptr && ts.player->IsActive(); };
    // M52h: ネットのロックステップ中も「巻き戻せない側 / 2 台で揃わない側」を止める
    const auto Networked = [&ts] { return ts.netLockstep; };

    // M51d: アクション評価。tick の入力が確定した直後 (verify の置換の後) に
    // 前 tick との比較で held/pressed/released と軸値を確定する。
    // 記録済みスナップショット 2 枚の純関数なので record/verify に透過 (決定台帳 4)。
    // M52g: レーン数ぶんまとめて評価する (playerCount 以降のレーンは Evaluate がゼロへ落とす)
    inputActions.Evaluate(ctx.inputs, prevTickInput, ctx.playerCount);
    for (uint32_t p = 0; p < kMaxPlayers; ++p) {
        prevTickInput[p] = ctx.inputs[p];
    }
    // M52g: 評価結果を PlayerInputComponent へ写す。**スクリプト層より前**に置くこと —
    // スクリプトはこの tick のミラーを読んで動く。ここで書いた値が tick 末のハッシュに
    // 載ることが、レーンの配線を replay_verify が検査できる唯一の根拠
    UpdatePlayerInputMirror(scene.GetWorld(), inputActions, ctx.inputs, ctx.playerCount);
    // M36b: tick 頭のワールド行列を補間用に採取 (record/verify 中は補間しないので省く)
    if (ts.prevWorld != nullptr && !Recording() && !Verifying()) {
        CapturePrevWorld(*ts.prevWorld, scene.GetWorld());
    }
    // v7 DebugDrawLine (M37): 前 tick の線を捨てて今 tick 分を積み直す。
    // 0 tick フレームでは最後の tick の線が描かれ続ける (意図どおり)
    debugLines.clear();
    // v14 GetContactInfo (M59k): 接触列は**今 tick の物理が書くまで読ませない**。
    // ここで外しておくのが決定論の要 — 接触列は毎 tick 使い回すバッファで
    // SimSnapshot に入っていないので、フェーズ 3 の Update から前 tick の列が
    // 読める状態にすると、タイムトラベル復元 / ネットのロールバック後の再シムで
    // 「巻き戻す前の接触」を読んでハッシュが割れる
    scriptHost.SetTickContacts(nullptr);
    managedHost.SetTickContacts(nullptr);
    if (ts.app != nullptr) {
        ts.app->OnTick(ctx); // エディタ更新 + simulateScripts の決定
    }
    if (ts.lastTickSimulated != nullptr) {
        *ts.lastTickSimulated = ctx.simulateScripts; // M36b: 編集中 (非 Play) は補間を切る
    }
    // M51g: ゲームフローの tick ゲート (決定台帳 5)。ポーズ/タイムスケールは dt を
    // 触らず「この tick でゲート対象を進めるか」で表現する (整数 tick 決定論と噛み合う)。
    // ゲート対象 = アニメ (3.5) / 物理 (3.6、CC 込み) / 衝突・パーティクル・トレイル (4)。
    // スクリプト層 (C++/C#) は非ゲート — 止めると sim レーンからアンポーズ不能になる
    // (ポーズ UI を動かすのもスクリプト)。Transform / 構造変更適用 / 入力 / ハッシュ /
    // シーン遷移も常時実行。Advance() は accum を進める副作用があるので
    // sim が走る tick に 1 回だけ呼ぶ (編集中に呼ぶと Play 開始時の位相がずれる)
    const bool stepSim = ctx.simulateScripts && scene.Time().Advance();
    // ---- フェーズ 3: スクリプト層 Start → Update ----
    const bool runScripts = ctx.simulateScripts && scriptHost.IsLoaded();
    if (runScripts) {
        scriptHost.SetTickContext(ctx.Input(), ctx.tickIndex, ctx.fixedDt);
        scriptHost.RunStartAndUpdate();
    }
    // C# スクリプト層 (別レーン): 記録/検証/ネット中は走らせない → 純 C++ 決定論を保持。
    // 再シム (M52e) でも同じ — C# の状態はスナップショットに入っておらず巻き戻らないので、
    // ここで走らせると「戻せない側だけが余分に進む」= どのみち世界が割れる
    const bool runManaged = ctx.simulateScripts && managedHost.IsReady()
        && !Recording() && !Verifying() && !Networked() && !ts.resim;
    if (runManaged) {
        managedHost.SetTickContext(ctx.Input(), ctx.tickIndex, ctx.fixedDt);
        managedHost.RunStartAndUpdate();
    }
    // ---- 音響 (フェーズ 3.4、M65a): スクリプト層の直後・アニメと物理の前 ----
    // ここに置く理由は 2 つある:
    //   1. スクリプトが書いた発音要求 (AcousticEmitter.pending*) を**同じ tick で**波にする
    //   2. M65f の AgentSystem が書く CharacterController.moveInput を、物理 (3.6) が
    //      **同じ tick で**消費できる (フェーズ 4 に置くと敵の操作が 1 tick 遅れる)
    // ★読む WorldMatrix は**前 tick のもの** (確定は フェーズ 4 の TransformSystem)。
    //   これは物理がコライダを tick 頭の位置で判定しているのと同じ扱いで、意図的な 1 tick 遅延。
    // ★TimeControl のゲート (stepSim) に載せる — ポーズ中に波だけ広がると
    //   「止めたのに音で見える」になるうえ、タイムトラベルのリングとも位相がずれる
    if (stepSim && ts.acoustic != nullptr) {
        MYE_PROFILE_SCOPE("acoustic");
        ts.acoustic->Sync(scene.GetWorld());
    }
    // ---- アニメーション (フェーズ 3.5): スクリプト後・Transform 前に LocalTransform を確定 ----
    // Play 中のみ進行 (編集時は Animation 窓が明示サンプリングする)。M51g からは
    // TimeControl の tick ゲート (stepSim) も掛かる — エフェクトの duration/linger や
    // アニメ時刻などの「タイマー」はここが止まることで一緒に止まる。
    // AnimatorComponent 非存在シーンでは完全 no-op = 既存シーンのリプレイ不変
    if (stepSim) {
        MYE_PROFILE_SCOPE("animation");
        animationSystem.Update(scene.GetWorld(), animLibrary);
        // Animator Controller (M22): ステートマシンでクリップを切替・ブレンド。
        // LocalTransform を駆動するので hash 対象、決定論 (整数 tick・整数比ブレンド)
        controllerSystem.Update(scene.GetWorld(), controllerLibrary, animLibrary);
        // スケルタルアニメの時刻を進める (M18)。ポーズは非ハッシュなのでリプレイ不変
        skinningSystem.Update(scene.GetWorld(), resources);
        // 部位のボーン追従 (M48g): 上で進めた timeTicks のポーズで LocalTransform を作る。
        // **skinning の直後・物理と TransformSystem の前**に置くこと — 同じ tick の
        // WorldMatrix に反映され、追従した部位のコライダ位置も同じ tick で確定する。
        // Part 非存在シーンでは完全 no-op = 既存シーンのリプレイ不変
        partFollowSystem.Update(scene.GetWorld(), resources);
        // 合成エフェクト (M32e): 子エミッタの停止/再開・duration+linger 後の自動破棄。
        // EffectComponent 非存在シーンでは完全 no-op = 既存シーンのリプレイ不変
        effectSystem.Update(scene.GetWorld());
    }
    // ---- 物理 (フェーズ 3.6): スクリプト/アニメ後・Transform 前に剛体を積分 ----
    // LocalTransform.position を書き換えるので TransformSystem 前に走らせ、確定した
    // ワールド位置でコライダ判定させる。Rigidbody 非存在シーンでは完全 no-op (opt-in)
    if (stepSim) {
        MYE_PROFILE_SCOPE("physics");
        // ソリッド接触ペアを受け取り CollisionSystem へ渡す (M28c OnCollision 配信)
        physicsSystem.Update(scene.GetWorld(), ctx.fixedDt, &solidContacts, ts.xpbd);
        // v14 GetContactInfo (M59k): ここから先 (OnCollision* / LateUpdate) だけが読める。
        // stepSim が false の tick は繋がないまま = ポーズ中は常に「接触なし」が返る
        scriptHost.SetTickContacts(&solidContacts);
        managedHost.SetTickContacts(&solidContacts);
    }
    // ---- フェーズ 4: システム層 ----
    // Transform を先に確定 (エミッタ/コライダのワールド位置は tick 決定論の一部)
    {
        MYE_PROFILE_SCOPE("transform");
        transformSystem.Update(scene.GetWorld());
    }
    if (stepSim) {
        {
            MYE_PROFILE_SCOPE("collision");
            // C# にもトリガー配信 (別レーン: 記録/検証中は managed=null で純 C++)
            collisionSystem.Update(scene.GetWorld(), &scriptHost,
                                   runManaged ? &managedHost : nullptr, &solidContacts);
        }
        {
            MYE_PROFILE_SCOPE("particles");
            particleSystem.Update(scene.GetWorld(), ctx.fixedDt);
        }
        // トレイル点列の蓄積 (M29c)。WorldMatrix 確定後の tick 側で 1 回だけ —
        // Render 側だと SceneView/GameView の多重描画で多重サンプルされる
        vfxRenderer.UpdateTrails(scene.GetWorld(), ctx.tickIndex);
        // 物理デバッグ可視化 (M59e)。**出力レーン**なので debugLines へ積むだけで
        // sim には触れない。Transform の後に置くのは速度ベクトルの根元に当 tick の
        // ワールド位置が要るため。再シム (M52e/M52i) は抑止する — 巻き戻すたびに
        // 同じ線を積み直すと、シークの途中経過が 1 フレームに折り重なって見える
        if (!ts.resim) {
            const PhysicsDebugFlags& physDebug = GetPhysicsDebugFlags();
            if (physDebug.Any()) {
                BuildPhysicsDebugLines(scene.GetWorld(), solidContacts, physDebug, debugLines,
                                       ts.xpbd);
            }
        }
    }
    // ---- フェーズ 5: スクリプト層 LateUpdate ----
    if (runScripts) {
        scriptHost.RunLateUpdate();
    }
    if (runManaged) {
        managedHost.RunLateUpdate();
    }
    // ---- エフェクト spawn を drain (M32f): Prefab::Instantiate は内部で構造変更を確定する
    // ため tick 末のここで。verify でもゲートしない = sim 状態として同 tick のハッシュに含まれる。
    // C++ スクリプトは verify 中も走り同一キューを積む → 同一 fileId/EntityID 列で再現される。
    if (!effectQueue.empty()) {
        for (const EffectSpawnRequest& req : effectQueue) {
            std::wstring full = Utf8ToWide(req.prefabKey);
            if (!PrefabLibrary::IsComposePath(full)) {
                full += PrefabLibrary::kPrefabSuffix; // 既定は従来どおり (既存キー互換)
            }
            if (full.find(L':') == std::wstring::npos) {
                full = assetsRoot + L"\\" + full; // assets ルート相対を絶対化
            }
            const uint64_t hash = PrefabLibrary::HashForPath(full);
            if (!prefabLibrary.Contains(hash)) {
                prefabLibrary.LoadFromFile(full);
            }
            const bool hasParent = (req.parent.index != 0u || req.parent.generation != 0u);
            const uint64_t parentFid =
                hasParent ? scene.EnsureFileId(
                                EntityID{ req.parent.index, req.parent.generation })
                          : 0;
            const uint64_t rootFid = Prefab::Instantiate(
                scene, prefabLibrary, hash, parentFid, req.reservedRootFid);
            if (rootFid != 0) {
                const EntityID root = scene.FindByFileId(rootFid).Id();
                if (auto* t = scene.GetWorld().GetComponent<LocalTransform>(root)) {
                    t->position = { req.pos.x, req.pos.y, req.pos.z };
                }
            } else {
                MYE_LOG_WARN("PlayEffect: prefab not found: %s",
                             WideToUtf8(full).c_str());
            }
        }
        effectQueue.clear();
    }
    scene.GetWorld().ApplyStructuralChanges(); // フェーズ 7 (tick 末適用 = ADR-005)

    // ---- 意図的な状態の変異 (M52i、--net-poke-tick N) ----
    // desync 検出と診断チェーン (--rep-diff → --hash-diff) が本当に働くかは、
    // 実際に壊して確かめるしかない (--crash-test と同じ流儀)。
    // ★**ここ (構造変更適用の直後 = ハッシュもダンプも撮る前) に置くのが要点**。
    //   tick の外から壊すと、その tick の記録ハッシュに載らず「再生では再現しない変異」に
    //   なる。--hash-dump より後ろだと、今度は診断ダンプにだけ映らない。ここに置けば
    //   record / verify / ネット / --hash-dump のすべてが同じ 1 フィールドの変異を見る。
    // ★ネット専用ではない (名前は用途由来)。片側のプロセスにだけ渡して使う
    if (config.netPokeTick >= 0 && ctx.tickIndex == static_cast<uint64_t>(config.netPokeTick)) {
        // 壊す先は**ハッシュの走査順で最初に出てくる LocalTransform**。
        // HashWorldDetailed と同じ列を使うので、--hash-diff が指す行と必ず対応する
        std::vector<EntityHash> order;
        uint64_t total = 0;
        HashWorldDetailed(scene.GetWorld(),
                          {&particleSystem.Cpu(), &scene.Time(), &scene.Persist(), ts.xpbd, ts.acoustic}, order, total);
        for (const EntityHash& e : order) {
            if (auto* t = scene.GetWorld().GetComponent<LocalTransform>(e.entity)) {
                t->position.x += 0.001f;
                MYE_LOG_ERROR("[net] --net-poke-tick %llu: corrupted %s LocalTransform.position.x "
                              "on purpose (this build will desync)",
                              static_cast<unsigned long long>(ctx.tickIndex),
                              scene.GetWorld().GetName(e.entity));
                break;
            }
        }
    }

    // ---- ハッシュ差分診断 (M52a): 指定 tick のフィールド単位ダンプ ----
    // ハッシュを撮るのと**同じ点**で撮る (ここより前後だと診断が別の状態を指す)
    if (!config.hashDumpPath.empty()
        && ctx.tickIndex == static_cast<uint64_t>(config.hashDumpTick)) {
        HashDump dump;
        HashWorldDump(scene.GetWorld(),
                      {&particleSystem.Cpu(), &scene.Time(), &scene.Persist(), ts.xpbd, ts.acoustic}, ctx.tickIndex, dump);
        WriteHashDump(config.hashDumpPath, dump);
    }

    // ---- リプレイ: tick 末の状態ハッシュ (spec 11.3) ----
    if (Recording()) {
        ts.recorder->RecordTick(ctx.inputs, ctx.playerCount,
                                HashWorld(scene.GetWorld(),
                                          {&particleSystem.Cpu(), &scene.Time(), &scene.Persist(), ts.xpbd, ts.acoustic}));
        if (ts.recorder->TickCount() >= static_cast<uint64_t>(config.replayTicks)) {
            ts.recorder->Finish();
            ctx.requestExit = true;
        }
    } else if (Verifying()) {
        const uint64_t actual = HashWorld(scene.GetWorld(),
                                          {&particleSystem.Cpu(), &scene.Time(), &scene.Persist(), ts.xpbd, ts.acoustic});
        const uint64_t expected = ts.player->ExpectedHash(ctx.tickIndex);
        if (expected == 0) {
            // ★期待値なし = クラッシュ .rep の「走り切らなかった最後の tick」(M52f)。
            //   照合対象が存在しないので数えるだけ。ここまで来たということは
            //   **落ちずに通り抜けた** = 再現しなかった、という情報そのものになる
            ++ts.player->unverifiedTicks;
            MYE_LOG_WARN("[replay] tick %llu has no expected hash (in-flight tick of a crash "
                         "bundle) - it did NOT crash this time (actual %016llX)",
                         static_cast<unsigned long long>(ctx.tickIndex),
                         static_cast<unsigned long long>(actual));
        } else if (actual != expected) {
            // 乖離: 初回の tick とエンティティ別サブハッシュを報告して失敗終了
            ts.player->failed = true;
            ts.player->firstMismatchTick = ctx.tickIndex;
            MYE_LOG_ERROR("[replay] HASH MISMATCH at tick %llu",
                          static_cast<unsigned long long>(ctx.tickIndex));
            MYE_LOG_ERROR("[replay]   expected %016llX / actual %016llX",
                          static_cast<unsigned long long>(expected),
                          static_cast<unsigned long long>(actual));
            std::vector<EntityHash> detail;
            uint64_t total = 0;
            HashWorldDetailed(scene.GetWorld(),
                              {&particleSystem.Cpu(), &scene.Time(), &scene.Persist(), ts.xpbd, ts.acoustic}, detail, total);
            MYE_LOG_ERROR("[replay]   entities=%zu rng=%016llX", detail.size(),
                          static_cast<unsigned long long>(scene.GetWorld().Rng().State()));
            for (size_t i = 0; i < detail.size() && i < 8; ++i) {
                MYE_LOG_ERROR("[replay]   entity %u:%u hash=%016llX (%s)",
                              detail[i].entity.index, detail[i].entity.generation,
                              static_cast<unsigned long long>(detail[i].hash),
                              scene.GetWorld().GetName(detail[i].entity));
            }
            // M52a: 失敗側のフィールド単位ダンプを自動で残す。
            // 期待側は「同じコマンドで録り直して --hash-dump-tick N」で撮り、
            // --hash-diff で突き合わせる (tools\replay_verify.bat が自動でやる)。
            // tick 番号は bat から読めるよう別ファイルにも落とす
            {
                const std::wstring tickStr = std::to_wstring(ctx.tickIndex);
                const std::wstring dumpPath =
                    config.replayVerifyPath + L".tick" + tickStr + L".actual.dump";
                HashDump dump;
                HashWorldDump(scene.GetWorld(),
                              {&particleSystem.Cpu(), &scene.Time(), &scene.Persist(), ts.xpbd, ts.acoustic}, ctx.tickIndex, dump);
                WriteHashDump(dumpPath, dump);
                std::ofstream mf(
                    std::filesystem::path(config.replayVerifyPath + L".mismatch.txt"));
                if (mf) {
                    mf << ctx.tickIndex << "\n";
                }
                MYE_LOG_ERROR("[replay]   field dump: %s", WideToUtf8(dumpPath).c_str());
            }
            if (ts.exitCode != nullptr) {
                *ts.exitCode = 1;
            }
            ctx.requestExit = true;
        } else {
            ++ts.player->verifiedTicks;
        }
    }

    // ---- オーディオ drain (M19/M45): ハッシュ後に再生する (voice 状態は絶対に
    // hashed state へ戻さない)。記録/検証中は AudioSystem 自体が suspend されており
    // Play が no-op になる。**キューの clear だけはゲートの外**で毎 tick 行う ----
    // 再シム中 (M52e) も同型に抑止する。EngineLoop 側で AudioSystem を suspend しても
    // いるので二重だが、**RunOneTick 単体で出力レーンが閉じている**方が後続の消費者
    // (M52i のロールバック) が抑止を忘れられない
    if (!ts.resim) {
        for (const ScriptAudioEvent& e : audioQueue) {
            ApplyScriptAudioEvent(e, scene.GetWorld(), audioSystem, soundLibrary, audioSources,
                                  audioScriptRng);
        }
    }
    audioQueue.clear();

    // ---- セーブ書出 (M51g): 出力レーン — tick 末ハッシュの後に書く (決定論を
    // 汚さない)。record/verify 中もゲートしない: 同じスクリプトが同じ tick で
    // 同じ内容を要求するだけで、sim 状態は一切読み書きしない。
    // 再シム (M52e) だけはゲートする — 巻き戻しのたびに同じセーブを書き直すのは
    // 「デバッグのために覗いただけ」の副作用として大きすぎる ----
    if (pendingSaveSlot >= 0) {
        const int slot = pendingSaveSlot;
        pendingSaveSlot = -1;
        if (ts.resim) {
            // 再シム (M52e): 同じ内容をもう一度書くだけなので抑止する。
            // 要求の**消費**は上で済ませてある (溜めたまま抜けると次の tick で書いてしまう)
            MYE_LOG_INFO("[save] slot %d write skipped (re-simulating)", slot);
        } else {
            // シーンパスは assets 相対へ落として書く (プロジェクト移動でセーブが死なない
            // ように)。assets 外 (メモリ構築シーン = 空 / 外部絶対パス) はそのまま
            std::wstring sp = scene.SourcePath();
            if (sp.size() > assetsRoot.size() + 1
                && _wcsnicmp(sp.c_str(), assetsRoot.c_str(), assetsRoot.size()) == 0
                && sp[assetsRoot.size()] == L'\\') {
                sp = sp.substr(assetsRoot.size() + 1);
            }
            const std::wstring savePath = SaveGameFile::PathForSlot(saveDir, slot);
            if (SaveGameFile::Write(savePath, sp, scene.Persist())) {
                MYE_LOG_INFO("[save] slot %d written (%zu keys): %s", slot,
                             scene.Persist().Entries().size(),
                             WideToUtf8(savePath).c_str());
            }
        }
    }
    // ---- ロード消費 (M51g): pendingScene と同じセーフポイント。PersistStore を
    // 置換し、記録されたシーンを pendingScene へ積む → 直下のブロックが同 tick で
    // ロードする。record/verify 中は no-op + WARN (「リプレイはセーブ読込を
    // 跨がない」— 決定台帳 5) ----
    if (pendingLoadSlot >= 0) {
        const int slot = pendingLoadSlot;
        pendingLoadSlot = -1;
        if (Recording() || Verifying() || Networked()) {
            MYE_LOG_WARN("[save] LoadGame(slot %d) is a no-op during record/verify/netplay",
                         slot);
        } else {
            SaveGameData data;
            const std::wstring savePath = SaveGameFile::PathForSlot(saveDir, slot);
            if (SaveGameFile::Read(savePath, data)) {
                scene.Persist().Entries() = std::move(data.persist);
                if (!data.scenePath.empty()) {
                    pendingScene = data.scenePath; // assets 相対は下の消費で絶対化
                }
                MYE_LOG_INFO("[save] slot %d loaded (%zu keys)", slot,
                             scene.Persist().Entries().size());
            } else {
                MYE_LOG_WARN("[save] load failed: %s", WideToUtf8(savePath).c_str());
            }
        }
    }

    // ---- シーン遷移 (M19.4): pendingScene が積まれていれば tick 末にロードする ----
    // スクリプトが決定論的に LoadScene → 記録/検証とも同一 tick に再現される。
    // world.Clear (LoadFromFile 内) + carry-state リセット + RNG 決定論的再シードで
    // 新シーンが決定論的に始まる。mid-iteration の world 破棄を避けるため必ず tick 末。
    if (!pendingScene.empty()) {
        std::wstring full = pendingScene;
        pendingScene.clear();
        if (full.find(L':') == std::wstring::npos) {
            full = assetsRoot + L"\\" + full; // assets ルート相対を絶対化
        }
        if (SceneSerializer::LoadFromFile(scene, full)) {
            scene.GetWorld().Rng().Seed(0x4D794531ull); // 決定論的再シード (World 既定値)
            collisionSystem.Reset();
            particleSystem.ResetParticles();
            if (ts.xpbd) {
                ts.xpbd->Reset(); // M60'b: 旧シーンの変形体の池を捨てる
            }
            if (ts.acoustic) {
                ts.acoustic->Reset(); // M65a: 旧シーンの波と占有グリッドを捨てる
            }
            vfxRenderer.Reset(); // M29c: トレイル点列も新シーンでリセット
            partFollowSystem.Reset(); // M48g: 旧シーンの warn 抑制を捨てる
            scriptHost.ClearStarted();
            managedHost.OnSceneReloaded();
            // M45: 旧シーンの SE を断ち、ハンドル採番も 0 から振り直す。
            // 採番列は「スクリプトの呼出順」だけで決まる必要があるので、
            // 記録/検証の別なく **必ず** リセットする (サスペンド中でも進む値のため)。
            // ★M45f: StopAll はボイスプール (SE) だけを止め、**BGM は止めない** —
            //   シーンをまたいで曲が途切れないのが BGM の要件。新シーンが別の曲を
            //   指定すれば PlayMusic 側でクロスフェードし、同じ曲なら鳴り続ける
            audioSystem.StopAll();
            audioSources.Reset(); // 旧シーンの音源キャッシュ (速度推定 / 再生済みフラグ)
            audioHandleSeq = 0;
            MYE_LOG_INFO("[scene] loaded: %s", WideToUtf8(full).c_str());
        } else {
            MYE_LOG_WARN("[scene] load failed: %s", WideToUtf8(full).c_str());
        }
    }

    ++ctx.tickIndex;
}

} // namespace mye
