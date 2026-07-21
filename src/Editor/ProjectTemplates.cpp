#include "Editor/ProjectTemplates.h"

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
// GUID はパス由来なので新プロジェクトでは AssetDatabase::ScanAndSync に再生成させる
bool CopyTreeWithoutMeta(const fs::path& src, const fs::path& dst, std::string* outError)
{
    std::error_code ec;
    fs::create_directories(dst, ec);
    for (const auto& entry : fs::recursive_directory_iterator(src, ec)) {
        const fs::path rel = fs::relative(entry.path(), src, ec);
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
    if (tmpl == ProjectTemplate::Demo3D) {
        // assets 一式 (デモシーン自体は初回起動時の BuildDemoScene がコード生成する)。
        // editor_settings.json はユーザー環境設定なので持ち込まない
        if (!CopyTreeWithoutMeta(engineAssets, assets, outError)) {
            return false;
        }
        fs::remove(assets / L"editor_settings.json", ec);
    } else {
        // Empty: エンジンが要求する最小構成 — シェーダ一式 + 効果音 + 空の作業フォルダ。
        // シェーダはエンジン実装と対のため v1 はコピー配布 (将来 Engine/Project の 2 ルート化)
        if (!CopyTreeWithoutMeta(engineAssets / L"shaders", assets / L"shaders", outError)) {
            return false;
        }
        fs::create_directories(assets / L"audio", ec);
        fs::copy_file(engineAssets / L"audio" / L"beep.wav", assets / L"audio" / L"beep.wav",
                      fs::copy_options::overwrite_existing, ec);
        for (const wchar_t* sub : { L"scenes", L"scripts", L"models", L"textures", L"materials" }) {
            fs::create_directories(assets / sub, ec);
        }
    }

    ProjectManifest m;
    m.name = name.empty() ? WideToUtf8(root.filename().wstring()) : name;
    m.engineVersion = kEngineVersion;
    if (!SaveProjectManifest(dir, m)) {
        return SetError(outError, "cannot write project.mye.json");
    }

    fs::create_directories(root / kProjectLocalDir, ec);
    WriteTextFile(root / L".gitignore", "/.mye/\n/cache/\n/dist/\n");

    MYE_LOG_INFO("CreateProject: '%s' (%s) at %s", m.name.c_str(),
                 tmpl == ProjectTemplate::Demo3D ? "3D Demo" : "Empty",
                 WideToUtf8(dir).c_str());
    return true;
}

} // namespace mye
