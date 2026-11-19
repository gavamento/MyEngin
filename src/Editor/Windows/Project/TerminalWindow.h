//====================================================================================
//                          TerminalWindow.h
//  MyEngine/ 秋田蓮音                                                      11/19/2026
//                                          ターミナル窓 (ドック可能な ImGui 窓。セルの描画・キー/IME/貼付/選択・設定)
//====================================================================================
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "Editor/Tools/Terminal/TerminalLayout.h"
#include "Editor/Tools/Terminal/TerminalSession.h"
#include "Editor/Tools/Terminal/TerminalShellResolve.h"
#include "Editor/Tools/Terminal/TerminalVsEnv.h"

namespace mye {

// editor_global.json に保存する 4 点 (UTF-8)。EditorApp が EditorGlobalSettings と受け渡す。
// 範囲のクランプは窓の側 (使う直前に ClampTerminal*)。保存値はユーザーが入力したまま持つ
struct TerminalWindowSettings {
    std::string shell;                                  // 空 = 自動検出
    int fontSize = kTerminalDefaultFontSize;
    int scrollback = kTerminalDefaultScrollback;
    bool loadVsDevEnv = true;                           // false なら VS 開発者環境を読み込まない (次の再起動から反映)
};

// 窓が EditorApp に頼むこと (この窓は Windows.h を include しない)
struct TerminalWindowHost {
    std::string projectRoot;   // シェルの作業ディレクトリ (UTF-8)。空 = 親のまま
};

// ドック可能な「ターミナル」窓。TerminalSession を 1 つ所有する (1 パネル 1 シェル)。毎フレーム
//   1. Pump    : 窓が閉じていてもタブが隠れていても呼ぶ (子の出力を画面へ流し、DA 応答などを返す。
//                呼ばないと読み取りキューが満杯になり子が止まる)
//   2. OnImGui : 表示中のときだけ、ツールバー・状態行・セルの描画・入力
// の順で呼ぶ。シェルは最初にパネルが描かれたときに遅延起動する (VS 開発者環境を読む設定のときは、読み込みが終わるか
// 30 秒の時間切れになるまで起動を保留し、その間は「読み込み中」を出す)。窓を閉じてもセッションは残る。
// エディタ終了時は Shutdown で必ず破棄する (子孫ごと終わる)
class TerminalWindow {
public:
    bool open = false;

    // 保存済みの設定を読み込む (変更扱いにはしない)
    void ApplySettings(const TerminalWindowSettings& settings);
    // 変更があれば true を返して out に今の値を入れる (保存は呼び出し側)
    bool TakeSettingsChanged(TerminalWindowSettings& out);

    void Pump();
    void OnImGui(const TerminalWindowHost& host);
    void Shutdown();

private:
    void StartSession(const TerminalWindowHost& host, const TerminalGridSize& grid);
    // 窓が表示されたフレームの先頭で呼ぶ。VS 開発者環境の読み込みを (設定が有効なら) 始め・進め、起動を保留するかを vsEnvPending_ に置く
    void UpdateVsEnvGate();
    // ツールバーに出す VS 環境の状態。起動済みなら、その起動時に決まった状態 (後から読み込みが終わっても変えない)
    enum class VsEnvShown { Disabled, Loading, Loaded, Unavailable };
    VsEnvShown VsEnvShownState() const;
    // 今のセッションを捨てて、次に描かれるときに新しく起動する (設定のシェル・スクロールバックはそのとき読み直す)
    void RequestRestart();

    void DrawToolbar();
    void DrawStatusLine();
    void DrawSurface(const TerminalWindowHost& host);
    void DrawSettingsPopup();
    void DrawContextMenu();
    // 入力。フォーカスがあるときだけ呼ぶ
    void HandleKeyboard(int rows);
    void CopySelection();
    void PasteClipboard();
    void MarkSettingsDirty() { settingsDirty_ = true; }

    std::unique_ptr<TerminalSession> session_;
    TerminalShellError shellError_ = TerminalShellError::None;
    bool startAttempted_ = false;   // 起動を試みたか (失敗しても毎フレーム試し直さない。再起動で下ろす)
    // VS 開発者環境。エディタ起動中に 1 回だけ読む (決まった結果は再起動でもやり直さない)。最初に必要になったときに作る
    std::unique_ptr<TerminalVsEnvLoader> vsLoader_;
    bool vsEnvPending_ = false;                  // 読み込み完了待ちでシェルの起動を保留している
    VsEnvShown sessionVsEnv_ = VsEnvShown::Disabled;   // 今のセッションを起動した時点の VS 環境の状態
    int shellSerial_ = 0;                        // MYE_TERMINAL_ID の連番。シェルを起動するたびに 1 増やす
    std::string shellName_;         // 起動したシェルの実行ファイル名 (ツールバー表示)
    TerminalWindowSettings settings_;
    char shellBuf_[512] = {};       // 設定のシェルの編集用 (ImGui の入力欄が直接書く)。シェルの値の正本はこちら
    bool settingsDirty_ = false;
    TerminalGridSize grid_;
    TerminalSelection selection_;
    bool dragging_ = false;
    bool popupWasOpen_ = false;     // 前のフレームの終わりにポップアップ (自分の設定・メニュー、他窓のメニュー) が開いていたか
    int viewOffset_ = 0;            // 0 = 最下部。正 = スクロールバックを何行戻って見ているか
    std::vector<TerminalCell> rowCells_;   // 描画用の使い回し (毎フレームの確保を避ける)
    TerminalRowRuns rowRuns_;
};

} // namespace mye
