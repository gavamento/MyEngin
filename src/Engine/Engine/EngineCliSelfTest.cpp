#include "Engine/Engine/EngineCliSelfTest.h"

#include <initializer_list>
#include <string>
#include <vector>

#include "Engine/Core/Log.h"
#include "Engine/Engine/EngineCli.h"
#include "Engine/Engine/EngineLoop.h"
#include "Engine/Engine/ShowcaseScenes.h"

namespace mye {
namespace {

struct ParseRun {
    EngineConfig config;
    EngineCliExtras extras;
    int consumed = 0;
    int notMine = 0;
    int errors = 0;
};

// Main と同じループで回す (NotMine の引数は読み飛ばす = Main 側の分岐が拾う位置)
ParseRun RunParse(std::initializer_list<const wchar_t*> args)
{
    std::vector<std::wstring> storage;
    storage.emplace_back(L"engine.exe");
    for (const wchar_t* a : args) {
        storage.emplace_back(a);
    }
    std::vector<wchar_t*> argv;
    for (std::wstring& s : storage) {
        argv.push_back(s.data());
    }
    ParseRun r;
    const int argc = static_cast<int>(argv.size());
    for (int i = 1; i < argc; ++i) {
        switch (ParseEngineCliFlag(argc, argv.data(), i, r.config, r.extras)) {
        case CliParse::Consumed:
            ++r.consumed;
            break;
        case CliParse::NotMine:
            ++r.notMine;
            break;
        case CliParse::Error:
            ++r.errors;
            break;
        }
    }
    return r;
}

} // namespace

bool RunEngineCliSelfTest()
{
    MYE_LOG_INFO("==== Engine CLI flag table self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };
    const EngineConfig def;
    ParseRun r;

    // ---- 値を取らないフラグ (1 本ずつ。どのフィールドへ入るかを固定する) ----
    r = RunParse({ L"--no-vsync" });
    check(r.consumed == 1 && !r.config.vsync, "--no-vsync");
    r = RunParse({ L"--font-embedded" });
    check(r.consumed == 1 && r.config.fontEmbedded, "--font-embedded");
    r = RunParse({ L"--shot-realtime" });
    check(r.consumed == 1 && r.config.shotRealtime, "--shot-realtime");
    r = RunParse({ L"--replay-fast" });
    check(r.consumed == 1 && r.config.replayFast, "--replay-fast");
    r = RunParse({ L"--rep-snapshot" });
    check(r.consumed == 1 && r.config.replayEmbedSnapshot, "--rep-snapshot");
    r = RunParse({ L"--no-crash-handler" });
    check(r.consumed == 1 && !r.config.crashHandler, "--no-crash-handler");
    r = RunParse({ L"--net-no-rollback" });
    check(r.consumed == 1 && !r.config.netRollback, "--net-no-rollback");
    r = RunParse({ L"--net-no-halt-on-desync" });
    check(r.consumed == 1 && !r.config.netHaltOnDesync, "--net-no-halt-on-desync");
    r = RunParse({ L"--synth-input" });
    check(r.consumed == 1 && r.config.synthInput, "--synth-input");
    r = RunParse({ L"--ui-demo-input" });
    check(r.consumed == 1 && r.config.uiDemoInput, "--ui-demo-input");
    r = RunParse({ L"--no-postfx" });
    check(r.consumed == 1 && !r.config.postFx, "--no-postfx");
    r = RunParse({ L"--no-audio" });
    check(r.consumed == 1 && !r.config.audio, "--no-audio");
    r = RunParse({ L"--warp" });
    check(r.consumed == 1 && r.config.forceWarp, "--warp");
    r = RunParse({ L"--no-bloom" });
    check(r.consumed == 1 && !r.config.postFxBloom, "--no-bloom");
    r = RunParse({ L"--no-fxaa" });
    check(r.consumed == 1 && !r.config.postFxFxaa, "--no-fxaa");
    r = RunParse({ L"--taa" });
    check(r.consumed == 1 && r.config.postFxTaa, "--taa");
    r = RunParse({ L"--no-jobs" });
    check(r.consumed == 1 && !r.config.useJobs, "--no-jobs");
    r = RunParse({ L"--no-sim-cache" });
    check(r.consumed == 1 && !r.config.useSimCache, "--no-sim-cache");
    r = RunParse({ L"--no-cook-cache" });
    check(r.consumed == 1 && !r.config.useCookCache, "--no-cook-cache");
    r = RunParse({ L"--velocity-debug" });
    check(r.consumed == 1 && r.config.velocityDebug == 1, "--velocity-debug");
    r = RunParse({ L"--ssr" });
    check(r.consumed == 1 && r.config.ssr, "--ssr");
    r = RunParse({ L"--probe-bake-all" });
    check(r.consumed == 1 && r.config.probeBakeAll, "--probe-bake-all");
    r = RunParse({ L"--rt-no-temporal" });
    check(r.consumed == 1 && !r.config.rtTemporal, "--rt-no-temporal");
    r = RunParse({ L"--rt-freeze-seed" });
    check(r.consumed == 1 && r.config.rtFreezeSeed, "--rt-freeze-seed");
    r = RunParse({ L"--rt-anim-seed" });
    check(r.consumed == 1 && r.config.rtAnimSeed, "--rt-anim-seed");
    r = RunParse({ L"--rt-no-svgf" });
    check(r.consumed == 1 && !r.config.rtSvgf, "--rt-no-svgf");
    r = RunParse({ L"--rt-gi" });
    check(r.consumed == 1 && r.config.rtGi, "--rt-gi");
    r = RunParse({ L"--rt-shadow" });
    check(r.consumed == 1 && r.config.rtShadow, "--rt-shadow");
    r = RunParse({ L"--rt-refl" });
    check(r.consumed == 1 && r.config.rtRefl, "--rt-refl");
    r = RunParse({ L"--rt-restir" });
    check(r.consumed == 1 && r.config.rtRestir && !r.config.rtRestirSpatial, "--rt-restir");
    r = RunParse({ L"--rt-restir-spatial" });
    check(r.consumed == 1 && r.config.rtRestirSpatial && r.config.rtRestir,
          "--rt-restir-spatial also turns ReSTIR on");
    r = RunParse({ L"--rt-restir-spatial", L"--rt-restir-no-spatial" });
    check(r.consumed == 2 && !r.config.rtRestirSpatial && r.config.rtRestir,
          "--rt-restir-no-spatial turns spatial off and keeps ReSTIR on");
    r = RunParse({ L"--rt-restir-visray" });
    check(r.consumed == 1 && r.config.rtRestirVisRay && r.config.rtRestirSpatial && r.config.rtRestir,
          "--rt-restir-visray also turns spatial reuse and ReSTIR on");
    r = RunParse({ L"--froxel" });
    check(r.consumed == 1 && r.config.froxel && r.config.froxelTemporal, "--froxel");
    r = RunParse({ L"--froxel-no-temporal" });
    check(r.consumed == 1 && !r.config.froxelTemporal && r.config.froxel,
          "--froxel-no-temporal also turns froxels on");
    r = RunParse({ L"--no-acoustic-front" });
    check(r.consumed == 1 && !r.config.acousticFront, "--no-acoustic-front");
    r = RunParse({ L"--particle-compare" });
    check(r.consumed == 1 && r.config.particleCompareOverride == 1, "--particle-compare");

    // ---- 値を 1 つ取るフラグ ----
    r = RunParse({ L"--frames", L"123" });
    check(r.consumed == 1 && r.config.maxFrames == 123, "--frames N");
    r = RunParse({ L"--width", L"640", L"--height", L"360" });
    check(r.consumed == 2 && r.config.width == 640 && r.config.height == 360, "--width N / --height N");
    r = RunParse({ L"--screenshot", L"out.png" });
    check(r.consumed == 1 && r.config.screenshotPath == L"out.png", "--screenshot PATH");
    r = RunParse({ L"--shot-frame", L"77", L"--shot-every", L"5" });
    check(r.consumed == 2 && r.config.screenshotFrame == 77 && r.config.screenshotEvery == 5,
          "--shot-frame N / --shot-every N");
    r = RunParse({ L"--replay-record", L"a.rep" });
    check(r.consumed == 1 && r.config.replayRecordPath == L"a.rep" && !r.config.vsync,
          "--replay-record PATH also turns vsync off");
    r = RunParse({ L"--replay-verify", L"b.rep" });
    check(r.consumed == 1 && r.config.replayVerifyPath == L"b.rep" && !r.config.vsync,
          "--replay-verify PATH also turns vsync off");
    r = RunParse({ L"--replay-ticks", L"900" });
    check(r.consumed == 1 && r.config.replayTicks == 900, "--replay-ticks N");
    r = RunParse({ L"--hash-dump", L"d.dump", L"--hash-dump-tick", L"42" });
    check(r.consumed == 2 && r.config.hashDumpPath == L"d.dump" && r.config.hashDumpTick == 42,
          "--hash-dump PATH / --hash-dump-tick N");
    r = RunParse({ L"--snapshot-stress", L"37" });
    check(r.consumed == 1 && r.config.snapshotStress == 37, "--snapshot-stress N");
    r = RunParse({ L"--crash-test", L"av" });
    check(r.consumed == 1 && r.extras.crashTestArg == L"av" && !r.config.vsync && r.config.crashTest == 0,
          "--crash-test KIND keeps the raw kind for the Main to validate and turns vsync off");
    r = RunParse({ L"--crash-at-tick", L"9", L"--crash-hash-interval", L"1" });
    check(r.consumed == 2 && r.config.crashTestTick == 9 && r.config.crashHashInterval == 1,
          "--crash-at-tick N / --crash-hash-interval N");
    r = RunParse({ L"--net-join", L"127.0.0.1:7777" });
    check(r.consumed == 1 && r.config.netRole == 2 && r.config.netJoinTarget == L"127.0.0.1:7777",
          "--net-join HOST:PORT");
    r = RunParse({ L"--net-players", L"3", L"--net-delay", L"5", L"--net-loss", L"10" });
    check(r.consumed == 3 && r.config.netPlayers == 3 && r.config.netInputDelay == 5
              && r.config.netLossPercent == 10,
          "--net-players N / --net-delay N / --net-loss N");
    r = RunParse({ L"--net-poke-tick", L"300" });
    check(r.consumed == 1 && r.config.netPokeTick == 300, "--net-poke-tick N");
    r = RunParse({ L"--local-players", L"4" });
    check(r.consumed == 1 && r.config.localPlayers == 4, "--local-players N");
    r = RunParse({ L"--postfx-mode", L"2" });
    check(r.consumed == 1 && r.config.postFxTonemap == 2, "--postfx-mode N");
    r = RunParse({ L"--exposure", L"1.5", L"--bloom-threshold", L"2", L"--bloom-intensity", L"0.75" });
    check(r.consumed == 3 && r.config.postFxExposure == 1.5f && r.config.postFxBloomThreshold == 2.0f
              && r.config.postFxBloomIntensity == 0.75f,
          "--exposure F / --bloom-threshold F / --bloom-intensity F");
    r = RunParse({ L"--motion-blur", L"0.25" });
    check(r.consumed == 1 && r.config.postFxMotionBlur == 0.25f, "--motion-blur F");
    r = RunParse({ L"--rt-debug", L"12", L"--hzb-debug", L"3" });
    check(r.consumed == 2 && r.config.rtDebugMode == 12 && r.config.hzbDebug == 3, "--rt-debug N / --hzb-debug N");
    r = RunParse({ L"--probe-bake", L"1,2,3" });
    check(r.consumed == 1 && r.config.probeBake && r.config.probeBakePos[0] == 1.0f
              && r.config.probeBakePos[1] == 2.0f && r.config.probeBakePos[2] == 3.0f,
          "--probe-bake X,Y,Z");
    r = RunParse({ L"--probe-bake", L"here" });
    check(r.consumed == 1 && r.config.probeBake && r.config.probeBakePos[1] == def.probeBakePos[1],
          "--probe-bake with an unreadable position still bakes at the default position");
    r = RunParse({ L"--probe-bake-frame", L"7", L"--probe-bake-png", L"faces.png" });
    check(r.consumed == 2 && r.config.probeBakeFrame == 7 && r.config.probeBakePng == L"faces.png",
          "--probe-bake-frame N / --probe-bake-png PATH");
    r = RunParse({ L"--rt-class-override", L"2" });
    check(r.consumed == 1 && r.config.rtClassOverride == 2 && !r.config.rtRestir,
          "--rt-class-override N does not turn ReSTIR on");
    r = RunParse({ L"--froxel-dump", L"4" });
    check(r.consumed == 1 && r.config.froxelDumpFrame == 4 && r.config.froxel,
          "--froxel-dump N also turns froxels on");
    r = RunParse({ L"--acoustic-dump", L"6" });
    check(r.consumed == 1 && r.config.acousticDumpFrame == 6 && !r.config.froxel,
          "--acoustic-dump N turns nothing else on");
    r = RunParse({ L"--acoustic-audio-log", L"120" });
    check(r.consumed == 1 && r.config.acousticAudioLogTicks == 120, "--acoustic-audio-log N");
    r = RunParse({ L"--particle-backend", L"gpu" });
    check(r.consumed == 1 && r.config.particleBackendOverride == 1, "--particle-backend gpu");
    r = RunParse({ L"--particle-backend", L"cpu" });
    check(r.consumed == 1 && r.config.particleBackendOverride == 0, "--particle-backend cpu");
    r = RunParse({ L"--particle-backend", L"metal" });
    check(r.errors == 1 && r.config.particleBackendOverride == -1,
          "--particle-backend with an unknown value is an error and changes nothing");
    r = RunParse({ L"--modal-backend", L"d3d11cs" });
    check(r.consumed == 1 && r.config.modalBackendName == L"d3d11cs",
          "--modal-backend d3d11cs (spelling accepted; the runtime fallback lives in "
          "ModalSoundLibrary::SetBackendByName)");
    r = RunParse({ L"--modal-backend", L"cpu" });
    check(r.consumed == 1 && r.config.modalBackendName == L"cpu", "--modal-backend cpu");
    r = RunParse({ L"--modal-backend", L"foo" });
    check(r.errors == 1 && r.config.modalBackendName == def.modalBackendName,
          "--modal-backend with an unknown value is an error and changes nothing");
    // M76f: 衝突音のログ + 同期焼き (--modal-demo の byte 一致検証が使う 2 本)
    r = RunParse({ L"--modal-audio-log", L"300" });
    check(r.consumed == 1 && r.config.modalAudioLogTicks == 300, "--modal-audio-log N");
    check(def.modalAudioLogTicks == 0, "--modal-audio-log defaults to 0 (no lines)");
    r = RunParse({ L"--modal-sync-bake" });
    check(r.consumed == 1 && r.config.modalSyncBake, "--modal-sync-bake sets the flag");
    check(!def.modalSyncBake, "--modal-sync-bake defaults to off");
    // M76h: 耳確認の調査ツール (--acoustic-dump / --froxel-dump と同じ系列)
    r = RunParse({ L"--modal-wav-dump", L"out_dir" });
    check(r.consumed == 1 && r.config.modalWavDumpDir == L"out_dir", "--modal-wav-dump DIR");
    check(def.modalWavDumpDir.empty(), "--modal-wav-dump defaults to off (empty)");
    r = RunParse({ L"--modal-face-probe" });
    check(r.consumed == 1 && r.config.modalFaceProbe, "--modal-face-probe sets the flag");
    check(!def.modalFaceProbe, "--modal-face-probe defaults to off");

    // ---- 値を 2 つ取るフラグ ----
    r = RunParse({ L"--rep-diff", L"a.rep", L"b.rep" });
    check(r.consumed == 1 && r.extras.repDiffA == L"a.rep" && r.extras.repDiffB == L"b.rep", "--rep-diff A B");
    r = RunParse({ L"--hash-diff", L"a.dump", L"b.dump" });
    check(r.consumed == 1 && r.extras.hashDiffA == L"a.dump" && r.extras.hashDiffB == L"b.dump",
          "--hash-diff A B");

    // ---- 値を省略できるフラグ ----
    r = RunParse({ L"--timetravel-selftest" });
    check(r.consumed == 1 && r.config.timeTravelProbeTicks == 400 && !r.config.vsync,
          "--timetravel-selftest defaults to 400 ticks and turns vsync off");
    r = RunParse({ L"--timetravel-selftest", L"250" });
    check(r.consumed == 1 && r.config.timeTravelProbeTicks == 250, "--timetravel-selftest N");
    r = RunParse({ L"--timetravel-selftest", L"--warp" });
    check(r.consumed == 2 && r.config.timeTravelProbeTicks == 400 && r.config.forceWarp,
          "an optional value never swallows the next flag");
    r = RunParse({ L"--whatif-selftest" });
    check(r.consumed == 1 && r.config.whatIfProbeTicks == 400 && r.config.synthInput && !r.config.vsync,
          "--whatif-selftest defaults to 400 ticks, synthetic input and no vsync");
    r = RunParse({ L"--whatif-selftest", L"300" });
    check(r.consumed == 1 && r.config.whatIfProbeTicks == 300, "--whatif-selftest N");
    r = RunParse({ L"--net-host" });
    check(r.consumed == 1 && r.config.netRole == 1 && r.config.netPort == def.netPort,
          "--net-host keeps the default port");
    r = RunParse({ L"--net-host", L"9000" });
    check(r.consumed == 1 && r.config.netRole == 1 && r.config.netPort == 9000, "--net-host PORT");

    // ---- 共通フラグとして読まないもの ----
    r = RunParse({ L"--frames" });
    check(r.consumed == 0 && r.notMine == 1 && r.config.maxFrames == def.maxFrames,
          "a flag missing its value is left to the Main (NotMine) and changes nothing");
    r = RunParse({ L"--rep-diff", L"only-one.rep" });
    check(r.consumed == 0 && r.notMine == 2 && r.extras.repDiffA.empty(),
          "a two-value flag with one value is left to the Main");
    r = RunParse({ L"--particle-backend" });
    check(r.errors == 0 && r.notMine == 1, "--particle-backend without a value is not an error");
    r = RunParse({ L"--modal-backend" });
    check(r.errors == 0 && r.notMine == 1, "--modal-backend without a value is not an error");
    r = RunParse({ L"--selftest", L"--scene", L"x.scene.json", L"--deferred", L"--ui-demo" });
    check(r.consumed == 0 && r.notMine == 5, "app-only flags (and their values) are not shared flags");

    // ---- --*-demo の表 (ShowcaseScenes.h) ----
    const ShowcaseDef* rtDemo = FindShowcase(L"--rt-demo", true);
    const ShowcaseDef* uiDemo = FindShowcase(L"--ui-demo", true);
    check(rtDemo != nullptr && uiDemo != nullptr && PickShowcase(uiDemo, rtDemo) == rtDemo
              && PickShowcase(rtDemo, uiDemo) == rtDemo && PickShowcase(nullptr, uiDemo) == uiDemo,
          "several --*-demo flags: the upper row of the table wins whatever order they are passed in");
    check(FindShowcase(L"--parts-demo", true) != nullptr && FindShowcase(L"--parts-demo", false) == nullptr
              && FindShowcase(L"--flow-demo", false) == nullptr && FindShowcase(L"--ui-demo", false) == uiDemo,
          "editor-only demos are not offered to the Runtime");
    check(rtDemo != nullptr
              && ShowcaseScenePath(*rtDemo, L"c:\\p\\assets") == L"c:\\p\\assets\\scenes\\rt_showcase.scene.json",
          "--rt-demo saves under assets\\scenes");
    check(uiDemo != nullptr && ShowcaseScenePath(*uiDemo, L"c:\\p\\assets") == L"cache\\ui_showcase.scene.json",
          "--ui-demo saves under cache");
    const ShowcaseDef* flowDemo = FindShowcase(L"--flow-demo", true);
    check(flowDemo != nullptr && flowDemo->prepare != nullptr && flowDemo->build == nullptr,
          "--flow-demo prepares its scene files instead of building a scene");
    check(FindShowcase(L"--frames", true) == nullptr, "shared flags are not demos");

    MYE_LOG_INFO("Engine CLI self test: %s (%d failure(s))", failCount == 0 ? "OK" : "FAILED", failCount);
    return failCount == 0;
}

} // namespace mye
