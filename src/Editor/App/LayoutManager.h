#pragma once
#include <string>
#include <vector>

namespace mye {

// 名前付きエディタレイアウト (Unity の Layout ドロップダウン相当)。
// 1 レイアウト = <dir>\<name>.ini (ImGui ドック/ウィンドウ設定) + <name>.panels.json (パネル開閉)。
// ini はウィンドウの開閉状態を持たないため、EditorApp 所有の open フラグを sidecar に保存する。
// ロードは ImGui のどの Begin よりも前でないとドック再構築が同フレームで効かないため、
// DrawToolbarUi は要求を積むだけにして EditorApp::OnImGui の先頭で ApplyPendingLoad が実行する。
class LayoutManager {
public:
    struct PanelBinding {
        const char* key; // panels.json のキー (ウィンドウ名)
        bool* flag;      // EditorApp が所有する open フラグ (EditorApp と同寿命)
    };

    void Init(const std::wstring& layoutsDir, std::vector<PanelBinding> panels);

    // ツールバー右端のレイアウトボタン + ポップアップ + 保存モーダル。
    // 「リセット (既定)」選択で true を返す (呼び出し側が既定ドックを再構築する)
    bool DrawToolbarUi();

    // 保留中のレイアウトロードを実行する。**ImGui のどの Begin よりも前** に呼ぶこと —
    // LoadIniSettingsFromDisk が既存ウィンドウの DockId を差し替え、同フレームの Begin で
    // 新レイアウトのドックに再バインドされる (imgui 1.92 の ApplyAll ハンドラで確認済み)
    void ApplyPendingLoad();

private:
    void SaveCurrent(const std::string& name);
    std::vector<std::string> ListLayouts() const; // <dir> の *.ini を名前昇順で列挙
    std::wstring IniPath(const std::string& name) const;
    std::wstring PanelsPath(const std::string& name) const;

    std::wstring dir_;
    std::vector<PanelBinding> panels_;
    std::string pendingLoad_; // 非空 = 次フレーム冒頭でロードするレイアウト名
    char nameBuf_[64] = {};
    bool openSaveModal_ = false;
};

} // namespace mye
