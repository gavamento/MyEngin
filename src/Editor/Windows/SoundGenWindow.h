#pragma once
#include <string>
#include <vector>

#include "Engine/Engine/Audio/AudioSystem.h"
#include "Engine/Engine/Audio/SynthCore.h"
#include "Engine/Engine/EngineLoop.h"

namespace mye {

// Sound Generator (M45b)。手続き的に効果音を合成し、試聴して .wav としてアセットへ書き出す。
// 合成そのものは Engine 層の純関数 (SynthCore.h) にあり、ここは UI だけを被せる。
// **テスト素材をバイナリコミットではなくコードで作れるようにするのが目的** —
// M45e (3D 減衰の確認には持続音が要る) / M45f (ループ素材が要る) の前提になる。
class SoundGenWindow {
public:
    bool open = false; // ツール窓なので既定は非表示 (Window メニューから開く)

    // assetDir = AssetBrowser の表示中フォルダ (空ならフォールバックで <assets>\audio)。
    // **ctx.assetsRoot 基準で解決すること** — FindAssetsRoot() を直接呼ぶとプロジェクト起動時に
    // 生成物がエンジンリポジトリ側へ落ちる (二経路規則)
    void OnImGui(EngineContext& ctx, const std::wstring& assetDir);

private:
    void ApplyPreset(int preset);
    void EnsureRendered();               // dirty_ なら SynthRender をやり直す
    void DrawWaveform(float height);     // ImDrawList で列毎の min/max を描く
    void RebuildPeaks(int columns);      // 波形描画キャッシュ (列数が変わった時のみ)
    void Preview(EngineContext& ctx);
    void Save(EngineContext& ctx, const std::wstring& assetDir);

    SynthParams params_;
    AudioClip clip_;
    bool dirty_ = true;

    // 波形描画キャッシュ。列 (ピクセル) 毎の最小/最大サンプル [-1,1]
    std::vector<float> peakMin_;
    std::vector<float> peakMax_;
    int peakColumns_ = 0;
    uint64_t peakSerial_ = 0; // clip_ が作り直された回数 (キャッシュの無効化キー)
    uint64_t clipSerial_ = 0;

    AudioHandle previewHandle_{};
    char saveName_[96] = "new_sfx";
    std::string status_; // 直近の保存結果 (窓の下端に表示)
};

} // namespace mye
