#include <cstdio>
#include <filesystem>
#include <string>

#include <Windows.h>
#include <shellapi.h>

#include "Engine/Core/Log.h"
#include "Engine/Engine/DemoContent.h"
#include "Engine/Engine/EngineLoop.h"
#include "Engine/Engine/Prefab.h"
#include "Engine/Engine/Project.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/SceneSerializer.h"
#include "Engine/Platform/CrashHandler.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/ShaderManager.h"

namespace {

// コンソールから起動された場合に標準出力を繋ぐ (CLI/リプレイ検証用)。EditorMain と同じ方針
void AttachParentConsole()
{
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    const bool redirected = (out != nullptr && out != INVALID_HANDLE_VALUE);
    if (!redirected && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
}

// エディタ UI 無しの薄いランタイム (engine_spec.md 1.4 / M15)。
// リソース + シーンを読み、常時シミュレートしてバックバッファへ直接描画する。
// エンジン (EngineLoop) と GameLogic.dll は Editor と完全に共有 —
// よって同一シーンのシミュレーションは Editor とビット単位で一致する (リプレイ検証で実証)。
class RuntimeApp : public mye::IEngineApp {
public:
    std::wstring scenePath;
    bool startDeferred = false;
    bool rtShowcase = false; // --rt-demo (M46i: コーネル箱のショーケース)

    void OnStart(mye::EngineContext& ctx) override
    {
        ctx.shaders->Load("forward_lit");
        mye::RegisterDemoContent(ctx);   // Editor と同じ実体登録 (AssetID 解決)
        mye::RegisterAssetLibraries(ctx); // .prefab / .anim を登録
        if (scenePath.empty() && rtShowcase) {
            // M46i: ショーケースはブートシーンと別枠。保存済みファイルがあればそれを読み、
            // 無ければコードから組む (main.scene.json には一切触らない)
            scenePath = ctx.assetsRoot + L"\\scenes\\rt_showcase.scene.json";
        } else if (scenePath.empty()) {
            scenePath = ctx.assetsRoot + L"\\scenes\\main.scene.json";
            mye::ProjectManifest manifest; // ブートシーンはマニフェスト優先 (M26)
            if (!ctx.projectRoot.empty() && mye::LoadProjectManifest(ctx.projectRoot, manifest)) {
                scenePath = mye::ProjectBootScenePath(ctx.projectRoot, manifest);
            }
        }
        // ショーケース材質は無条件で登録する (M50a)。--scene で保存済みショーケースを
        // 直接開く経路 (replay_verify の Runtime 側 parts 検証もこれ) でも実体が揃う。
        // Runtime には --parts-demo が無いので、ゲートしたままだと parts 材質は常に欠落する
        mye::RegisterRtShowcaseContent(ctx);
        mye::RegisterPartsShowcaseContent(ctx);
        mye::RegisterFlowShowcaseContent(ctx); // M51j: flow_* 材質 (配布ブートシーンにも使う)
        if (std::filesystem::exists(scenePath)) {
            mye::SceneSerializer::LoadFromFile(*ctx.scene, scenePath);
            // Editor と同じ「ロード直後 1 回」(M48e)。ここを揃えないと Editor で録った .rep と
            // Runtime の verify で初期状態が食い違う
            mye::Prefab::RefreshNonOverridden(*ctx.scene, *ctx.prefabs);
        } else if (rtShowcase) {
            mye::BuildRtShowcaseScene(ctx); // M46i
        } else {
            mye::BuildDemoScene(ctx); // ブートシーンが無ければデモを構築
        }
        // ランタイムは即 Play 相当。Editor の PlayModeController::Play と同じ Save+Load リロードで
        // EntityID を正規化する — これにより Editor が録った .rep と決定論的に一致する (M8 規約)。
        {
            const nlohmann::json snap = mye::SceneSerializer::SaveToJson(*ctx.scene);
            mye::SceneSerializer::LoadFromJson(*ctx.scene, snap);
        }
        if (startDeferred) {
            ctx.renderPath = ctx.renderPathDeferred;
        }
        MYE_LOG_INFO("RuntimeApp started (%u entities, scene=%s)",
                     ctx.scene->GetWorld().AliveCount(), mye::WideToUtf8(scenePath).c_str());
    }

    // ランタイムは常時シミュレート (Editor の Play 相当)
    void OnTick(mye::EngineContext& ctx) override { ctx.simulateScripts = true; }
};

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    AttachParentConsole();

    mye::EngineConfig config;
    config.title = L"MyEngine Runtime";
    config.renderSceneToBackbuffer = true; // シーンをバックバッファへ直接描画
    config.enableImGui = false;            // エディタ UI 無し

    RuntimeApp app;
    std::wstring hashDiffA; // --hash-diff A B (M52a)
    std::wstring hashDiffB;
    std::wstring crashTestArg; // --crash-test <kind> (M52f)

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            const std::wstring arg = argv[i];
            if (arg == L"--frames" && i + 1 < argc) {
                config.maxFrames = _wtoi64(argv[++i]);
            } else if (arg == L"--width" && i + 1 < argc) {
                config.width = _wtoi(argv[++i]);
            } else if (arg == L"--height" && i + 1 < argc) {
                config.height = _wtoi(argv[++i]);
            } else if (arg == L"--no-vsync") {
                config.vsync = false;
            } else if (arg == L"--screenshot" && i + 1 < argc) {
                config.screenshotPath = argv[++i];
            } else if (arg == L"--shot-frame" && i + 1 < argc) {
                config.screenshotFrame = _wtoi64(argv[++i]);
            } else if (arg == L"--shot-every" && i + 1 < argc) {
                config.screenshotEvery = _wtoi64(argv[++i]);
            } else if (arg == L"--scene" && i + 1 < argc) {
                app.scenePath = argv[++i];
            } else if (arg == L"--replay-record" && i + 1 < argc) {
                config.replayRecordPath = argv[++i];
                config.vsync = false;
            } else if (arg == L"--replay-verify" && i + 1 < argc) {
                config.replayVerifyPath = argv[++i];
                config.vsync = false;
            } else if (arg == L"--replay-ticks" && i + 1 < argc) {
                config.replayTicks = _wtoi64(argv[++i]);
            } else if (arg == L"--rep-snapshot") {
                // M52d: .rep へ記録開始時点の sim 状態を埋め込む (シーン非依存の再生)
                config.replayEmbedSnapshot = true;
            } else if (arg == L"--hash-dump" && i + 1 < argc) {
                config.hashDumpPath = argv[++i]; // M52a: フィールド単位ダンプの出力先
            } else if (arg == L"--hash-dump-tick" && i + 1 < argc) {
                config.hashDumpTick = _wtoi64(argv[++i]);
            } else if (arg == L"--snapshot-stress" && i + 1 < argc) {
                // M52d: N tick ごとにスナップショット往復を挟む (期待ハッシュ不変が合格条件)
                config.snapshotStress = _wtoi64(argv[++i]);
            } else if (arg == L"--timetravel-selftest") {
                // M52e: 巻き戻し + 再シムのハッシュ照合 (tick 数は省略可、既定 400)。
                // Runtime は常に sim を進めるので --autoplay 相当の細工は要らない
                config.timeTravelProbeTicks = 400;
                if (i + 1 < argc && argv[i + 1][0] != L'-') {
                    config.timeTravelProbeTicks = _wtoi64(argv[++i]);
                }
                config.vsync = false;
            } else if (arg == L"--crash-test" && i + 1 < argc) {
                crashTestArg = argv[++i]; // M52f (綴り違いは下で弾く)
                config.vsync = false;
            } else if (arg == L"--crash-at-tick" && i + 1 < argc) {
                config.crashTestTick = _wtoi64(argv[++i]);
            } else if (arg == L"--no-crash-handler") {
                config.crashHandler = false; // M52f: 既定 on を外す (デバッガ下での切り分け用)
            } else if (arg == L"--hash-diff" && i + 2 < argc) {
                // M52a: 配布ビルド単体でもクラッシュ報告のダンプを突き合わせられるように
                hashDiffA = argv[++i];
                hashDiffB = argv[++i];
            } else if (arg == L"--deferred") {
                app.startDeferred = true;
            } else if (arg == L"--postfx-mode" && i + 1 < argc) {
                config.postFxTonemap = _wtoi(argv[++i]);
            } else if (arg == L"--no-postfx") {
                config.postFx = false;
            } else if (arg == L"--no-audio") {
                config.audio = false; // M45: XAudio2 を初期化しない (端末の無い CI / 撮影専用実行)
            } else if (arg == L"--font-embedded") {
                config.fontEmbedded = true; // M52c: 撮影のフォントを機種非依存に固定
            } else if (arg == L"--shot-realtime") {
                config.shotRealtime = true; // M52c: 決定的撮影を解除して実時間で回す
            } else if (arg == L"--warp") {
                config.forceWarp = true; // M52b: ソフトウェアラスタライザ固定 (CI / 撮影再現)
            } else if (arg == L"--exposure" && i + 1 < argc) {
                config.postFxExposure = static_cast<float>(_wtof(argv[++i]));
            } else if (arg == L"--no-bloom") {
                config.postFxBloom = false;
            } else if (arg == L"--bloom-threshold" && i + 1 < argc) {
                config.postFxBloomThreshold = static_cast<float>(_wtof(argv[++i]));
            } else if (arg == L"--bloom-intensity" && i + 1 < argc) {
                config.postFxBloomIntensity = static_cast<float>(_wtof(argv[++i]));
            } else if (arg == L"--no-fxaa") {
                config.postFxFxaa = false;
            } else if (arg == L"--no-jobs") {
                config.useJobs = false; // M25: 並列を直列化 (決定論ゲート / 計測比較)
            } else if (arg == L"--no-sim-cache") {
                config.useSimCache = false; // M51a: sim 索引を素通し (決定論ゲート / 切り分け)
            } else if (arg == L"--no-cook-cache") {
                config.useCookCache = false; // M51b: クックを使わず毎回フルパース (切り分け)
            } else if (arg == L"--rt-debug" && i + 1 < argc) {
                config.rtDebugMode = _wtoi(argv[++i]); // M46b (Deferred のみ)
            } else if (arg == L"--rt-no-temporal") {
                config.rtTemporal = false; // M46d: 1spp 生のまま (A/B 計測用)
            } else if (arg == L"--rt-freeze-seed") {
                config.rtFreezeSeed = true; // M46d: 乱数列を進めない (決定的スクショ)
            } else if (arg == L"--rt-anim-seed") {
                config.rtAnimSeed = true; // M46d: スクショ時の自動 freeze を解除
            } else if (arg == L"--rt-no-svgf") {
                config.rtSvgf = false; // M46e: 空間フィルタ off (蓄積のみ = A/B 計測用)
            } else if (arg == L"--rt-gi") {
                config.rtGi = true; // M46f: GI を最終画像へ合成 (Deferred のみ)
            } else if (arg == L"--rt-shadow") {
                config.rtShadow = true; // M46g: 平行光の影をレイトレで (Deferred のみ)
            } else if (arg == L"--rt-refl") {
                config.rtRefl = true; // M46h: スペキュラ環境項をレイトレ反射で (Deferred のみ)
            } else if (arg == L"--rt-demo") {
                app.rtShowcase = true; // M46i: コーネル箱のショーケースシーンを構築
            } else if (arg == L"--project" && i + 1 < argc) {
                // M26: プロジェクト指定。dist 配布物は従来どおり exe 隣の assets を自動発見する
                config.projectRoot = std::filesystem::absolute(argv[++i]).wstring();
            }
        }
        LocalFree(argv);
    }

    // --crash-test の綴り違いを黙って無視しない (M52f)。
    // 「落とすつもりで走らせたのに何も起きない」を 1 時間追いかける事故を潰す
    if (!crashTestArg.empty()) {
        const mye::CrashTestKind kind = mye::ParseCrashTestKind(crashTestArg.c_str());
        if (kind == mye::CrashTestKind::None) {
            std::fprintf(stderr,
                         "unknown --crash-test kind: %s "
                         "(av | purecall | terminate | invalidparam | stackoverflow)\n",
                         mye::WideToUtf8(crashTestArg).c_str());
            return 2;
        }
        config.crashTest = static_cast<int>(kind);
    }

    // --hash-diff A B: ダンプ 2 本を突き合わせて終了 (M52a)。同一なら 0、食い違えば 1
    if (!hashDiffA.empty() && !hashDiffB.empty()) {
        mye::HashDump a;
        mye::HashDump b;
        if (!mye::ReadHashDump(hashDiffA, a) || !mye::ReadHashDump(hashDiffB, b)) {
            return 2;
        }
        return mye::DiffHashDumps(a, b).Same() ? 0 : 1;
    }

    mye::EngineLoop loop;
    return loop.Run(config, app);
}
