#pragma once
#include <string>

namespace mye {

// エディタ設定の永続化 (<dir>\editor_settings.json)。
// 置き場はプロジェクト起動時 <project>\.mye\、レガシー起動時 assets\ 直下 (M26)。
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
    // Scene View カメラの WASD 移動速度 (M27d。RMB ホールド中のホイールで調整)
    float camMoveSpeed = 6.0f;

    void Load(const std::wstring& dir);
    void Save() const;

private:
    std::wstring path_;
};

} // namespace mye
