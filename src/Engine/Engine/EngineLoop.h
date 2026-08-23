#pragma once
#include <cstdint>
#include <string>

#include "Engine/Platform/Input.h"

namespace mye {

class Win32Window;
class GraphicsDevice;
class SwapChain;
class Scene;
class ShaderManager;
class RenderSystem;
class UIRenderer;
class VfxRenderer;
class IRenderPath;
class ReloadHub;
class ScriptHost;
class DllReloader;
class ManagedHost;
class ParticleSystem;
class PrefabLibrary;
class AnimationLibrary;
class ControllerLibrary;
class AssetDatabase;
class AudioSystem;
class SoundLibrary;
class MixerLibrary;
class InputActions;
class TimeTravel;
struct RenderResources;
struct NetRuntimeInfo;

struct EngineConfig {
    std::wstring title = L"MyEngine";
    int width = 1600;
    int height = 900;
    int64_t maxFrames = -1;  // >0 でそのフレーム数後に自動終了 (スモークテスト / CI 用)
    bool enableImGui = true; // false = エディタ UI 無し (将来の Runtime.exe 用の余地)
    bool vsync = true;
    float clearColor[4] = { 0.08f, 0.09f, 0.11f, 1.0f };
    std::wstring screenshotPath; // 空でなければ screenshotFrame で PNG 保存 (検証用)
    int64_t screenshotFrame = 60;
    int64_t screenshotEvery = 0; // >0 で N フレーム毎に連番保存 (ライブ検証用)
    // ---- 決定的スクショ (M52c) ----
    // --screenshot 指定 (連番の --shot-every を除く) で**自動 on**。フレームの dt を実時間
    // ではなく固定 tick 幅に固定し (= frame 番号がそのまま tick 番号)、非同期テクスチャの
    // デコードを撮影前に待ち切る。これが無いと「1 フレームに何 tick 回ったか」と
    // 「テクスチャが間に合ったか」が実時間で変わり、同条件 2 回の PNG が一致しない
    // (M52c で実測)。--shot-realtime で従来の実時間駆動へ戻せる
    bool shotRealtime = false;
    // --font-embedded: UI のフォントアトラスを内蔵 8x8 (ASCII) に固定する。
    // golden PNG を「そのマシンに入っている TTF」から切り離すためのもので、
    // 英語版 Windows Server の CI ランナーには日本語 TTF が無い = 探索させると別の絵になる
    bool fontEmbedded = false;
    // true: シーンをバックバッファへ直接描画 (Runtime / M1 デモ)。
    // false: 描画は app の OnRenderViews に委ねる (エディタは SceneView/GameView の RT へ描く)
    bool renderSceneToBackbuffer = true;

    // ---- ポストプロセス (M16) ----
    bool postFx = true;         // false で HDR 配管をバイパス (従来の直描き)
    int postFxTonemap = 1;      // 0=passthrough(配管検証) 1=ACES 2=Reinhard
    float postFxExposure = 1.0f;
    bool postFxBloom = true;
    float postFxBloomThreshold = 1.0f;
    float postFxBloomIntensity = 0.6f;
    bool postFxFxaa = true;
    // M55e: モーションブラー強度のグローバル既定 (--motion-blur N)。0 = off。
    // シーンカメラに CameraPostFx があればそちらが勝ち、SceneView は常に強制 0。
    // これが無いと「既定 off の機能を撮影して確かめる」手段が Inspector 操作しかない
    float postFxMotionBlur = 0.0f;

    // ---- ジョブシステム (M25) ----
    bool useJobs = true; // false で全並列を直列化 (決定論ゲート / 計測比較用)

    // ---- sim 索引 (M51a) ----
    // false で World クエリキャッシュ / Scene fileId 索引を素通しして線形経路に落とす
    // (決定論ゲート / 障害切り分け用)。結果はキャッシュ有無でビット同一
    bool useSimCache = true;

    // ---- アセットクックキャッシュ (M51b) ----
    // false で cache\cooked\ を読み書きせず毎回フルパース (障害切り分け / A-B 計測用)。
    // 登録される内容はクック有無でビット同一 (CookedCacheSelfTest + replay_verify が保証)
    bool useCookCache = true;

