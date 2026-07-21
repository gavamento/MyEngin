#pragma once
#include <string>

namespace mye {

// 新規プロジェクトのテンプレート種別
enum class ProjectTemplate {
    Empty,  // シェーダ + 最小構成のみ (scenes/scripts は空フォルダ)
    Demo3D, // エンジン assets 一式をコピー (デモシーンは初回起動時にコード生成)
};

// <dir> に新規プロジェクトを作成する。
//   - dir は「存在しない」か「空フォルダ」であること (非空は拒否)
//   - engineAssetsRoot = エンジン側 assets の絶対パス (FindAssetsRoot())。コピー元
//   - 失敗時は false を返し、outError (UTF-8、null 可) に理由を格納
// 生成物: assets\ (テンプレート内容) / project.mye.json / .mye\ / .gitignore
bool CreateProject(const std::wstring& dir, const std::string& name, ProjectTemplate tmpl,
                   const std::wstring& engineAssetsRoot, std::string* outError);

} // namespace mye
