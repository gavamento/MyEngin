#pragma once
#include <string>

namespace mye {

// エンジンのバージョン表記。project.mye.json の engineVersion と照合し、
// 不一致はプロジェクトマネージャで警告表示する (起動はブロックしない)。
// 配布 (M98) の正本でもある: GitHub Releases のタグ v<これ> と一致しないと release.yml が止まり、
// Launcher / エディタの更新確認はこれと Release の版を比べる (EngineRelease.h の ParseEngineVersion の形)
constexpr const char* kEngineVersion = "0.88.3";

// プロジェクトルート直下のマニフェストファイル名
constexpr const wchar_t* kProjectManifestFile = L"project.mye.json";
// プロジェクトローカル状態 (imgui.ini / editor_settings.json 等、VCS 除外) の置き場
constexpr const wchar_t* kProjectLocalDir = L".mye";

// シーンが無いときに何で開くか。作成時のテンプレートが書く
enum class ProjectStartContent {
    Unspecified, // キー無し (この項目より前に作られたプロジェクト)。資産の有無から推定する
    Demo,        // デモ材質を登録し、デモシーンを生成する
    Empty,       // デモ材質を登録せず、カメラとライトだけのシーンで開く
};

// プロジェクトマニフェスト (<root>\project.mye.json)。
// 読みは root.value(key, default) の前方互換パターン (EditorSettings と同じ)
struct ProjectManifest {
    std::string name;
    std::string engineVersion;
    std::string bootScene = "scenes/main.scene.json"; // <root>/assets/ 相対 (スラッシュ区切り)
    // M66b: プロジェクトを**作った**ときの絶対パス (任意。空 = 未記録)。
    // 起動時に今のパスと NormalizePathKey で比べ、食い違えば警告する。
    // ★これは「正しい場所」を強制するためのものではない (クローン先が人ごとに
    //   違うのは普通)。狙いは「チームで共有する project.mye.json に個人の
    //   ローカルパスが載ったまま」に気付ける口を 1 つ作ること。
    //   formatVersion は 1 のまま — 読み手は value(key, default) なので、
    //   このキーが無い旧マニフェストもそのまま読める
    std::string canonicalRoot;
    // JSON では "startContent": "demo" / "empty"。Unspecified はキーを書かない
    ProjectStartContent startContent = ProjectStartContent::Unspecified;
};

// dir がプロジェクトルートか (マニフェストの存在チェックのみ)
bool IsProjectRoot(const std::wstring& dir);
bool LoadProjectManifest(const std::wstring& root, ProjectManifest& out);
bool SaveProjectManifest(const std::wstring& root, const ProjectManifest& m);

// bootScene を assets 配下の絶対パス (バックスラッシュ区切り) に解決する
std::wstring ProjectBootScenePath(const std::wstring& root, const ProjectManifest& m);

} // namespace mye