    // ---- レイトレのデバッグ表示 (M46b、--rt-debug N) ----
    // 0=off 1=BVH ヒートマップ 2=ヒット法線 3=インスタンス ID
    // 4=生 GI (1spp) 5=蓄積 GI 6=履歴長 (M46c/M46d) 7=SVGF 後 8=推定分散 (M46e)。
    // Deferred パスのみ効く。
    // 終了時に BVH の規模とトラバーサルの GPU 時間をログに出す (性能実測用)
    int rtDebugMode = 0;
    // M55c: velocity バッファ (GBuffer RT4) の可視化 (--velocity-debug)。0=off。
    // Deferred パスのみ効く。TAA / モーションブラー v2 が入るまでの唯一の目視口
    int velocityDebug = 0;
    // M56c: HZB (min-Z ピラミッド) の可視化 (--hzb-debug N)。0=off / N=ミップ N-1 を表示。
    // Deferred パスのみ効く。**0 のときはピラミッドを組みもしない** = 従来と 1 命令も違わない。
    // SSR (M56d) が入るまでは、これが「本当に段が積めているか」の唯一の目視口になる
    int hzbDebug = 0;
    // M56d: SSR (--ssr)。**Deferred のみ** (GBuffer と HZB が前提)。
    // シーンカメラに CameraPostFx があればそちらの ssrOn が勝つ (TAA と同じ規則)。
    // on にすると HZB (min-Z ピラミッド) も一緒に組まれる
    bool ssr = false;
    // M55d: TAA (--taa)。**Deferred のみ** (画面速度が GBuffer RT4 にしかない)。
    // シーンカメラに CameraPostFx があればそちらの taaOn が勝つ (ポスプロ設定と同じ規則)
    bool postFxTaa = false;
    // M46d: テンポラル蓄積 (--rt-no-temporal で off = 1spp 生のまま。A/B 計測用)。
    // 乱数列の freeze は既定でスクショ/リプレイ時に自動 on (M46c)。
    // --rt-freeze-seed で常時 on、--rt-anim-seed でその自動 on を解除する
    // (蓄積のデノイズ効果をスクリーンショットに写すには後者が要る)
    bool rtTemporal = true;
    bool rtFreezeSeed = false;
    bool rtAnimSeed = false;
    // M46e: SVGF 空間フィルタ (--rt-no-svgf で off)。rtTemporal=false では元から動かない
    bool rtSvgf = true;
    // M46f: レイトレ GI を最終画像へ合成する (--rt-gi)。Deferred パスのみ。
    // off なら BVH の構築も転送も走らないので既定の描画経路は一切変わらない
    bool rtGi = false;
    // M46g: 平行光の影をレイトレで作る (--rt-shadow)。Deferred パスのみ。同上
    bool rtShadow = false;
    // M46h: スペキュラ環境項をレイトレ反射で置き換える (--rt-refl)。Deferred パスのみ。同上
    bool rtRefl = false;

    // ---- グラフィックスドライバ (M52b) ----
    // true (--warp) で D3D_DRIVER_TYPE_WARP (ソフトウェアラスタライザ) を直接使う。
    // false でも HARDWARE の生成に失敗すれば WARP へ自動フォールバックするので、
    // GPU の無い CI runner でも selftest / replay_verify がそのまま回る。
    // sim は CPU 専用 = リプレイのハッシュは採用ドライバに依らない。
    // golden スクリーンショットは撮影機と CI を一致させるため --warp 固定で撮る
    bool forceWarp = false;

    // ---- オーディオ (M45) ----
    // false (--no-audio) で XAudio2 を一切初期化しない。オーディオ端末の無い CI や
    // スクリーンショット専用実行で、デバイス確保とストリーミングスレッドを避けるため
    bool audio = true;

    // ---- プロジェクト (M26) ----
    // 空 = 従来動作 (FindAssetsRoot で単一リポジトリレイアウトを探索)。
    // 非空 = <projectRoot>\assets をアセットルートにする (--project で注入)
    std::wstring projectRoot;

    // ---- リプレイ一貫性検証 (engine_spec.md 11.3) ----
    std::wstring replayRecordPath; // 空でなければ記録モード (replayTicks 分記録して終了)
    std::wstring replayVerifyPath; // 空でなければ検証モード (全 tick 照合、exit code 0/1)
    int64_t replayTicks = 600;     // 記録する tick 数 (60Hz で 10 秒)
    // M52d: --rep-snapshot で「記録開始時点の sim 状態」を .rep の先頭へ埋め込む。
    // 埋め込みがあれば検証側は**シーンの中身に依存せず**その状態から再生できる
    // (配布ビルドのクラッシュ .rep が本命 = M52f)。既定 off なので .rep のサイズは従来どおり
    bool replayEmbedSnapshot = false;

