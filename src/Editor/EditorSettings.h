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
    // ---- Source Control の背景 fetch (M66f、spec §4.2) ----
    // ★**個人設定**なのでここ (.mye\editor_settings.json) に置く。project_settings.json に
    //   置くとチームで共有されてしまい、「同僚のリポジトリが 1 分ごとに fetch する」
    //   ような設定を押し付けることになる
    bool scmAutoFetch = true;
    int scmFetchIntervalMin = 5;
    // ---- パーティクルの調査用トグル (M66h、spec §4.2 の決定 8) ----
    // ★同上の理由でここ。以前は assets\project_settings.json (= チームで共有され、
    //   git に載る) にあり、比較モードを 1 回入れただけで「全員の画面に粒子が 2 重に出る」
    //   差分が push できてしまう状態だった。値は Editor が起動時と変更時に
    //   ParticleSystem の setter へ流す (Engine 層は EditorSettings を知らない)
    bool particleCompareMode = false;
    float particleCompareOffsetX = 4.0f;
    bool particleCpuSimd = true;

    void Load(const std::wstring& dir);
    void Save() const;

private:
    std::wstring path_;
};

} // namespace mye
