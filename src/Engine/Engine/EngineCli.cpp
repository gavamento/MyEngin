#include "Engine/Engine/EngineCli.h"

#include <cstdio>
#include <cstdlib>
#include <cwchar>

#include "Engine/Engine/EngineLoop.h"

namespace mye {
namespace {

enum class CliValue {
    None,     // フラグだけ
    One,      // 値 1 つ。足りなければ共通フラグとして扱わない (NotMine)
    Two,      // 値 2 つ。同上
    Optional, // 次の引数が '-' で始まらなければ値として読む (省略時は既定値)
};

// 1 フラグの適用。v1 / v2 = 値 (無ければ nullptr)
struct CliArgs {
    EngineConfig& c;
    EngineCliExtras& x;
    const wchar_t* v1;
    const wchar_t* v2;
};

struct CliFlag {
    const wchar_t* name;
    CliValue value;
    bool (*apply)(CliArgs& a); // false = 値の綴り違い
};

// ★Editor と Runtime で**意味が同じ**フラグだけを置く。片方にしか無いフラグ (--scene / --deferred /
//   --*-demo / --project など) と、片方だけが足す副作用 (Editor の --autoplay 相当) は各 Main に残す
const CliFlag kEngineCliFlags[] = {
    // ---- ウィンドウ / 撮影 ----
    { L"--frames", CliValue::One, [](CliArgs& a) { a.c.maxFrames = _wtoi64(a.v1); return true; } },
    { L"--width", CliValue::One, [](CliArgs& a) { a.c.width = _wtoi(a.v1); return true; } },
    { L"--height", CliValue::One, [](CliArgs& a) { a.c.height = _wtoi(a.v1); return true; } },
    { L"--no-vsync", CliValue::None, [](CliArgs& a) { a.c.vsync = false; return true; } },
    { L"--screenshot", CliValue::One, [](CliArgs& a) { a.c.screenshotPath = a.v1; return true; } },
    { L"--shot-frame", CliValue::One, [](CliArgs& a) { a.c.screenshotFrame = _wtoi64(a.v1); return true; } },
    { L"--shot-every", CliValue::One, [](CliArgs& a) { a.c.screenshotEvery = _wtoi64(a.v1); return true; } },
    // M52c: 撮影のフォントを機種非依存に固定する / 決定的撮影を解除して実時間で回す
    { L"--font-embedded", CliValue::None, [](CliArgs& a) { a.c.fontEmbedded = true; return true; } },
    { L"--shot-realtime", CliValue::None, [](CliArgs& a) { a.c.shotRealtime = true; return true; } },

    // ---- リプレイ / ハッシュ ----
    { L"--replay-record", CliValue::One,
      [](CliArgs& a) {
          a.c.replayRecordPath = a.v1;
          a.c.vsync = false;
          return true;
      } },
    { L"--replay-verify", CliValue::One,
      [](CliArgs& a) {
          a.c.replayVerifyPath = a.v1;
          a.c.vsync = false;
          return true;
      } },
    { L"--replay-ticks", CliValue::One, [](CliArgs& a) { a.c.replayTicks = _wtoi64(a.v1); return true; } },
    // バッチ記録の早回し (replay_verify.bat 用)。手動記録には渡さないこと —
    // ライブ入力は 1 フレーム 1 回しか採らないので複数 tick が同じ値を食う
    { L"--replay-fast", CliValue::None, [](CliArgs& a) { a.c.replayFast = true; return true; } },
    // M52d: .rep へ記録開始時点の sim 状態を埋め込む (シーン非依存の再生)
    { L"--rep-snapshot", CliValue::None, [](CliArgs& a) { a.c.replayEmbedSnapshot = true; return true; } },
    // M52a: フィールド単位ダンプの出力先と tick
    { L"--hash-dump", CliValue::One, [](CliArgs& a) { a.c.hashDumpPath = a.v1; return true; } },
    { L"--hash-dump-tick", CliValue::One, [](CliArgs& a) { a.c.hashDumpTick = _wtoi64(a.v1); return true; } },
    // M52d: N tick ごとにスナップショット往復を挟む (期待ハッシュ不変が合格条件)
    { L"--snapshot-stress", CliValue::One, [](CliArgs& a) { a.c.snapshotStress = _wtoi64(a.v1); return true; } },
    // M52e: 巻き戻し + 再シムのハッシュ照合 (tick 数は省略可、既定 400)
    { L"--timetravel-selftest", CliValue::Optional,
      [](CliArgs& a) {
          a.c.timeTravelProbeTicks = (a.v1 != nullptr) ? _wtoi64(a.v1) : 400;
          a.c.vsync = false;
          return true;
      } },
    // M72b: 分岐 (What-if) の自動プローブ (tick 数は省略可、既定 400)。合成入力で回す
    { L"--whatif-selftest", CliValue::Optional,
      [](CliArgs& a) {
          a.c.whatIfProbeTicks = (a.v1 != nullptr) ? _wtoi64(a.v1) : 400;
          a.c.synthInput = true;
          a.c.vsync = false;
          return true;
      } },
    // M52h: .rep 2 本の突き合わせ / M52a: ダンプ 2 本の突き合わせ (どちらも Main が実行して終了する)
    { L"--rep-diff", CliValue::Two,
      [](CliArgs& a) {
          a.x.repDiffA = a.v1;
          a.x.repDiffB = a.v2;
          return true;
      } },
    { L"--hash-diff", CliValue::Two,
      [](CliArgs& a) {
          a.x.hashDiffA = a.v1;
          a.x.hashDiffB = a.v2;
          return true;
      } },

    // ---- クラッシュバンドル (M52f) ----
    // 意図的に落としてクラッシュバンドルを検証する。★綴り違いは Main が ParseCrashTestKind で弾く
    { L"--crash-test", CliValue::One,
      [](CliArgs& a) {
          a.x.crashTestArg = a.v1;
          a.c.vsync = false;
          return true;
      } },
    { L"--crash-at-tick", CliValue::One, [](CliArgs& a) { a.c.crashTestTick = _wtoi64(a.v1); return true; } },
    // 既定 on を外す (デバッガ下での切り分け用)
    { L"--no-crash-handler", CliValue::None, [](CliArgs& a) { a.c.crashHandler = false; return true; } },
    { L"--crash-hash-interval", CliValue::One,
      [](CliArgs& a) {
          a.c.crashHashInterval = _wtoi64(a.v1);
          return true;
      } },

    // ---- ネット対戦 (M52h / M52i) ----
    // ホストとして待受 (ポート省略時は 7777)。参加側は --net-join HOST:PORT
    { L"--net-host", CliValue::Optional,
      [](CliArgs& a) {
          a.c.netRole = 1;
          if (a.v1 != nullptr) {
              a.c.netPort = _wtoi(a.v1);
          }
          return true;
      } },
    { L"--net-join", CliValue::One,
      [](CliArgs& a) {
          a.c.netRole = 2;
          a.c.netJoinTarget = a.v1;
          return true;
      } },
    { L"--net-players", CliValue::One, [](CliArgs& a) { a.c.netPlayers = _wtoi(a.v1); return true; } },
    // 入力遅延 (tick)。**全 peer で一致必須** — 違うとハンドシェイクで弾かれる
    { L"--net-delay", CliValue::One, [](CliArgs& a) { a.c.netInputDelay = _wtoi(a.v1); return true; } },
    // 入力パケットを故意に捨てる (検証用)
    { L"--net-loss", CliValue::One, [](CliArgs& a) { a.c.netLossPercent = _wtoi(a.v1); return true; } },
    // M52i: 予測ロールバックを切って M52h の素の遅延ロックステップへ落とす
    { L"--net-no-rollback", CliValue::None, [](CliArgs& a) { a.c.netRollback = false; return true; } },
    // 検出しても止めずに走り続ける (観察用)
    { L"--net-no-halt-on-desync", CliValue::None, [](CliArgs& a) { a.c.netHaltOnDesync = false; return true; } },
    // M52i: 片側にだけ渡して意図的に desync を起こす (検出器の実地検証)
    { L"--net-poke-tick", CliValue::One, [](CliArgs& a) { a.c.netPokeTick = _wtoi64(a.v1); return true; } },

    // ---- 入力レーン (M52g / M75f) ----
    { L"--local-players", CliValue::One, [](CliArgs& a) { a.c.localPlayers = _wtoi(a.v1); return true; } },
    { L"--synth-input", CliValue::None, [](CliArgs& a) { a.c.synthInput = true; return true; } },
    // --ui-demo を押す入力台本 (replay 8 ペア目の記録側)
    { L"--ui-demo-input", CliValue::None, [](CliArgs& a) { a.c.uiDemoInput = true; return true; } },

    // ---- 描画 / ポストプロセス ----
    { L"--postfx-mode", CliValue::One, [](CliArgs& a) { a.c.postFxTonemap = _wtoi(a.v1); return true; } },
    { L"--no-postfx", CliValue::None, [](CliArgs& a) { a.c.postFx = false; return true; } },
    // M45: XAudio2 を初期化しない (端末の無い CI / 撮影専用実行)
    { L"--no-audio", CliValue::None, [](CliArgs& a) { a.c.audio = false; return true; } },
    // M52b: ソフトウェアラスタライザ固定 (CI / 撮影再現)
    { L"--warp", CliValue::None, [](CliArgs& a) { a.c.forceWarp = true; return true; } },
    { L"--exposure", CliValue::One,
      [](CliArgs& a) {
          a.c.postFxExposure = static_cast<float>(_wtof(a.v1));
          return true;
      } },
    { L"--no-bloom", CliValue::None, [](CliArgs& a) { a.c.postFxBloom = false; return true; } },
    { L"--bloom-threshold", CliValue::One,
      [](CliArgs& a) {
          a.c.postFxBloomThreshold = static_cast<float>(_wtof(a.v1));
          return true;
      } },
    { L"--bloom-intensity", CliValue::One,
      [](CliArgs& a) {
          a.c.postFxBloomIntensity = static_cast<float>(_wtof(a.v1));
          return true;
      } },
    { L"--no-fxaa", CliValue::None, [](CliArgs& a) { a.c.postFxFxaa = false; return true; } },
    // M55d: TAA + カメラジッタ (Deferred のみ)
    { L"--taa", CliValue::None, [](CliArgs& a) { a.c.postFxTaa = true; return true; } },
    // M55e: モーションブラーの強度 (0..1)。SceneView は強制 0 なので効くのは GameView / Runtime の描画だけ
    { L"--motion-blur", CliValue::One,
      [](CliArgs& a) {
          a.c.postFxMotionBlur = static_cast<float>(_wtof(a.v1));
          return true;
      } },
    // M25 / M51a / M51b: 並列を直列化 / sim 索引を素通し / クックを使わず毎回フルパース (切り分け用)
    { L"--no-jobs", CliValue::None, [](CliArgs& a) { a.c.useJobs = false; return true; } },
    { L"--no-sim-cache", CliValue::None, [](CliArgs& a) { a.c.useSimCache = false; return true; } },
    { L"--no-cook-cache", CliValue::None, [](CliArgs& a) { a.c.useCookCache = false; return true; } },
    // M46b / M55c / M56c: デバッグ表示 (Deferred のみ)
    { L"--rt-debug", CliValue::One, [](CliArgs& a) { a.c.rtDebugMode = _wtoi(a.v1); return true; } },
    { L"--velocity-debug", CliValue::None, [](CliArgs& a) { a.c.velocityDebug = 1; return true; } },
    { L"--hzb-debug", CliValue::One, [](CliArgs& a) { a.c.hzbDebug = _wtoi(a.v1); return true; } },
    // M56d: SSR (Deferred のみ。HZB も一緒に組まれる)
    { L"--ssr", CliValue::None, [](CliArgs& a) { a.c.ssr = true; return true; } },
    // M56e: 反射プローブを 1 回だけ焼く (値は "X,Y,Z")。**明示指示専用** — 自動ベイクの口はどこにも無い
    // (撮影ごとに焼き上がりが変わると決定的撮影が壊れるため)。読めない値なら既定の位置で焼く
    { L"--probe-bake", CliValue::One,
      [](CliArgs& a) {
          a.c.probeBake = true;
          float px = 0.0f, py = 0.0f, pz = 0.0f;
          if (swscanf_s(a.v1, L"%f,%f,%f", &px, &py, &pz) == 3) {
              a.c.probeBakePos[0] = px;
              a.c.probeBakePos[1] = py;
              a.c.probeBakePos[2] = pz;
          }
          return true;
      } },
    // M56f: シーン中の ReflectionProbeComponent を全部焼く。
    // ★撮影に映すならベイクのフレームを --shot-frame より前に置くこと (ベイクはスクショ保存の後に走る)
    { L"--probe-bake-all", CliValue::None, [](CliArgs& a) { a.c.probeBakeAll = true; return true; } },
    { L"--probe-bake-frame", CliValue::One, [](CliArgs& a) { a.c.probeBakeFrame = _wtoi(a.v1); return true; } },
    { L"--probe-bake-png", CliValue::One, [](CliArgs& a) { a.c.probeBakePng = a.v1; return true; } },

    // ---- レイトレ (M46 / M67) ----
    { L"--rt-no-temporal", CliValue::None, [](CliArgs& a) { a.c.rtTemporal = false; return true; } },
    { L"--rt-freeze-seed", CliValue::None, [](CliArgs& a) { a.c.rtFreezeSeed = true; return true; } },
    { L"--rt-anim-seed", CliValue::None, [](CliArgs& a) { a.c.rtAnimSeed = true; return true; } },
    { L"--rt-no-svgf", CliValue::None, [](CliArgs& a) { a.c.rtSvgf = false; return true; } },
    { L"--rt-gi", CliValue::None, [](CliArgs& a) { a.c.rtGi = true; return true; } },
    { L"--rt-shadow", CliValue::None, [](CliArgs& a) { a.c.rtShadow = true; return true; } },
    { L"--rt-refl", CliValue::None, [](CliArgs& a) { a.c.rtRefl = true; return true; } },
    // M67d: 反射のサンプルを reservoir で時空間再利用する (--rt-refl と併用)
    { L"--rt-restir", CliValue::None, [](CliArgs& a) { a.c.rtRestir = true; return true; } },
    // M67f: 空間再利用の on / 明示 off。どちらも本体 (--rt-restir) を一緒に立てる
    { L"--rt-restir-spatial", CliValue::None,
      [](CliArgs& a) {
          a.c.rtRestirSpatial = true;
          a.c.rtRestir = true;
          return true;
      } },
    { L"--rt-restir-no-spatial", CliValue::None,
      [](CliArgs& a) {
          a.c.rtRestirSpatial = false;
          a.c.rtRestir = true;
          return true;
      } },
    // M67f: 候補ごとに可視レイ (光漏れを消す)。**タップの中でしか撃たない**ので空間再利用も一緒に立てる
    { L"--rt-restir-visray", CliValue::None,
      [](CliArgs& a) {
          a.c.rtRestirVisRay = true;
          a.c.rtRestirSpatial = true;
          a.c.rtRestir = true;
          return true;
      } },
    // M67f: 全インスタンスの ReflectionClass を強制 (-1 = off)。ReSTIR とは独立なので --rt-restir は立てない
    { L"--rt-class-override", CliValue::One,
      [](CliArgs& a) {
          a.c.rtClassOverride = _wtoi(a.v1);
          return true;
      } },

    // ---- フロクセル (M57) / 音響 (M65 / M68) ----
    { L"--froxel", CliValue::None, [](CliArgs& a) { a.c.froxel = true; return true; } },
    // M57c: 深度スライスジッタと履歴の混合を止める (A/B 用)。--froxel も一緒に立てる
    { L"--froxel-no-temporal", CliValue::None,
      [](CliArgs& a) {
          a.c.froxelTemporal = false;
          a.c.froxel = true;
          return true;
      } },
    // M57b/M57c: N 回目の描画でグリッドを読み戻して統計と検査をログへ。--froxel も一緒に立てる
    { L"--froxel-dump", CliValue::One,
      [](CliArgs& a) {
          a.c.froxelDumpFrame = _wtoi(a.v1);
          a.c.froxel = true;
          return true;
      } },
    // M65d: N 回目の描画で残光ボリュームを読み戻し、CPU 側の配列とバイト単位で突き合わせる (他のフラグは立てない)
    { L"--acoustic-dump", CliValue::One, [](CliArgs& a) { a.c.acousticDumpFrame = _wtoi(a.v1); return true; } },
    // 2026-09-12: 解析的な波面 (円) を止めて残光だけの絵にする (A/B 用)
    { L"--no-acoustic-front", CliValue::None, [](CliArgs& a) { a.c.acousticFront = false; return true; } },
    // M68a: tick < N のあいだ整形の結果を 1 行ずつ標準出力へ + 終了時に summary。
    // ★--no-audio と併用すると 1 行も出ない (設計どおり = ヘッドレスはゼロコスト)
    { L"--acoustic-audio-log", CliValue::One,
      [](CliArgs& a) {
          a.c.acousticAudioLogTicks = _wtoi(a.v1);
          return true;
      } },

    // ---- パーティクル (M57追補) ----
    // バックエンドを CLI から固定する (project_settings.json より優先。ただし書き戻さない)。
    // GPU 粒子を --screenshot で撮る唯一の口。**shot_verify は Runtime.exe で撮る**ので両 Main に要る
    { L"--particle-backend", CliValue::One,
      [](CliArgs& a) {
          const std::wstring backend = a.v1;
          if (backend == L"gpu") {
              a.c.particleBackendOverride = 1;
          } else if (backend == L"cpu") {
              a.c.particleBackendOverride = 0;
          } else {
              // ★綴り違いを黙って無視しない — 黙って cpu で撮ると
              //   「GPU の絵のつもりの golden」が CPU の絵になり、以後ずっと嘘をつく
              std::fwprintf(stderr, L"unknown --particle-backend value (expected cpu|gpu)\n");
              return false;
          }
          return true;
      } },
    // CPU/GPU を横に並べて描く (spec 7.4)
    { L"--particle-compare", CliValue::None, [](CliArgs& a) { a.c.particleCompareOverride = 1; return true; } },

    // ---- Deep-Modal (M76e) ----
    // 推論バックエンドを固定する。"d3d11cs" は綴りとしては受け付けるが未実装 —
    // 実際に cpu へ縮退させて WARN を出すのは ModalSoundLibrary::SetBackendByName の役目
    { L"--modal-backend", CliValue::One,
      [](CliArgs& a) {
          const std::wstring backend = a.v1;
          if (backend == L"cpu" || backend == L"d3d11cs") {
              a.c.modalBackendName = backend;
              return true;
          }
          std::fwprintf(stderr, L"unknown --modal-backend value (expected cpu|d3d11cs)\n");
          return false;
      } },
};

} // namespace

CliParse ParseEngineCliFlag(int argc, wchar_t** argv, int& i, EngineConfig& config, EngineCliExtras& extras)
{
    const std::wstring arg = argv[i];
    for (const CliFlag& flag : kEngineCliFlags) {
        if (arg != flag.name) {
            continue;
        }
        CliArgs a{ config, extras, nullptr, nullptr };
        switch (flag.value) {
        case CliValue::None:
            break;
        case CliValue::One:
            if (i + 1 >= argc) {
                return CliParse::NotMine;
            }
            a.v1 = argv[++i];
            break;
        case CliValue::Two:
            if (i + 2 >= argc) {
                return CliParse::NotMine;
            }
            a.v1 = argv[++i];
            a.v2 = argv[++i];
            break;
        case CliValue::Optional:
            if (i + 1 < argc && argv[i + 1][0] != L'-') {
                a.v1 = argv[++i];
            }
            break;
        }
        return flag.apply(a) ? CliParse::Consumed : CliParse::Error;
    }
    return CliParse::NotMine;
}

} // namespace mye
