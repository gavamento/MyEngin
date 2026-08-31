#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "Engine/Platform/Input.h"

namespace mye {

class Scene;
class CpuParticleBackend;
class XpbdBackend;
class CollisionSystem;
class ScriptHost;

// sim レーンのスナップショット (M52d、決定台帳 1)。
// 「ある tick の sim 状態を丸ごと保存し、後でビット同一に復元し、そこから同じ入力で
// 回すと同じハッシュ列になる」ための共通部品。タイムトラベル (M52e) / クラッシュ
// バンドル (M52f) / ロールバック (M52i) の 3 者がこの 1 個に乗る。
//
// **対象は sim レーンだけ** = record/verify がハッシュを撮っている範囲と同一:
//   World (全アーキタイプのカラム生バイト + レコード表 + freeIndices + ルート + RNG)
//   Scene (TimeControl / PersistStore / nextFileId / sourcePath / override 表)
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
};

// blob の形式版。**.rep の版とは独立** (M52a 申し送り 7 と同じ規約) —
// blob のレイアウトを変えたらここだけを上げる。
// v2 (M52e): LOP 節へ audioHandleSeq を追加
// v3 (M52g): LOP 節の prevTickInput を kMaxPlayers 本のレーン配列へ (レーン数を節に明記)
// v4 (M60'b): XPB 節 (XpbdBackend の池) を LOP 節の後・World 節の前に追加
// v5 (M61a): PTC 節へ prevOrigin/prevOriginValid/prewarmed を追加。
//            ParticleEmitterComponent の A群拡張で descCache の Raw サイズも変化
// v6 (M60'd): XPB 節へ attachValid/attachLx/Ly/Lz (終端アタッチの焼き込み) を追加
// v7 (M63a): PTC 節へ rot0/rotVel/flipU (per-particle の不変属性) を追加。
//            ParticleEmitterComponent の B群拡張 18 本で descCache の Raw サイズも変化。
//            ★M63b〜e が消費するフィールドも M63a でまとめて確保してあるので、
//              B群で版が上がるのはこの 1 回だけ (分割して足すと 5 回上がる)
// v8 (M64a): InputSnapshot が 64 -> 72 バイト。LOP 節の prevTickInput が
//            レーン数ぶんそのまま太るので blob レイアウトが変わる
// v9 (M64b): SCR 節の Start 済み記録が (エンティティ) 1 語から
//            (エンティティ, スクリプト型) の 2 語へ。**同じエンティティに 2 つ以上
//            スクリプトを付けると 2 つ目以降の Start() が呼ばれない**バグの修正で、
//            キーの語数がそのまま blob の語数になる
inline constexpr uint32_t kSimSnapshotVersion = 9;

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
