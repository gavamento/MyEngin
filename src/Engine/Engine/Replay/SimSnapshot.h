#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Platform/Input.h"

namespace mye {

class Scene;
class CpuParticleBackend;
class XpbdBackend;
class AcousticField;
class CollisionSystem;
class ScriptHost;

// ワールドハッシュに畳む「ECS 外の sim 状態」の束 (WorldHasher の SimSources) を組む**唯一の場所**。
// record / verify / --hash-dump / タイムトラベルの自己検証 / ネットの開始ハッシュが全部ここを通る —
// 呼び出し側で波括弧初期化を手書きすると、項目を足したときに 1 か所だけ古いまま残る
// (M70c: acoustic を 4 か所で渡し忘れ、波の出るシーンでだけ crash .rep が全 tick 割れた)
SimSources SimSourcesOf(Scene& scene, const CpuParticleBackend* particles, const XpbdBackend* xpbd,
                        const AcousticField* acoustic);

// sim レーンのスナップショット (M52d、決定台帳 1)。
// 「ある tick の sim 状態を丸ごと保存し、後でビット同一に復元し、そこから同じ入力で
// 回すと同じハッシュ列になる」ための共通部品。タイムトラベル (M52e) / クラッシュ
// バンドル (M52f) / ロールバック (M52i) の 3 者がこの 1 個に乗る。
//
// **対象は sim レーンだけ** = record/verify がハッシュを撮っている範囲と同一:
//   World (全アーキタイプのカラム生バイト + レコード表 + freeIndices + ルート + RNG)
//   Scene (TimeControl / UI 対話状態 / PersistStore / nextFileId / sourcePath / override 表)
//   CpuParticleBackend の池 / XpbdBackend の池 (M60'b) /
//   CollisionSystem の前 tick ペア / ScriptHost の Start 済み記録
//   EngineLoop の prevTickInput (アクション評価の pressed/released 判定に効く) と
//   audioHandleSeq (再生ハンドルの採番列)
// **対象外**: C# (ManagedHost) レーン / GPU パーティクル / VfxRenderer トレイル /
//   TransformSystem の側テーブル (M51c) / オーディオ。
//   側テーブルは World::SnapshotRead が hierarchyDirty_ を立てることで Rebuild →
//   全無効化に落ちる (= 次 tick は全件再計算 = スキップ経路とビット同値)。
//   **前 3 者 (C# / GPU パーティクル / トレイル) の Reset は呼び出し元の責務**にしてある —
//   「戻した後にどう見せたいか」は消費者ごとに違うため (タイムトラベルは未来のトレイルを
//   消したいが、--snapshot-stress は描画を乱したくない)。M52e で忘れずに呼ぶこと。
//   C# 非対応は record/verify と**同じ境界**であり、新しい制約ではない。
//
// ★撮れるのは「構造変更が空の tick 末」だけ (World::SnapshotWrite の MYE_CHECK)。
struct SimRefs {
    Scene* scene = nullptr;            // 必須 (World の所有者)
    CpuParticleBackend* particles = nullptr;
    // M60'b: XPBD 変形体の池。ハッシュ (WorldHasher の SimSources) と対で撮る —
    // 片方だけだと「リプレイは通るのに巻き戻しで割れる」型のバグになる (3 点セット契約)
    XpbdBackend* xpbd = nullptr;
    // M65a: 音響の場。**復元されるのは波スロット表だけ**で、占有グリッドも距離場も
    // 導出値なので blob に入らない。★RestoreSimSnapshot は復元の最後に
    // AcousticField::Invalidate() を呼ぶ — 呼ばないと「戻した波」と「戻す前に育てた
    // 距離場」が組み合わさり、再シムでだけ結果が変わる (最悪の型のバグ)
    AcousticField* acoustic = nullptr;
    CollisionSystem* collision = nullptr;
    ScriptHost* scripts = nullptr;
    // M52g: **kMaxPlayers 本のレーン配列**の先頭を指す (1 本ではない)。
    // blob には常に kMaxPlayers 本ぶん書く = --local-players の指定に依らず往復できる
    InputSnapshot* prevTickInput = nullptr;
    // M52e: 再生ハンドルの採番カウンタ (EngineApiTable の PlaySound 系が ++ して script へ返す)。
    // ★ハッシュには入らないが**戻り値がスクリプト経由で sim 状態へ入りうる**ので、
    //   複数 tick の再シムを跨ぐと採番がずれて世界が割れる。--snapshot-stress は
    //   同一 tick の往復しか見ないのでこの穴を検出できなかった (M52e で発見)
    uint64_t* audioHandleSeq = nullptr;
    uint64_t* tickIndex = nullptr; // 撮影時に読み、復元時に書き戻す (null なら素通し)

