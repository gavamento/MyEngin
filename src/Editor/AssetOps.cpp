#include "Editor/AssetOps.h"

#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <Windows.h>
#include <shellapi.h>

#include "nlohmann/json.hpp"

#include "Editor/Selection.h"
#include "Editor/Undo/UndoStack.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Animation.h"
#include "Engine/Engine/EngineLoop.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/FbxLoader.h"
#include "Engine/Engine/ModelLoader.h"
#include "Engine/Engine/Prefab.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/Script/ManagedHost.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GpuResources.h"

namespace fs = std::filesystem;
using namespace DirectX;

namespace mye {
namespace {

// ファイル名向けサニタイズ (英数 _ - 空白のみ残す)。空なら fallback
std::string SanitizeFileName(const std::string& in, const char* fallback)
{
    std::string out;
    for (char c : in) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '_' || c == '-' || c == ' ') {
            out += c;
        }
    }
    const size_t b = out.find_first_not_of(' ');
    const size_t e = out.find_last_not_of(' ');
    if (b == std::string::npos) {
        return fallback;
    }
    return out.substr(b, e - b + 1);
}

// C++ 識別子向けサニタイズ (英数 _ のみ、先頭は英字。数字始まり/空は Script を前置)
std::string SanitizeIdentifier(const std::string& in)
{
    std::string out;
    for (char c : in) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '_') {
            out += c;
        }
    }
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) {
        out = "Script" + out;
    }
    return out;
}

void ReplaceAll(std::string& s, const std::string& from, const std::string& to)
{
    if (from.empty()) {
        return;
    }
    for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size())) {
        s.replace(p, from.size(), to);
    }
}

bool EndsWith(const std::string& s, const char* suffix)
{
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::wstring RepoRoot(EngineContext& ctx)
{
    return fs::path(ctx.assetsRoot).parent_path().wstring(); // assets\ の親 = リポジトリルート
}

} // namespace

bool CreateFolderAsset(const std::wstring& dir, const std::string& name)
{
    const std::wstring p = dir + L"\\" + Utf8ToWide(SanitizeFileName(name, "New Folder"));
    std::error_code ec;
    if (fs::create_directory(p, ec)) {
        MYE_LOG_INFO("created folder: %s", WideToUtf8(p).c_str());
        return true;
    }
    MYE_LOG_WARN("could not create folder: %s", WideToUtf8(p).c_str());
    return false;
}

std::wstring CreateSceneAsset(const std::wstring& dir, const std::string& name)
{
    const std::string safe = SanitizeFileName(name, "New Scene");
    const std::wstring path = dir + L"\\" + Utf8ToWide(safe) + L".scene.json";
    nlohmann::json root;
    root["engine"] = "MyEngine";
    root["version"] = 2;
    root["sceneName"] = safe;
    root["nextFileId"] = 1;
    root["entities"] = nlohmann::json::array();
    std::ofstream f{ fs::path(path) };
    if (!f) {
        MYE_LOG_ERROR("could not write scene: %s", WideToUtf8(path).c_str());
        return {};
    }
    f << root.dump(2);
    MYE_LOG_INFO("created scene: %s", WideToUtf8(path).c_str());
    return path;
}

std::wstring CreateAnimationAsset(EngineContext& ctx, const std::wstring& dir, const std::string& name)
{
    if (!ctx.anims) {
        return {};
    }
    const std::string safe = SanitizeFileName(name, "New Clip");
    const std::wstring path = dir + L"\\" + Utf8ToWide(safe) + L".anim.json";
    AnimationClipAsset clip;
    clip.name = safe;
    clip.lengthTicks = 60;
    const uint64_t hash = ctx.anims->Register(path, clip);
    if (hash == 0 || !ctx.anims->SaveToFile(hash)) {
        MYE_LOG_ERROR("could not write animation: %s", WideToUtf8(path).c_str());
        return {};
    }
    MYE_LOG_INFO("created animation clip: %s", WideToUtf8(path).c_str());
    return path;
}

std::wstring CreateMaterialAsset(EngineContext& ctx, const std::wstring& dir, const std::string& name)
{
    const std::string safe = SanitizeFileName(name, "New Material");
    const std::wstring path = dir + L"\\" + Utf8ToWide(safe) + L".mat.json";
    nlohmann::json root;
    root["engine"] = "MyEngine";
    root["material"] = 1;
    root["name"] = safe;
    root["shader"] = "forward_lit";
    root["baseColor"] = { 0.8, 0.8, 0.8, 1.0 };
    root["metallic"] = 0.0;
    root["roughness"] = 0.5;
    root["texture"] = "";   // assets ルート相対パス (空 = 白テクスチャ)
    root["normalMap"] = ""; // 空 = ノーマルマップなし
    root["transparent"] = false;
    std::ofstream f{ fs::path(path) };
    if (!f) {
        MYE_LOG_ERROR("could not write material: %s", WideToUtf8(path).c_str());
        return {};
    }
    f << root.dump(2);
    f.close();
    // 生成直後に登録 → 参照ピッカー / ダブルクリック割り当てで即使える
    if (ctx.resources) {
        ctx.resources->materials.LoadFromFile(path, ctx.resources->textures, ctx.assetsRoot);
    }
    MYE_LOG_INFO("created material: %s", WideToUtf8(path).c_str());
    return path;
}

