#pragma once
#include <string>

namespace mye {

// externalEditorCmd の特別値: スクリプトを生成ソリューションごと Visual Studio で開く (OpenScriptInIde)
inline constexpr const char* kExternalEditorVisualStudio = "{vs}";
// 旧既定値 (VS Code)。保存済み設定にこの値がそのまま残っていれば Load で新しい既定値へ移行する
inline constexpr const char* kLegacyExternalEditorCmd = "code -g {file}:{line}";

// エディタ設定の永続化 (<dir>\editor_settings.json)。
// 置き場はプロジェクト起動時 <project>\.mye\、レガシー起動時 assets\ 直下 (M26)。
// project_settings.json (ParticleSystem) と同じ「読み直して該当キーだけ上書き」の
// マージ保存パターンで、imgui.ini が管理しない設定 (最後に開いたシーン・スナップ量・
// 外部エディタコマンド等) を保持する。root.value(key, default) で前方/後方互換。
struct EditorSettings {
    std::string lastScenePath;                       // 最後に開いた/保存したシーン (UTF-8)
    std::string externalEditorCmd = kExternalEditorVisualStudio; // スクリプトを開く / Console ソースジャンプ (M11)
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
    // ★同上の理由でここ。project_settings.json (チームで共有され、git に載る) に置くと、
    //   比較モードを 1 回入れただけで「全員の画面に粒子が 2 重に出る」差分が push できて
    //   しまう。値は Editor が起動時と変更時に
    //   ParticleSystem の setter へ流す (Engine 層は EditorSettings を知らない)
    bool particleCompareMode = false;
    float particleCompareOffsetX = 4.0f;
    bool particleCpuSimd = true;
    // ---- 手触り (M94i) ----
    // エディタで Play 中にカメラの揺れ・キックを切る (編集しながら長く見ると酔うため)。個人の好みなのでここ。
    // ゲームの出力倍率 (SetFeelOutputScale) とは別で、sim にも録画にも影響しない
    bool feelMuteCamera = false;

    void Load(const std::wstring& dir);
    void Save() const;

private:
    std::wstring path_;
};

} // namespace mye
