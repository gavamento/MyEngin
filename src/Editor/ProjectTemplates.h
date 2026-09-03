#pragma once
#include <string>
#include <vector>

namespace mye {

// 推奨 .gitignore の行 (M66h、spec §2 の S10)。**ここが正本** — 新規プロジェクトの
// テンプレも、Source Control 窓の「推奨 .gitignore を適用」も同じ配列から作る。
//   /.mye/     エディタの個人設定 (editor_settings.json / imgui.ini / layouts)
//   /cache/    クック済みアセット・スクリプトのビルド生成物
//   /dist/     --package の出力
//   /crash/    クラッシュバンドル (minidump + .rep。他人の機体の物は再現に使えない)
//   /save/     SaveGame の出力 (EngineLoop が <project>\save に置く)
//   /assets/scripts/Generated/  C# の生成コード
//   *.log      ビルドログ等
// ★末尾に足すこと。順序はテンプレのファイル内容そのものなので、並べ替えると
//   既存プロジェクトとの diff が無意味に増える
extern const char* const kRecommendedGitignore[7];

// テンプレとして書き出す本文 (各行 + '\n')
std::string RecommendedGitignoreText();

// 既存の .gitignore 本文に**足りない**推奨行だけを、推奨順で返す。
//   - 比較は行単位の完全一致 (前後の空白と CR を落としてから)。`git check-ignore` は
//     呼ばない — 「!否定行がある」「**/crash/ と書いてある」のような等価な書き方を
//     取りこぼすが、その代わりに**既存行を絶対に壊さない**方に倒している
//   - 空文字列 (ファイルが無い) なら全行が返る
std::vector<std::string> MissingGitignoreLines(const std::string& existingText);

// 不足行を末尾に足した本文を返す (不足が無ければ existingText をそのまま)。
// ★既存部分は**バイト単位でそのまま**。改行コードも並びもコメントも触らない
//   (触ると、押した人が書いていない行まで git の差分に乗る)。
//   末尾が改行で終わっていなければ 1 つだけ補ってから追記する
std::string GitignoreWithRecommended(const std::string& existingText);

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