std::wstring CreateCppScript(EngineContext& ctx, const std::string& rawName)
{
    const std::string name = SanitizeIdentifier(rawName);
    const std::wstring dir = RepoRoot(ctx) + L"\\src\\GameLogic\\Scripts";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::wstring path = dir + L"\\" + Utf8ToWide(name) + L".cpp";
    if (fs::exists(path)) {
        MYE_LOG_WARN("script already exists: %s", WideToUtf8(path).c_str());
        return path;
    }
    std::string t =
        "#include <math.h>\n"
        "#include \"Shared/ScriptAPI.h\"\n"
        "\n"
        "// {NAME} — C++ スクリプト。ビルド後、Inspector の Add Component から付与できます。\n"
        "// フィールドは POD 型のみ (float/int32/uint32/uint64/bool/MyeVec*/MyeQuat/MyeColor/MyeEntityId)。\n"
        "struct {NAME} : Script<{NAME}> {\n"
        "    float speed = 1.0f; // Inspector に出る & ホットリロードを跨いで保持される\n"
        "\n"
        "    void Start(MyeUpdateContext& ctx)\n"
        "    {\n"
        "        MyeLogf(ctx, \"{NAME} started (speed=%.2f)\", speed);\n"
        "    }\n"
        "\n"
        "    void Update(MyeUpdateContext& ctx)\n"
        "    {\n"
        "        // 毎 tick (1/60s) 呼ばれる。決定論のため乱数は ctx.api->RandomRange を使うこと。\n"
        "        (void)ctx;\n"
        "        // 例: self の Transform を操作する\n"
        "        // MyeGameObject self = MyeSelf(ctx);\n"
        "        // MyeVec3 p = self.GetLocalPosition();\n"
        "        // p.y += speed * ctx.dt;\n"
        "        // self.SetLocalPosition(p);\n"
        "    }\n"
        "};\n"
        "REGISTER_SCRIPT({NAME}, FIELDS(speed));\n";
    ReplaceAll(t, "{NAME}", name);
    std::ofstream f{ fs::path(path) };
    if (!f) {
        MYE_LOG_ERROR("could not write script: %s", WideToUtf8(path).c_str());
        return {};
    }
    f << t;
    MYE_LOG_INFO("created C++ script: %s", WideToUtf8(path).c_str());
    MYE_LOG_INFO("edit it, then click 'Rebuild Scripts' in the Assets panel to compile + hot-reload.");
    return path;
}

std::wstring CreateCSharpScript(EngineContext& ctx, const std::string& rawName)
{
    const std::string name = SanitizeIdentifier(rawName);
    const std::wstring dir = ctx.assetsRoot + L"\\scripts";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::wstring path = dir + L"\\" + Utf8ToWide(name) + L".cs";
    if (fs::exists(path)) {
        MYE_LOG_WARN("C# script already exists: %s", WideToUtf8(path).c_str());
        return path;
    }
    std::string t =
        "using MyeScripting;\n"
        "\n"
        "// {NAME} — C# スクリプト。Assets パネルの [Compile C# Scripts] でコンパイルし、\n"
        "// Inspector の Add Component から付与できます。public フィールドは Inspector に出ます。\n"
        "public class {NAME} : MyeScript\n"
        "{\n"
        "    public float speed = 1.0f;\n"
        "\n"
        "    public override void Start()\n"
        "    {\n"
        "        Log(\"{NAME} started (speed=\" + speed + \")\");\n"
        "    }\n"
        "\n"
        "    public override void Update(float dt)\n"
        "    {\n"
        "        // 毎 tick (1/60s) 呼ばれる。self の Transform を操作する例:\n"
        "        var p = Transform.LocalPosition;\n"
        "        p.Y += speed * dt;\n"
        "        Transform.LocalPosition = p;\n"
        "    }\n"
        "}\n";
    ReplaceAll(t, "{NAME}", name);
    std::ofstream f{ fs::path(path) };
    if (!f) {
        MYE_LOG_ERROR("could not write C# script: %s", WideToUtf8(path).c_str());
        return {};
    }
    f << t;
    MYE_LOG_INFO("created C# script: %s", WideToUtf8(path).c_str());
    MYE_LOG_INFO("edit it, then click 'Compile C# Scripts' in the Assets panel to compile in-engine.");
    return path;
}

