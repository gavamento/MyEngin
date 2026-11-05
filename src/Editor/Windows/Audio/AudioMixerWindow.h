#pragma once
#include <cstdint>
#include <string>

#include "Editor/Undo/UndoStack.h" // M93f: 文書 Undo
#include "Engine/Engine/Audio/Playback/AudioMixer.h"
#include "Engine/Engine/Loop/EngineLoop.h"

namespace mye {

// Audio Mixer (M45d)。**編集対象は AudioSystem のランタイムそのもの** —
// 窓は毎フレーム CurrentMixer() を読み、変更はランタイムへ直接書く。
// こうすると「アセットとランタイムのどちらが正か」という二重管理が消える。
// Save だけがランタイム → .mixer.json の書き出しになる。
//
// ランタイムの反映は 2 系統 (M45a の設計):
//   - 音量 / ミュート / ソロ / リバーブ送り → SetVolume / SetOutputMatrix (voice 再生成なし)
//   - バスの追加 / 削除 / 改名 / 親変更     → ApplyMixer (次の Update() でグラフ全体を作り直す)
class AudioMixerWindow {
public:
    bool open = false; // ツール窓なので既定は非表示 (Window メニューから開く)

    // undo (M93f): フェーダ / M・S / 送り / リバーブの連続編集は 1 フレームの前後の差分で、バスの追加・削除・
    // 改名・親変更・ミキサー切替・既定へ戻すは操作ごとに、ランタイムのミキサー全体 (MixerAsset の JSON) を
    // before / after として積む。Undo は ApplyMixer で戻す (次の Update で反映)。アクティブな .mixer.json の
    // 選択そのものは戻さない (ランタイムのバス構成だけが対象)
    void OnImGui(EngineContext& ctx, UndoStack& undo);

    // Asset Browser で .mixer.json がダブルクリックされた時に窓を開くためのフック
    void FocusOnActive() { open = true; }

    // ランタイムのミキサーが .mixer.json と食い違うか (M66d、spec §4.1 の S6)。
    // ★この窓は「ランタイムそのもの」を編集する = アセットとの差がそのまま未保存分。
    //   窓を一度も開いていなければ ApplyMixer 直後の状態 = 差は無いので、
    //   ポインタを控えるのが OnImGui の中でも判定を取りこぼさない
    bool HasUnsavedChanges() const;

private:
    void DrawStrip(EngineContext& ctx, int bus);
    void DrawMeter(EngineContext& ctx, int bus, float width, float height);
    void DrawFooter(EngineContext& ctx);
    // トポロジ編集: 現在のランタイムを写して書き換え、ApplyMixer で差し戻す
    void AddBus(EngineContext& ctx, int parentBus);
    void RemoveBus(EngineContext& ctx, int bus);
    void RenameBus(EngineContext& ctx, int bus, const std::string& newName);
    void ReparentBus(EngineContext& ctx, int bus, int newParent);
    void Save(EngineContext& ctx);
    // トポロジ編集 1 回を文書エントリとして積む (M93f)。ApplyMixer は次の Update で反映されるので、
    // フレーム末の差分では拾えない = 操作の場で before / after を積む
    void PushTopologyOp(EngineContext& ctx, const char* label, const MixerAsset& before, const MixerAsset& after);

    // 寿命は EngineContext と同じ。窓を開いていなければ nullptr = 編集していない
    AudioSystem* audio_ = nullptr;
    MixerLibrary* mixers_ = nullptr;
    int renaming_ = -1;         // 名前を編集中のバス (-1 = なし)
    bool renameFocus_ = false;  // 編集開始フレームだけキーボードフォーカスを移す
    char renameBuf_[64] = {};
    std::string status_;
    // M93f: 文書 Undo。undo_ は OnImGui の間だけ有効、docOpPushed_ はその枠で PushTopologyOp を呼んだか
    // (呼んだフレームはフレーム末の差分を取らない = 二重に積まない)
    UndoStack* undo_ = nullptr;
    bool docOpPushed_ = false;
    DocUndoRecorder docUndo_;
};

} // namespace mye
