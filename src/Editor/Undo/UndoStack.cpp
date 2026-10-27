#include "Editor/Undo/UndoStack.h"

#include <algorithm>

#include "Editor/Scene/Selection.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Animation/AnimatorController.h" // M93f: 文書エントリ (Controller)
#include "Engine/Engine/Audio/Playback/AudioMixer.h"     // M93f: 文書エントリ (Mixer)
#include "Engine/Engine/Audio/Playback/AudioSystem.h"
#include "Engine/Engine/Loop/EngineLoop.h"               // EngineContext
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Prefab.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Scene/SceneSerializer.h"

namespace mye {

using nlohmann::json;

std::vector<uint64_t> UndoStack::FileIdsOf(const json& arr)
{
    std::vector<uint64_t> out;
    if (!arr.is_array()) {
        return out;
    }
    for (const json& item : arr) {
        const uint64_t fid = item.value("fileId", 0ull);
        if (fid != 0 && std::find(out.begin(), out.end(), fid) == out.end()) {
            out.push_back(fid);
        }
    }
    return out;
}

void UndoStack::BeginRecord(const char* label, const Selection& selBefore)
{
    pending_ = Entry{};
    pending_.label = label ? label : "Edit";
    pending_.selBefore = selBefore.ids;
    pending_.primaryBefore = selBefore.primary;
    pending_.session = currentSession_;
    recording_ = true;
}

void UndoStack::CaptureBefore(Scene& scene, uint64_t fileId)
{
    if (!recording_) {
        return;
    }
    GameObject g = scene.FindByFileId(fileId);
    if (!g) {
        return;
    }
    json sub = SceneSerializer::SubtreeToJson(scene, g.Id());
    for (json& item : sub) {
        pending_.before.push_back(std::move(item));
    }
}

void UndoStack::CaptureAfter(Scene& scene, uint64_t fileId)
{
    if (!recording_) {
        return;
    }
    GameObject g = scene.FindByFileId(fileId);
    if (!g) {
        return;
    }
    // ---- プレハブ override リストの記録 (M48e) ----
    // **エディタの編集経路はすべてここを通る** (Inspector のフィールド/名前、ギズモ、
    // Hierarchy のリネーム/D&D、コンポーネントの貼り付け・リセット・追加・削除、Revert)。
    // 個々の編集箇所に MarkOverride を撒くと必ず取りこぼしが出て、次のロードで
    // RefreshNonOverridden がその上書きをベース値に戻してしまう = 静かなデータ損失になる。
    // 編集直後はベースが現行値と一致している唯一のタイミングなので、ここでライブ diff を
    // 撮り直すのが最も安全。SubtreeToJson の**前**に撮ること — Undo ペイロードにも
    // 更新後の overrides キーが載り、Undo/Redo でリストごと復元される
    if (prefabs_) {
        Prefab::RecordOverridesSubtree(scene, *prefabs_, g.Id());
    }
    json sub = SceneSerializer::SubtreeToJson(scene, g.Id());
    for (json& item : sub) {
        pending_.after.push_back(std::move(item));
    }
}

void UndoStack::EndRecord(const Selection& selAfter)
{
    if (!recording_) {
        return;
    }
    recording_ = false;

    pending_.selAfter = selAfter.ids;
    pending_.primaryAfter = selAfter.primary;

    const std::vector<uint64_t> beforeIds = FileIdsOf(pending_.before);
    const std::vector<uint64_t> afterIds = FileIdsOf(pending_.after);
    for (uint64_t fid : afterIds) {
        if (std::find(beforeIds.begin(), beforeIds.end(), fid) == beforeIds.end()) {
            pending_.createdIds.push_back(fid);
        }
    }
    for (uint64_t fid : beforeIds) {
        if (std::find(afterIds.begin(), afterIds.end(), fid) == afterIds.end()) {
            pending_.destroyedIds.push_back(fid);
        }
    }

    // 実変化が無ければ積まない (フィールドを触っただけで値が変わっていない等)
    if (pending_.createdIds.empty() && pending_.destroyedIds.empty()
        && pending_.before == pending_.after) {
        return;
    }

    pending_.serial = ++serialCounter_; // 新しい状態 ID (StateSerial / ダーティ判定用)
    undo_.push_back(std::move(pending_));
    redo_.clear();
}

void UndoStack::CancelRecord()
{
    recording_ = false;
    pending_ = Entry{};
}

void UndoStack::ApplyStructuralChanges(Scene& scene)
{
    scene.GetWorld().ApplyStructuralChanges();
}

void UndoStack::PushFileOp(const char* label, UndoFileOp op)
{
    Entry e;
    e.label = label ? label : "File Op";
    e.fileOp = std::move(op);
    // serial は現状態を継承 — ファイル操作はシーン状態を変えないので、これを新規採番すると
    // 保存済みシーンが偽 dirty (未保存確認モーダル) になる。session は既定の 0 のまま
    // (Play 停止でディスクは巻き戻らない = EndPlaySession の破棄対象にしない)
    e.serial = StateSerial();
    undo_.push_back(std::move(e));
    redo_.clear();
}

void UndoStack::PushDocOp(const char* label, UndoDocOp op)
{
    if (op.kind == UndoDocOp::Kind::None || op.before == op.after) {
        return; // 実変化なし
    }
    Entry e;
    e.label = label ? label : "Edit";
    e.docOp = std::move(op);
    e.serial = StateSerial(); // 文書はシーン状態ではない — 新規採番するとシーンが偽 dirty になる (PushFileOp と同じ)
    undo_.push_back(std::move(e));
    redo_.clear();
}

bool ExecuteDocOp(EngineContext* ctx, const UndoDocOp& op, bool redo)
{
    if (ctx == nullptr) {
        MYE_LOG_WARN("[undo] document op skipped: no engine context");
        return false;
    }
    const json& target = redo ? op.after : op.before;
    switch (op.kind) {
    case UndoDocOp::Kind::Controller: {
        ControllerAsset* asset = ctx->controllers != nullptr ? ctx->controllers->GetMutable(op.hash) : nullptr;
        if (asset == nullptr) {
            MYE_LOG_WARN("[undo] controller %016llx is no longer registered - edit not restored",
                         static_cast<unsigned long long>(op.hash));
            return false;
        }
        // 同一性 (hash / name / path) は保ち、中身だけ JSON から戻す。FromJson は states を必須にする
        ControllerAsset restored = *asset;
        if (!ControllerLibrary::FromJson(target, restored)) {
            MYE_LOG_WARN("[undo] controller %s: stored JSON could not be read - edit not restored", asset->name.c_str());
            return false;
        }
        restored.hash = asset->hash;
        restored.name = asset->name;
        restored.path = asset->path;
        *asset = std::move(restored);
        return true;
    }
    case UndoDocOp::Kind::Mixer: {
        if (ctx->audio == nullptr) {
            MYE_LOG_WARN("[undo] mixer edit skipped: no audio system");
            return false;
        }
        MixerAsset m;
        if (!MixerLibrary::FromJson(target, m)) {
            MYE_LOG_WARN("[undo] mixer: stored JSON could not be read - edit not restored");
            return false;
        }
        ctx->audio->ApplyMixer(m); // 音量もトポロジも ApplyMixer 1 本で戻る (次の Update で反映)
        return true;
    }
    case UndoDocOp::Kind::None:
        break;
    }
    return false;
}

void DocUndoRecorder::Observe(UndoStack& undo, const char* label, UndoDocOp::Kind kind, uint64_t docHash,
                              const json& pre, json post, bool anyItemActive)
{
    if (pending && hash != docHash) {
        pending = false; // 文書が切り替わった: 前の文書の after が無いので捨てる
        before = json();
    }
    if (!pending && post != pre) {
        pending = true;
        hash = docHash;
        before = pre;
    }
    if (!pending || anyItemActive) {
        return; // 変化なし / 連続操作の途中 (離したフレームで 1 つに畳む)
    }
    pending = false;
    UndoDocOp op;
    op.kind = kind;
    op.hash = docHash;
    op.before = std::move(before);
    op.after = std::move(post);
    before = json();
    undo.PushDocOp(label, std::move(op)); // 往復して元に戻っていれば PushDocOp が捨てる
}

void UndoStack::ApplyStep(Scene& scene, Selection& sel, const json& payload,
                          const std::vector<uint64_t>& destroyFirst,
                          const std::vector<uint64_t>& selIds, uint64_t primary)
{
    World& world = scene.GetWorld();
    for (uint64_t fid : destroyFirst) {
        GameObject g = scene.FindByFileId(fid);
        if (g) {
            world.DestroyEntity(g.Id());
        }
    }
    world.ApplyStructuralChanges(); // 破棄を確定してから再適用する
    // ペイロードはサブツリーの全量スナップショットなので、プレハブタグ (隠しコンポーネント) も
    // JSON が正解。消せないままだと「Create Prefab → Undo」でタグだけ残る (M48c)
    SceneSerializer::ApplyPartial(scene, payload, /*removeHiddenMissing=*/true);
    sel.Set(selIds, primary);
}

void UndoStack::Undo(Scene& scene, Selection& sel)
{
    if (undo_.empty()) {
        return;
    }
    Entry e = std::move(undo_.back());
    undo_.pop_back();
    if (e.fileOp.kind != UndoFileOp::Kind::None) {
        // ファイル操作エントリ (M51i)。失敗 (逆操作先消滅 = WARN 済み no-op) でも
        // redo 側へ送る — 死んだエントリで Ctrl+Z が詰まらないようにする
        ExecuteAssetFileOp(fileOpCtx_, e.fileOp, /*redo=*/false);
    } else if (e.docOp.kind != UndoDocOp::Kind::None) {
        ExecuteDocOp(fileOpCtx_, e.docOp, /*redo=*/false); // 文書エントリ (M93f)。失敗しても同じく redo 側へ
    } else {
        // op が生成したものを破棄してから before を適用 (= op 前の状態へ)
        ApplyStep(scene, sel, e.before, e.createdIds, e.selBefore, e.primaryBefore);
    }
    redo_.push_back(std::move(e));
}

void UndoStack::Redo(Scene& scene, Selection& sel)
{
    if (redo_.empty()) {
        return;
    }
    Entry e = std::move(redo_.back());
    redo_.pop_back();
    if (e.fileOp.kind != UndoFileOp::Kind::None) {
        ExecuteAssetFileOp(fileOpCtx_, e.fileOp, /*redo=*/true); // 順操作の再適用 (M51i)
    } else if (e.docOp.kind != UndoDocOp::Kind::None) {
        ExecuteDocOp(fileOpCtx_, e.docOp, /*redo=*/true); // 文書エントリ (M93f)
    } else {
        // op が破棄したものを破棄してから after を適用 (= op 後の状態へ)
        ApplyStep(scene, sel, e.after, e.destroyedIds, e.selAfter, e.primaryAfter);
    }
    undo_.push_back(std::move(e));
}

void UndoStack::EndPlaySession()
{
    const uint32_t s = currentSession_;
    auto pred = [s](const Entry& e) { return e.session == s; };
    undo_.erase(std::remove_if(undo_.begin(), undo_.end(), pred), undo_.end());
    redo_.erase(std::remove_if(redo_.begin(), redo_.end(), pred), redo_.end());
    currentSession_ = 0;
}

void UndoStack::ClearAll()
{
    undo_.clear();
    redo_.clear();
    recording_ = false;
    pending_ = Entry{};
    baseSerial_ = ++serialCounter_; // 新しいシーンの基底状態 (過去の serial と重ならない)
}

} // namespace mye