void CompileCSharpScripts(EngineContext& ctx)
{
    if (!ctx.managedHost) {
        MYE_LOG_WARN("C# scripting host not available (.NET runtime not initialized)");
        return;
    }
    if (!ctx.managedHost->IsReady()) {
        MYE_LOG_WARN("C# scripting host not ready — check MyeScripting.dll / .NET 8 runtime");
        return;
    }
    MYE_LOG_INFO("compiling C# scripts (assets\\scripts\\*.cs) in-engine...");
    ctx.managedHost->CompileScripts(ctx.assetsRoot + L"\\scripts");
}

void InstantiateAssetAtPath(EngineContext& ctx, Selection& selection, UndoStack& undo,
                            const std::wstring& path, const XMFLOAT3* pos, uint64_t parentFileId)
{
    if (!ctx.scene) {
        return;
    }
    const std::string u = WideToUtf8(path);
    undo.BeginRecord("Place Asset", selection);
    uint64_t rootFid = 0;
    if (EndsWith(u, ".prefab.json")) {
        const uint64_t hash = ctx.prefabs ? ctx.prefabs->LoadFromFile(path) : 0;
        if (hash != 0) {
            rootFid = Prefab::Instantiate(*ctx.scene, *ctx.prefabs, hash, parentFileId);
            ctx.scene->GetWorld().ApplyStructuralChanges();
        }
    } else if (EndsWith(u, ".glb") || EndsWith(u, ".gltf") || EndsWith(u, ".fbx")) {
        GameObject o = EndsWith(u, ".fbx")
                           ? FbxLoader::Load(*ctx.scene, *ctx.resources, *ctx.shaders, path)
                           : ModelLoader::Load(*ctx.scene, *ctx.resources, *ctx.shaders, path);
        ctx.scene->GetWorld().ApplyStructuralChanges();
        if (o) {
            if (parentFileId != 0) {
                GameObject par = ctx.scene->FindByFileId(parentFileId);
                if (par) {
                    o.SetParent(par);
                    ctx.scene->GetWorld().ApplyStructuralChanges();
                }
            }
            rootFid = ctx.scene->EnsureFileId(o.Id());
        }
    } else {
        undo.CancelRecord();
        MYE_LOG_WARN("cannot place asset (drag a prefab or model): %s", u.c_str());
        return;
    }
    if (rootFid == 0) {
        undo.CancelRecord();
        MYE_LOG_WARN("failed to place asset: %s", u.c_str());
        return;
    }
    if (pos) {
        GameObject r = ctx.scene->FindByFileId(rootFid);
        if (r) {
            r.SetLocalPosition(pos->x, pos->y, pos->z);
        }
    }
    selection.SelectOnly(rootFid);
    undo.CaptureAfter(*ctx.scene, rootFid);
    undo.EndRecord(selection);
}

void OpenInExternalEditor(const std::string& editorCmd, const std::wstring& path)
{
    if (editorCmd.empty()) {
        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    std::string cmd = editorCmd;
    ReplaceAll(cmd, "{file}", WideToUtf8(path));
    ReplaceAll(cmd, "{line}", "1");
    const std::wstring wargs = L"/c " + Utf8ToWide(cmd);
    ShellExecuteW(nullptr, L"open", L"cmd.exe", wargs.c_str(), nullptr, SW_HIDE);
}

void RebuildGameLogic(EngineContext& ctx)
{
    const std::wstring repo = RepoRoot(ctx);
    const std::wstring bat = repo + L"\\tools\\build_scripts.bat";
    if (!fs::exists(bat)) {
        MYE_LOG_ERROR("build_scripts.bat not found: %s", WideToUtf8(bat).c_str());
        return;
    }
    // 実行中の構成を exe パスから判定 (bin\x64\<Config>\)。構成マクロ分岐は決定論ルールで不可
    const std::wstring exeDir = GetExecutableDir();
    const bool isRelease = exeDir.find(L"Release") != std::wstring::npos
        || exeDir.find(L"release") != std::wstring::npos;
    const std::wstring cfg = isRelease ? L"Release" : L"Debug";
    // cmd /c ""<bat>" <cfg>"  (スペース入りパス対応)
    const std::wstring args = L"/c \"\"" + bat + L"\" " + cfg + L"\"";
    MYE_LOG_INFO("building GameLogic (%s)... hot reload applies on success",
                 WideToUtf8(cfg).c_str());
    ShellExecuteW(nullptr, L"open", L"cmd.exe", args.c_str(), repo.c_str(), SW_SHOWNORMAL);
}

} // namespace mye
