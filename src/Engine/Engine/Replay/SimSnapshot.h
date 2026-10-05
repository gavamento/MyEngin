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
class NavSystem;
class BehaviorTreeSystem;
class CollisionSystem;
class ScriptHost;

// ワールドハッシュに畳む「ECS 外の sim 状態」の束 (WorldHasher の SimSources) を組む**唯一の場所**。
// record / verify / --hash-dump / タイムトラベルの自己検証 / ネットの開始ハッシュが全部ここを通る —
// 呼び出し側で波括弧初期化を手書きすると、項目を足したときに 1 か所だけ古いまま残る
// (M70c: acoustic を 4 か所で渡し忘れ、波の出るシーンでだけ crash .rep が全 tick 割れた)
SimSources SimSourcesOf(Scene& scene, const CpuParticleBackend* particles, const XpbdBackend* xpbd,
                        const AcousticField* acoustic, const NavSystem* nav = nullptr,
                        const BehaviorTreeSystem* behaviorTree = nullptr);

// sim レーンのスナップショット (M52d、決定台帳 1)。
// 「ある tick の sim 状態を丸ごと保存し、後でビット同一に復元し、そこから同じ入力で
// 回すと同じハッシュ列になる」ための共通部品。タイムトラベル (M52e) / クラッシュ
// バンドル (M52f) / ロールバック (M52i) の 3 者がこの 1 個に乗る。
//
// **対象は sim レーンだけ** = record/verify がハッシュを撮っている範囲と同一:
//   World (全アーキタイプのカラム生バイト + レコード表 + freeIndices + ルート + RNG)
//   Scene (TimeControl / UI 対話状態 / PersistStore / nextFileId / sourcePath / override 表)
//   SessionLanes (M81b。Scene が持つので refs.scene から引く = SimRefs に別参照を足さない)
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
    // M82c: NavMesh。復元されるのは Surface ごとの store の差し替え分・dtCrowd・スロット表で、ナビメッシュ本体は
    // .mnav から作り直す導出値。★RestoreSimSnapshot は World を差し替えた後に NavSystem::ApplySnapshot を呼ぶ
    // (Surface の .mnav を先に読み込んでから状態を当てる = 空の NavSystem へ復元しても次の Update が読み直さない)
    NavSystem* nav = nullptr;
    // M85: ビヘイビアツリーの実行状態 (ブラックボード・ノードごとの状態)。復元は World を差し替えた後
    // (BehaviorTreeComponent が残っているエンティティだけ表へ戻す)。ハッシュと対で撮る (3 点セット契約)
    BehaviorTreeSystem* behaviorTree = nullptr;

    // この束で撮るワールドハッシュの源 (SimSourcesOf)。scene は非 null が前提
    SimSources HashSources() const { return SimSourcesOf(*scene, particles, xpbd, acoustic, nav, behaviorTree); }
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
// v24 (M81b): SES 節 (SessionLanes) を ACU 節の後・World 節の前に追加
// v25 (M82c): NAV 節 (NavSystem の差し替え分・dtCrowd・スロット表) を SES 節の後・World 節の前に追加。NavMeshAgent コンポーネント
// v26 (M82d): CharacterControllerComponent へ stepOffset、NavMeshSurfaceComponent へ autoCellSize (World 節のカラム生バイト)
// v27 (M82f): Nav 節の slots に stuck / noProgressTicks / bestRemaining、障害物に yaw。NavMeshObstacleComponent と Surface.drawObstacles (World 節のカラム生バイト)
// v28 (M82g): Nav 節の障害物にエリア (NavMeshModifier の塗り替え)。NavMeshModifierComponent と NavMeshAgentComponent.areaMask (World 節のカラム生バイト)
// v29 (M82h): Nav 節の slots に Link の渡り (linkPhase 以下)、store の状態に Off-Mesh Link。NavMeshLinkComponent / NavMeshAgentComponent.linkComplete・linkStart・linkEnd / Surface.drawLinks (World 節のカラム生バイト)
// v30 (M83a): AIPerceptionComponent / AIStimulusSourceComponent (World 節のカラム生バイト)
// v31 (M84c): NavMeshAgentComponent.navFilter (World 節のカラム生バイト)
// v32 (M84d): NavMeshAgentComponent の細かい制御 (isStopped 以下、World 節のカラム生バイト) と Nav 節の crowd に avoidancePriority
// v33 (M84e): NavMeshSurfaceComponent の Link の自動生成 (generateLinks 以下、World 節のカラム生バイト)。
//            生成した Link は store の Off-Mesh Link の一覧に入る (書式は v29 のまま)
// v34 (M85a): BT 節 (BehaviorTreeSystem の表) を NAV 節の後・World 節の前に追加。BehaviorTreeComponent (World 節のカラム生バイト)
// v35 (M85c): BT 節の各エンティティの末尾に、種類別の追加状態 (MoveTo / RotateTo の保存値など) の生バイト列を追加
// v36 (M85d): SearchArea の追加状態 (BtSearchAreaState = 起点・今の点・残り個数・段階) が BT 節の追加状態の生バイトに入る
// v37 (M85e): BT 節の末尾にイベントの配送待ち (BtEvent の列。0 件でも件数は書く)
// v38 (M85g): PatrolRouteComponent (World 節のカラム生バイト) と、Patrol の追加状態 (BtPatrolState = 次の点・向き・待ち・段階) が BT 節の追加状態の生バイトに入る
inline constexpr uint32_t kSimSnapshotVersion = 38;

// 撮る: out を clear して blob を書く。成功で true。
// 節ごとの参照が null なら「空の節」を書くのでレイアウトは常に同じ
bool CaptureSimSnapshot(const SimRefs& refs, std::vector<std::byte>& out);

// 戻す: blob を検証してから一括で差し替える。失敗時は**何も書き換えない**
// (途中まで復元された世界が一番たちが悪い)。
// 唯一の例外: World を差し替えた後の Nav 節の適用 (.mnav の読み込みなど) は World が要るので事前に検証できない。
// その失敗では false を返すが World は復元済みで、失敗した Surface だけが Failed (ナビゲーション無効) になる。
// 呼び出し側は false を「巻き戻し失敗」として扱い、続けて再シムしてはいけない
// refs 側に無い節は読み捨てる = 撮影時より少ない構成へも戻せる
bool RestoreSimSnapshot(const SimRefs& refs, const std::byte* data, size_t size);

// blob 先頭のヘッダだけ読む (.rep 埋め込み blob の素性確認 / ログ用)
bool PeekSimSnapshotTick(const std::byte* data, size_t size, uint64_t& outTick);

// blob に入っている World RNG (state / inc) を読む。magic と版が現行でなければ false。
// ★World 節は blob の**最後**で、その末尾 16 バイトが RNG (CaptureSimSnapshot の並びに依存する。
//   SimSnapshotSelfTest が「撮った World の RNG と一致する」ことを固定している)。
// .rep ヘッダの rngState / rngInc との突き合わせ (ReplayPlayer::Load) に使う
bool PeekSimSnapshotWorldRng(const std::byte* data, size_t size, uint64_t& outState,
                             uint64_t& outInc);

} // namespace mye
