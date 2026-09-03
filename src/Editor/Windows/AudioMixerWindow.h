#pragma once
#include <cstdint>
#include <string>

#include "Engine/Engine/Audio/AudioMixer.h"
#include "Engine/Engine/EngineLoop.h"

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

    void OnImGui(EngineContext& ctx);

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

    // 寿命は EngineContext と同じ。窓を開いていなければ nullptr = 編集していない
    AudioSystem* audio_ = nullptr;
    MixerLibrary* mixers_ = nullptr;
    int renaming_ = -1;         // 名前を編集中のバス (-1 = なし)
    bool renameFocus_ = false;  // 編集開始フレームだけキーボードフォーカスを移す
    char renameBuf_[64] = {};
    std::string status_;
};

} // namespace mye
