//====================================================================================
//                          FeelMonitorWindow.h
//  MyEngin/ 秋田蓮音                                                       11/04/2026
//                                          手触りモニタ (M94i、ADR-031): 再生中の手触り・時間効果・先行入力の観測
//====================================================================================
#pragma once

namespace mye {

struct EngineContext;
struct EditorSettings;

// 何が・いつ鳴って・いま何を出しているかを 1 枚で見る (AGENTS 3.2 の観測可能性)。
//   - 時間効果: ヒットストップの残り、スローの速さと残り、この tick の実効の速さ
//   - 再生中の手触り: スロットごとの名前 (プリセット名 / コード)・強さ・経過 / 持続・チャネル
//   - いまの出力: カメラのずらし・回転・FOV、振動、画面効果 (最新 tick の値)
//   - 先行入力: bufferTicks を持つアクションの窓 (レーン 0)
// 読むだけで sim の状態は書かない。例外は「エディタでは揺れを切る」(EditorSettings の個人設定。描画だけに効き、
// EditorApp が毎フレーム EngineContext::feelMuteCamera へ写す)
class FeelMonitorWindow {
public:
    bool open = false;
    void OnImGui(EngineContext& ctx, EditorSettings& settings);
};

} // namespace mye