    // ---- ハッシュ差分診断 (M52a) ----
    // 空でなければ hashDumpTick の tick 末 (= ハッシュを撮るのと同じ点) で
    // フィールド単位ダンプを書き出す。記録/検証/通常再生のどのモードでも効く。
    // 検証中の MISMATCH では指定の有無に関わらず自動で
    // <rep>.tick<N>.actual.dump + <rep>.mismatch.txt (tick 番号) を吐く
    std::wstring hashDumpPath;
    int64_t hashDumpTick = 0;

    // ---- スナップショット往復ストレス (M52d) ----
    // >0 で N tick ごとに「撮って戻して撮り直す」を tick 境界へ挟む。sim の意味論は
    // 一切変わらないので**期待ハッシュは全一致するはず** — 復元の非対称性 (撮れるが
    // 戻らない項目) を 600 tick の実データで炙り出すための道具で、selftest の小さな
    // 世界では出ない取りこぼしはここでしか捕まらない。tools\replay_verify.bat が
    // 3 ペアすべてに掛ける
    int64_t snapshotStress = 0;

    // ---- タイムトラベルの自動プローブ (M52e、--timetravel-selftest [N]) ----
    // >0 で「N tick 進める → N-K へシーク → 記録入力で N まで再シム → ハッシュが
    // 元の N と一致するか」を複数の K で実走し、結果を exit code で返す。
    // ★これがエディタ GUI を開かずにタイムトラベルを検証できる唯一の口。
    // Editor では --autoplay と併用しないと sim が進まない (両 Main が自動で立てる)
    int64_t timeTravelProbeTicks = 0;

    // ---- マルチプレイヤー入力レーン (M52g) ----
    // --local-players N (1..kMaxPlayers)。sim が消費する入力レーンの本数で、
    // .rep の playerCount にそのまま入る。**検証中は .rep 側の値が優先**
    // (レコード長がファイルで決まっているので、指定と食い違ったら .rep に従う)。
    // レーン n はキーボードではなく XInput スロット n を見る (Input.h のレーン規約)
    int localPlayers = 1;
    // --synth-input: レーンごとに違う合成入力を tick へ流し込む (SynthLaneInput)。
    // ライブ入力の代わりに置くだけなので **記録もされ検証でも再現する** =
    // 「合成入力で録った .rep」は普通の .rep と同じ扱いで verify できる。
    // ★これが無いとヘッドレスの入力は全レーン恒常ゼロで、レーンの配線ミスが
    //   記録側と検証側で対称に起きてハッシュ一致してしまう (Input.h の SynthLaneInput 参照)
    bool synthInput = false;

    // ---- ネット対戦: UDP + 遅延ロックステップ (M52h、決定台帳 5) ----
    // NetRole の生値 (0=off / 1=host / 2=join)。**Engine/Net の型をここへ持ち込まない**
    // ために int で持つ (crashTest と同じ流儀 — EngineLoop.h はほぼ全 TU が読む)
    int netRole = 0;
    int netPort = 7777;            // --net-host PORT
    std::wstring netJoinTarget;    // --net-join HOST:PORT
    int netPlayers = 2;            // --net-players N (M52 は 2 人 P2P 固定)
    int netInputDelay = 3;         // --net-delay N (tick)。全 peer で一致必須
    int netLossPercent = 0;        // --net-loss N (入力パケットを故意に捨てる。検証用)

