#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "Engine/Platform/Input.h"

namespace mye {

class Scene;
class CpuParticleBackend;
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
//   CpuParticleBackend の池 / CollisionSystem の前 tick ペア / ScriptHost の Start 済み記録
//   EngineLoop の prevTickInput (アクション評価の pressed/released 判定に効く)
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
    CollisionSystem* collision = nullptr;
    ScriptHost* scripts = nullptr;
    InputSnapshot* prevTickInput = nullptr;
    uint64_t* tickIndex = nullptr; // 撮影時に読み、復元時に書き戻す (null なら素通し)
};

// blob の形式版。**.rep の版とは独立** (M52a 申し送り 7 と同じ規約) —
// blob のレイアウトを変えたらここだけを上げる
inline constexpr uint32_t kSimSnapshotVersion = 1;

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