    // この束で撮るワールドハッシュの源 (SimSourcesOf)。scene は非 null が前提
    SimSources HashSources() const { return SimSourcesOf(*scene, particles, xpbd, acoustic); }
};

// blob の形式版。**.rep の版とは独立** (M52a 申し送り 7 と同じ規約) —
// blob のレイアウトを変えたらここだけを上げる。描画レーン専用の値や NoHash のコンポーネントでも
// 生バイトは World 節に載るので版は上がる。版は一致しか見ないので番号は飛ばしてよいが、
// **同じ番号で別レイアウトの blob を作らない**。
// v2 (M52e): LOP 節へ audioHandleSeq
// v3 (M52g): LOP 節の prevTickInput を kMaxPlayers 本のレーン配列へ (レーン数を節に明記)
// v4 (M60'b): XPB 節 (XpbdBackend の池) を LOP 節の後・World 節の前に追加
// v5 (M61a): PTC 節へ prevOrigin/prevOriginValid/prewarmed + ParticleEmitterComponent の A群拡張
// v6 (M60'd): XPB 節へ attachValid/attachLx/Ly/Lz (終端アタッチの焼き込み)
// v7 (M63a): PTC 節へ rot0/rotVel/flipU + ParticleEmitterComponent の B群拡張 18 本
// v8 (M64a): InputSnapshot 64 -> 72 バイト (LOP 節の prevTickInput がレーン数ぶん太る)
// v9 (M64b): SCR 節の Start 済み記録を (エンティティ, スクリプト型) の 2 語へ
// v10 (M65a): ACU 節 (音響の波スロット表) を XPB 節の後・World 節の前に追加
// v11 (M65h): AcousticVolumeComponent へ glowKeepPerTick / glowIntensity、AcousticEmitterComponent へ footstepGain
// v12 (M70b): InputSnapshot 72 -> 88 バイト (UI キャンバスの 4 値)
// v13 (M70c): Scene 節に UI の対話状態 (hovered / pressed / clicked / focused = EntityID x 4)
// v14: Light.safeRadius (World 節のカラム生バイト)
// v15 (M71a): Scene 節に sceneName (スクリプトが GetSceneName で読む sim 状態)
// v16 (M18 追補): SkinnedMesh へ loop / fadeTicks とクロスフェードの再生状態 5 本
// v17: 欠番 (使い回さない)
// v18 (M75b): InputSnapshot 88 -> 112 バイト + Scene 節の UI 対話状態に changed / pressSurfX/Y / prevSurfX/Y / dragging
// v19 (M65i): AcousticVolumeComponent へ glowAlbedoMix
// v20 (2026-09-13): AcousticVolumeComponent へ glowDecayEveryTicks
// v21 (2026-09-14): AcousticField::kMaxWaves 16 -> 32 (古い blob は ReadAcoustic が本数不一致で拒む)
// v22: 組込みコンポーネントの 0/1 int32 フィールドを bool 化 (World 節の生カラムサイズ変更)
// v23: WaterWaveComponent へ surfaceMaterial (M79e) と timeTicks (浮力と水面の時計) を末尾追加
inline constexpr uint32_t kSimSnapshotVersion = 23;

// 撮る: out を clear して blob を書く。成功で true。
// 節ごとの参照が null なら「空の節」を書くのでレイアウトは常に同じ
bool CaptureSimSnapshot(const SimRefs& refs, std::vector<std::byte>& out);

// 戻す: blob を検証してから一括で差し替える。失敗時は**何も書き換えない**
// (途中まで復元された世界が一番たちが悪い)。
// refs 側に無い節は読み捨てる = 撮影時より少ない構成へも戻せる
bool RestoreSimSnapshot(const SimRefs& refs, const std::byte* data, size_t size);

// blob 先頭のヘッダだけ読む (.rep 埋め込み blob の素性確認 / ログ用)
bool PeekSimSnapshotTick(const std::byte* data, size_t size, uint64_t& outTick);

} // namespace mye