    // ---- 予測ロールバック + desync 検出 (M52i) ----
    // 既定 on。未着レーンを予測 (直近の確定値の繰り返し) で埋めて先へ進み、外れたら
    // 巻き戻して確定入力で再シムする。--net-no-rollback で M52h の素の遅延ロックステップ
    // (そろうまで止まる) へ落とせる — 「ロールバックが原因か」を切り分けるための口
    bool netRollback = true;
    // 確定 tick のワールドハッシュを交換し、割れたら crash\desync_<tick>\ を吐いて停止する。
    // --net-no-halt-on-desync で「検出して警告するが走り続ける」へ落とせる (観察用)。
    // ★止めるのが既定なのは、desync 後の世界は 2 台で別物であって「遊べているように
    //   見えるだけ」だから。黙って続けるのは一番たちが悪い
    bool netHaltOnDesync = true;
    // --net-poke-tick N: **意図的に desync を起こす**。tick N の末に sim 状態を 1 フィールド
    // だけ壊す (ハッシュ順で最初のエンティティの位置 x に +0.001)。検出器と診断チェーン
    // (--rep-diff → --hash-diff) が本当に働くかは、実際に壊して確かめるしかない
    // (--crash-test と同じ流儀)。ネット非依存で効くので **.rep の再生でも再現できる**
    int64_t netPokeTick = -1;

