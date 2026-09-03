#include "Editor/ProjectTemplates.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "Engine/Core/Log.h"
#include "Engine/Engine/Project.h"
#include "Engine/Platform/PathUtil.h"

namespace fs = std::filesystem;

namespace mye {

namespace {

bool SetError(std::string* outError, const std::string& msg)
{
    if (outError) {
        *outError = msg;
    }
    MYE_LOG_ERROR("CreateProject: %s", msg.c_str());
    return false;
}

// .meta サイドカーを除いて再帰コピーする。
// GUID はパス由来なので新プロジェクトでは AssetDatabase::ScanAndSync に再生成させる。
// skipTopLevel が非 null なら、src 直下のそのフォルダ配下を丸ごとコピーしない
bool CopyTreeWithoutMeta(const fs::path& src, const fs::path& dst, std::string* outError,
                         const wchar_t* skipTopLevel = nullptr)
{
    std::error_code ec;
    fs::create_directories(dst, ec);
    for (const auto& entry : fs::recursive_directory_iterator(src, ec)) {
        const fs::path rel = fs::relative(entry.path(), src, ec);
        if (skipTopLevel != nullptr && !rel.empty() && rel.begin()->wstring() == skipTopLevel) {
            continue;
        }
        const fs::path to = dst / rel;
        if (entry.is_directory()) {
            fs::create_directories(to, ec);
        } else {
            if (entry.path().extension() == L".meta") {
                continue;
            }
            fs::create_directories(to.parent_path(), ec);
            fs::copy_file(entry.path(), to, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                return SetError(outError, "copy failed: " + WideToUtf8(to.wstring()));
            }
        }
    }
    return true;
}

void WriteTextFile(const fs::path& path, const std::string& text)
{
    std::ofstream f(path, std::ios::binary);
    if (f) {
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
}

} // namespace

// clang-format off
const char* const kRecommendedGitignore[7] = {
    "/.mye/",
    "/cache/",
    "/dist/",
    "/crash/",
    "/save/",
    "/assets/scripts/Generated/",
    "*.log",
};
// clang-format on

std::string RecommendedGitignoreText()
{
    std::string text;
    for (const char* line : kRecommendedGitignore) {
        text += line;
        text += '\n';
    }
    return text;
}

std::vector<std::string> MissingGitignoreLines(const std::string& existingText)
{
    // 既存行を「前後の空白と CR を落とした形」で集める。CRLF / LF の両方を受ける
    // (このリポジトリは core.autocrlf=true 前提なので、手元のファイルは CRLF もある)
    std::vector<std::string> have;
    for (size_t pos = 0; pos <= existingText.size();) {
        const size_t nl = existingText.find('\n', pos);
        const size_t end = (nl == std::string::npos) ? existingText.size() : nl;
        const std::string line = existingText.substr(pos, end - pos);
        const size_t first = line.find_first_not_of(" \t\r");
        const size_t last = line.find_last_not_of(" \t\r");
        if (first != std::string::npos) {
            have.push_back(line.substr(first, last - first + 1));
        }
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 1;
    }
    std::vector<std::string> missing;
    for (const char* want : kRecommendedGitignore) {
        if (std::find(have.begin(), have.end(), std::string(want)) == have.end()) {
            missing.emplace_back(want);
        }
    }
    return missing;
}

std::string GitignoreWithRecommended(const std::string& existingText)
{
    const std::vector<std::string> missing = MissingGitignoreLines(existingText);
    if (missing.empty()) {
        return existingText;
    }
    std::string text = existingText;
    if (!text.empty() && text.back() != '\n') {
        text += '\n';
    }
    for (const std::string& line : missing) {
        text += line;
        text += '\n';
    }
    return text;
}

bool CreateProject(const std::wstring& dir, const std::string& name, ProjectTemplate tmpl,
                   const std::wstring& engineAssetsRoot, std::string* outError)
{
    std::error_code ec;
    const fs::path root(dir);
    if (fs::exists(root, ec) && !fs::is_empty(root, ec)) {
        return SetError(outError, "directory is not empty: " + WideToUtf8(dir));
    }
    if (!fs::exists(engineAssetsRoot, ec)) {
        return SetError(outError,
                        "engine assets not found: " + WideToUtf8(engineAssetsRoot));
    }
    if (!fs::create_directories(root / L"assets", ec) && !fs::exists(root / L"assets", ec)) {
        return SetError(outError, "cannot create directory: " + WideToUtf8(dir));
    }

    const fs::path engineAssets(engineAssetsRoot);
    const fs::path assets = root / L"assets";
    // シェーダはコピーしない (2 ルート化): エンジン組込みを実行時に解決するので、
    // プロジェクトに複製するとエンジン更新に取り残されて機能が丸ごと死ぬ。
    // assets\shaders は「上書きしたいシェーダを置く場所」として空で作る
    if (tmpl == ProjectTemplate::Demo3D) {
        // assets 一式 (デモシーン自体は初回起動時の BuildDemoScene がコード生成する)。
        // editor_settings.json はユーザー環境設定なので持ち込まない
        if (!CopyTreeWithoutMeta(engineAssets, assets, outError, L"shaders")) {
            return false;
        }
        fs::remove(assets / L"editor_settings.json", ec);
        fs::create_directories(assets / L"shaders", ec);
    } else {
        // Empty: エンジンが要求する最小構成 — 効果音 + ミキサー + 空の作業フォルダ。
        // **default.mixer.json は必須** — 無いと新規プロジェクトのミキサー窓が空になり、
        // .sound.json の bus 参照が全部フォールバックに落ちる (M45d)
        fs::create_directories(assets / L"audio", ec);
        fs::copy_file(engineAssets / L"audio" / L"beep.wav", assets / L"audio" / L"beep.wav",
                      fs::copy_options::overwrite_existing, ec);
        fs::copy_file(engineAssets / L"audio" / L"default.mixer.json",
                      assets / L"audio" / L"default.mixer.json",
                      fs::copy_options::overwrite_existing, ec);
        for (const wchar_t* sub :
             { L"scenes", L"scripts", L"models", L"textures", L"materials", L"shaders" }) {
            fs::create_directories(assets / sub, ec);
        }
    }

    ProjectManifest m;
    m.name = name.empty() ? WideToUtf8(root.filename().wstring()) : name;
    m.engineVersion = kEngineVersion;
    // M66b: 作成時のパスを「正のパス」として刻む。以後ここと違う場所で開くと
    // Source Control 窓が警告を出す (= 誰かのローカルパスが共有マニフェストへ
    // 混ざったまま push された、を検知する唯一の手掛かり)
    m.canonicalRoot = WideToUtf8(fs::absolute(root).wstring());
    if (!SaveProjectManifest(dir, m)) {
        return SetError(outError, "cannot write project.mye.json");
    }

    fs::create_directories(root / kProjectLocalDir, ec);
    WriteTextFile(root / L".gitignore", RecommendedGitignoreText());

    MYE_LOG_INFO("CreateProject: '%s' (%s) at %s", m.name.c_str(),
                 tmpl == ProjectTemplate::Demo3D ? "3D Demo" : "Empty",
                 WideToUtf8(dir).c_str());
    return true;
}

} // namespace mye
