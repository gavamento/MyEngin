//====================================================================================
//                          BrowserWindow.h
//  MyEngine/ 秋田蓮音                                                      11/19/2026
//                                          ブラウザ窓 (ドック可能な ImGui 窓。埋め込みの追従・airspace・設定)
//====================================================================================
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Editor/Tools/Browser/BrowserCatalog.h"
#include "Editor/Tools/Browser/BrowserHost.h"

namespace mye {

// editor_global.json に保存する 3 点 (UTF-8)。EditorApp が EditorGlobalSettings と受け渡す
struct BrowserWindowSettings {
    std::string id;         // 選んだ候補の id。"custom" = exe 指定。空 = 未選択 (既定の印のもの、無ければ先頭)
    std::string customExe;  // "custom" のときの exe パス
    std::string startUrl;   // 空 = ブラウザの既定
};

constexpr const char* kBrowserCustomId = "custom";

// 窓が EditorApp に頼むこと。Win32 のダイアログは窓へ持ち込まない (この窓は Windows.h を include しない)
struct BrowserWindowHost {
    // exe を選ぶファイルダイアログ。キャンセルは空
    std::function<std::wstring()> pickExe;
};

// ドック可能な「ブラウザ」窓。BrowserHost を 1 つ持ち、毎フレーム
//   1. OnImGui   : パネルの提出 (内容矩形を記録する)
//   2. ApplyToHost: 全 ImGui 窓の提出が終わった後に、見せる条件を集めて BrowserHost::Update へ渡す
// の順で呼ぶ。ApplyToHost はパネルが描かれないフレーム (タブ非アクティブ・閉じている) でも毎フレーム呼ぶこと
class BrowserWindow {
public:
    bool open = false;

    // 子窓の親 (メイン窓の HWND)
    void SetMainWindow(void* mainHwnd) { host_.SetMainWindow(mainHwnd); }
    // 保存済みの設定を読み込む (変更扱いにはしない)
    void ApplySettings(const BrowserWindowSettings& settings);
    // 変更があれば true を返して out に今の値を入れる (保存は呼び出し側)
    bool TakeSettingsChanged(BrowserWindowSettings& out);

    void OnImGui(const BrowserWindowHost& host);
    // 重なり判定から外す窓の ImGui ウィンドウ ID (入力を取らない一時表示 = 通知)。毎フレーム ApplyToHost の前に渡す
    void SetIgnoredWindowIds(const std::vector<unsigned int>& ids) { ignoredIds_ = ids; }
    // mainMinimized: メイン窓が最小化されているか (EditorApp が Win32 で見て渡す)
    void ApplyToHost(bool mainMinimized);
    // エディタ終了時。埋め込み中なら切り離す (メイン窓の Destroy より前に呼ぶ)
    void Shutdown();
    // マウスボタン押下のとき、子窓がフォーカスを持っていればメイン窓へ戻す
    void ReturnFocusToMain() { host_.ReturnFocusToMain(); }
    // ブラウザ上のマウスボタン押下の通知のとき、子窓へフォーカスを渡す予約をする (渡すのは次の ApplyToHost)
    void NoteMouseDownAt(int screenX, int screenY) { host_.NoteMouseDownAt(screenX, screenY); }

    BrowserHost& Host() { return host_; }
    const BrowserHost& Host() const { return host_; }
    const std::vector<BrowserCandidate>& Candidates() const { return candidates_; }
    BrowserRect ContentRect() const { return contentRect_; }
    // 直近の ApplyToHost の判定 (None = 見せた)
    BrowserHideReason LastHideReason() const { return lastHide_; }

    // 起動ボタンと同じ経路。選択中のブラウザを開始 URL で起動する。起動できなければ (exe 未選択・CreateProcess 失敗) false
    bool StartLaunch();
    // 検証用: id のブラウザを選ぶ (設定は変更扱いにしない)。無ければ false
    bool SelectBrowserById(const std::wstring& id);
    void SetStartUrl(const std::wstring& url);
    // 検出し直す (パネルを開いたとき / 再検出ボタン)
    void Redetect();

private:
    // 選択中の候補の添字。candidates_.size() = 「その他」
    size_t ResolveSelectedIndex() const;
    bool ResolveLaunchTarget(std::wstring& exe, BrowserFamily& family, std::string& name) const;
    // 選択中の起動対象で、窓の exe をどう突き合わせるか (自動特定と「既存の窓を取り込む」の一覧で共通)
    BrowserImageMatch SelectedImageMatch() const;
    void DrawIdle(const BrowserWindowHost& host);
    void DrawLaunching();
    void DrawEmbedded();
    void DrawHiddenPlaceholder();
    // 「既存の窓を取り込む」ボタンと一覧ポップアップ。一覧は押した瞬間にだけ列挙する (毎フレームは列挙しない)
    void DrawAttachExisting(bool canTarget);
    // Failed の 1 行 (種別の文言 + 必要なら英語の詳細)
    void DrawFailedLine() const;
    void MarkSettingsDirty() { settingsDirty_ = true; }

    BrowserHost host_;
    std::vector<unsigned int> ignoredIds_;
    std::vector<BrowserCandidate> candidates_;
    bool detected_ = false;
    bool wasOpen_ = false;
    std::string selectedId_;
    std::string customExe_;
    char urlBuf_[512] = {};
    bool settingsDirty_ = false;
    std::string embeddedName_;
    std::vector<BrowserWindowInfo> existingWindows_;
    int64_t launchStartMs_ = 0;
    // このフレームの OnImGui でパネルが描かれたか (ApplyToHost が読んで下ろす)
    bool panelDrawn_ = false;
    BrowserRect contentRect_;
    BrowserHideReason lastHide_ = BrowserHideReason::PanelNotDrawn;
};

} // namespace mye