    // ---- クラッシュバンドル (M52f) ----
    // 落ちたら <projectRoot|exeDir>\crash\<timestamp>\ に minidump + crash.rep +
    // crash.txt + scene.json を残す。既定 on (配布ビルドのバグ報告が本命なので、
    // 「調子が悪いときだけ有効にする」形にはしない)。--no-crash-handler で外せる
    bool crashHandler = true;
    // --crash-test <av|purecall|terminate|invalidparam|stackoverflow>: crashTestTick の
    // tick 本体へ入る直前に意図的に落とす。ハンドラが「本当に落ちたときに動くか」は
    // 実際に落として確かめるしかない (M52a 申し送り 5 と同じ流儀)
    int crashTest = 0;            // CrashTestKind の生値 (Platform への依存を持ち込まない)
    int64_t crashTestTick = 120;  // --crash-at-tick N
};

// フレーム計測 (Profiler ウィンドウ表示用)。EngineLoop が毎フレーム更新する
struct FrameTimings {
    float frameMs = 0.0f;   // フレーム全体
    float reloadMs = 0.0f;  // フェーズ 2 (ホットリロード)
    float tickMs = 0.0f;    // 全 tick (フェーズ 3-5,7)
    float renderMs = 0.0f;  // フェーズ 6 (シーン描画 + OnRenderViews)
    float presentMs = 0.0f; // フェーズ 8 (ImGui + Present)
    int ticksThisFrame = 0;
};

// アプリ側 (Editor / Runtime) がサブシステムへアクセスするための窓口
struct EngineContext {
    Win32Window* window = nullptr;
    GraphicsDevice* device = nullptr;
    SwapChain* swapChain = nullptr;
    Scene* scene = nullptr;             // アクティブシーン (EngineLoop が所有)
    ShaderManager* shaders = nullptr;
    RenderResources* resources = nullptr;
    RenderSystem* renderSystem = nullptr;
    UIRenderer* uiRenderer = nullptr;          // ゲーム内 UI (M21)。GameView / Runtime が重ね描画
    VfxRenderer* vfx = nullptr;                // Sprite/Trail/TextMesh (M29c)。RenderSystem に渡す
    IRenderPath* renderPath = nullptr;         // 現在アクティブなパス (書き換えると切替)
    IRenderPath* renderPathForward = nullptr;  // 選択肢: Forward
    IRenderPath* renderPathDeferred = nullptr; // 選択肢: Deferred
    ReloadHub* reloadHub = nullptr;
    ScriptHost* scriptHost = nullptr;
    DllReloader* dllReloader = nullptr;
    ManagedHost* managedHost = nullptr; // C# スクリプトホスト (CoreCLR)。未導入時は null/未 ready
    ParticleSystem* particles = nullptr;
    PrefabLibrary* prefabs = nullptr;   // 登録済みプレハブ (.prefab.json) — Editor / ReloadHub が使う
    AnimationLibrary* anims = nullptr;  // 登録済み AnimationClip (.anim.json)
    ControllerLibrary* controllers = nullptr; // 登録済み Animator Controller (.controller.json、M22)
    AssetDatabase* assetDb = nullptr;   // GUID/.meta サイドカー DB (M23)。パス⇄GUID 解決
    // オーディオ (M45)。**決定論レーン外の出力 sink** — sim から再生位置や再生中判定を
    // 読み戻してはいけない (読んだ瞬間にリプレイが壊れる)。エディタの試聴/ミキサー用に公開する
    AudioSystem* audio = nullptr;
    SoundLibrary* sounds = nullptr;     // 登録済みサウンドアセット (.sound.json、M45c)
    MixerLibrary* mixers = nullptr;     // 登録済みミキサー (.mixer.json、M45d)。アクティブは 1 本
    std::wstring assetsRoot;            // assets\ の絶対パス
    std::wstring projectRoot;           // プロジェクトルート (M26)。レガシー起動時は空
    std::wstring imguiIniPath;          // imgui.ini の解決済みパス (レガシー時は L"imgui.ini")
    // 入力レーン (M52g)。現フレームのスナップショット (tick 中も同一)。
    // inputs[0] が従来の単一入力そのもので、Input() はその別名 —
    // 「1 本の入力」を前提にしていた既存コード (エディタの UI ヒットテスト / C++ / C#
    // スクリプトの KeyDown 系) は全部レーン 0 を見続ける。
    // playerCount 以降のレーンは常にゼロ値 (Evaluate が毎 tick 潰す)
    InputSnapshot inputs[kMaxPlayers] = {};
    // 消費するレーン数 (1..kMaxPlayers)。--local-players N か、検証中は .rep の
    // playerCount がそのまま入る。**走行中は変わらない** (途中で変えると .rep の
    // tick レコード長が変わって読めなくなる)
    uint32_t playerCount = 1;
    InputSnapshot& Input() { return inputs[0]; }
    const InputSnapshot& Input() const { return inputs[0]; }
    // アクションマップ (M51d)。EngineLoop が所有し tick 頭に評価済み。
    // assets\input\actions.json が無ければ空マップ (ActionState/AxisValue は常に 0)
    InputActions* inputActions = nullptr;
    // タイムトラベルのリング (M52e)。EngineLoop が所有し、シーク要求は tick 境界で捌く。
    // エディタは「Play で有効化 → タイムライン窓から RequestSeek」しか触らない
    // (Restore と再シムはここでは起きない = 途中の状態を UI に見せない)
    TimeTravel* timeTravel = nullptr;
    // ネットセッションの状態 (M52i)。EngineLoop が毎フレーム 1 回書く読み取り専用の POD。
    // null = このビルド/実行ではネットを張っていない。
    // ★中身はすべて機種依存 (自分がどちら側か / ping / ロールバック回数)。
    //   **sim 状態へ書き戻さないこと** — 詳細は NetRuntime.h
    const NetRuntimeInfo* net = nullptr;
    // この tick でスクリプト層 (フェーズ 3/5) を実行するか。
    // エディタは OnTick で Play 状態に応じて設定する (Runtime は常に true)
    bool simulateScripts = true;
    uint64_t frameIndex = 0;  // 描画フレーム数
    uint64_t tickIndex = 0;   // 累計固定 tick 数 (シミュレーション時間 = tickIndex * fixedDt)
    float fixedDt = 1.0f / 60.0f;
    FrameTimings timings;     // 前フレームの計測値
    bool requestExit = false;
};

class IEngineApp {
public:
    virtual ~IEngineApp() = default;
    virtual void OnStart(EngineContext&) {}
    virtual void OnTick(EngineContext&) {}        // 固定 tick 毎 (spec 5.3 フェーズ 3 スロット)
    virtual void OnRenderViews(EngineContext&) {} // フェーズ 6: 独自 RT への描画 (エディタの SceneView 等)
    virtual void OnImGui(EngineContext&) {}       // 描画フレーム毎 (spec 5.3 フェーズ 8)
    virtual void OnShutdown(EngineContext&) {}
};

// メインループ (engine_spec.md 5.3)。
//
// 決定論のための設計判断 (ADR 候補):
//   シミュレーションは 60Hz 固定 tick で進み、構造変更の適用 (spec フェーズ 7) は
//   「フレーム末」ではなく「tick 末」に行う。1 フレームに複数 tick が走る場合でも
//   tick 列としての挙動が フレームレートに依存しなくなり、リプレイ再現 (spec 11.3) が
//   フレーム分割と無関係に成立する。フレーム構造:
//     1. 時間更新 / 入力スナップショット確定
//     2. ホットリロード適用 (セーフポイント)
//     [tick × N] 3. スクリプト Update → 4. システム → 5. LateUpdate → 7. 構造変更適用
//     6. シーン描画
//     8. ImGui 描画 / Present
class EngineLoop {
public:
    int Run(const EngineConfig& config, IEngineApp& app);
};

} // namespace mye
