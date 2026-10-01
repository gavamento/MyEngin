//====================================================================================
//                          HeadlessSim.h
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          GPU・窓・オーディオ無しで RunOneTick を回す sim ホスト
//====================================================================================
#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "Engine/Engine/Demo/StartScene.h"
#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Engine/Session/SessionTypes.h"

namespace mye {

struct HeadlessSimSetup {
    // 共有 CLI (ParseEngineCliFlag) で埋めた設定。sim に効くのは projectRoot / useJobs /
    // useSimCache / localPlayers / replayVerifyPath など。クックキャッシュは常に切る (下の注記)
    EngineConfig config;
    StartSceneOptions scene;
    // true: サーバ / クライアントのセッションとして動かす。RunTick へ渡すシステム入力
    // (参加・離脱) を tick ごとに適用し、SessionLanes を sim 状態に持つ (ctx.hasSystemInput)
    bool systemInput = false;
};

struct HeadlessVerifyResult {
    bool ran = false;               // .rep を読めて tick ループまで到達した
    bool passed = false;            // 全 tick 一致 (期待値なしの tick は数えない)
    uint64_t verifiedTicks = 0;
    uint64_t unverifiedTicks = 0;   // 期待ハッシュが 0 の tick (クラッシュ .rep の未完了 tick)
    uint64_t totalTicks = 0;        // .rep に入っている tick 数
    uint64_t firstMismatchTick = 0; // passed == false かつ ran のときだけ意味を持つ
    double elapsedMs = 0.0;         // tick ループの実時間 (ログ用。sim には入らない)
};

// GPU デバイス・窓・オーディオデバイス・ImGui を作らずに、EngineLoop と同じ RunOneTick で
// sim を進めるホスト。EngineLoop::Run の GPU 系を除いた起動手順は SimInit.h の関数群を共有し、
// tick の中身は TickRunner.cpp の RunOneTick そのもの (sim の経路を増やさない)。
//
// 状態はすべてインスタンスが持つので、1 プロセスに複数立てられる。ただし次のプロセス全体の
// 共有物は 1 つしか持てない: ライブラリ注入 (meshcol:: 等)、ジョブシステム、AssetDatabase のキー解決、
// ScriptHost が掴む GameLogic.dll の API 文脈。複数立てるときは tick を回す直前に Activate() を呼ぶこと
// (注入先を自分の実体へ付け替える)。
//
// ★クックキャッシュは使わない (useCookCache を強制 false)。device 無しでクックすると
//   texture=0 の材質を .mmdl へ書き、Editor / Runtime と共有の cache\cooked を汚すため
class HeadlessSim {
public:
    HeadlessSim();
    ~HeadlessSim();
    HeadlessSim(const HeadlessSim&) = delete;
    HeadlessSim& operator=(const HeadlessSim&) = delete;

    // sim 側の初期化 → 起動シーンの構築 → 構造変更の確定まで。失敗は ERROR ログ付きで false
    bool Init(const HeadlessSimSetup& setup);

    // プロセス共有の注入先を自分の実体へ付け替える (複数インスタンスを交互に回すとき)
    void Activate();

    // setup.config.replayVerifyPath の .rep を読み込み、全 tick を照合する。
    // 不一致なら <rep>.tickN.actual.dump と <rep>.mismatch.txt を残して打ち切る (TickRunner の経路)
    HeadlessVerifyResult VerifyReplay();

    // ---- 確定入力で 1 tick ずつ進める (サーバ / クライアントの確定 tick と再シム) ----
    // lanes (PlayerCount() 本) と sys を ctx へ置換して RunOneTick を 1 回回し、tick 末のワールドハッシュを
    // 返す。置換は EngineLoop の verify と同じ ApplyConfirmedInputs。resim = ロールバックの再シム
    // (TickServices::resim)。複数インスタンスを交互に回すので、内部で Activate() する
    uint64_t RunTick(const InputSnapshot* lanes, const SystemInputTick* sys, bool resim);
    // いまの sim 状態のワールドハッシュ (tick 末ハッシュと同じ束)
    uint64_t WorldHash();
    // スナップショットの撮影 / 復元に使う参照束 (NetRollback / RestoreSimSnapshot 用)
    const SimRefs& Refs() const;
    uint32_t PlayerCount() const;
    // --net-poke-tick と同じ意図的な状態破壊 (desync 検出の検証用。-1 = 無効)
    void SetPokeTick(int64_t tick);

    uint64_t TickIndex() const;
    const std::wstring& AssetsRoot() const;
    // 自己検査用: Server が使うシャドウコピー先 (Editor / Runtime の cache\hot とは別)
    const std::wstring& ShadowCopyDir() const;
    // 起動時に算出した出自 (engine / game / content ...)。Init 後に有効
    const SimProvenance& Provenance() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mye
