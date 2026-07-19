#pragma once
#include <string>

namespace mye {

// エディタ設定の永続化 (assets\editor_settings.json)。
// project_settings.json (ParticleSystem) と同じ「読み直して該当キーだけ上書き」の
// マージ保存パターンで、imgui.ini が管理しない設定 (最後に開いたシーン・スナップ量・
// 外部エディタコマンド等) を保持する。root.value(key, default) で前方/後方互換。
struct EditorSettings {
    std::string lastScenePath;                       // 最後に開いた/保存したシーン (UTF-8)
    std::string externalEditorCmd = "code -g {file}:{line}"; // Console ソースジャンプ用 (M11)
    // Scene View ギズモのスナップ量 (M9)
    float snapTranslate = 0.5f;
    float snapRotateDeg = 15.0f;
    float snapScale = 0.1f;
    bool gridVisible = true;

    void Load(const std::wstring& assetsRoot);
    void Save() const;

private:
    std::wstring path_;
};

} // namespace mye
