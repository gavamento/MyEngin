#include "Editor/AssetOps.h"

#include <cctype>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>

#include <Windows.h>
#include <shellapi.h>

#include "nlohmann/json.hpp"

#include "Editor/Selection.h"
#include "Editor/Undo/UndoStack.h"
#include "Engine/Core/ComponentRegistry.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Animation.h"
#include "Engine/Engine/AssetDatabase.h"
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

// ---- 外部ファイルインポート (エクスプローラー D&D) のヘルパー ----

} // namespace

// 既知の複合サフィックス (.scene.json 等) を保ったままファイル名を stem/suffix に分割する。
// 素朴な extension() 分割だと連番付与で "x.prefab (1).json" になり種別判定が壊れる
void SplitAssetName(const std::wstring& filename, std::wstring& stem, std::wstring& suffix)
{
    static const std::wstring kCompound[] = {L".scene.json", L".prefab.json", L".anim.json",
                                             L".mat.json", L".controller.json"};
    for (const std::wstring& c : kCompound) {
        if (filename.size() > c.size() &&
            filename.compare(filename.size() - c.size(), c.size(), c) == 0) {
            stem = filename.substr(0, filename.size() - c.size());
            suffix = c;
            return;
        }
    }
    const fs::path p{ filename };
    stem = p.stem().wstring();
    suffix = p.extension().wstring();
}

namespace {

// destDir 直下で衝突しない絶対パスを返す ("name.ext" → "name (1).ext" → ...)
std::wstring MakeUniquePath(const std::wstring& destDir, const std::wstring& filename, bool isDir)
{
    std::wstring stem = filename;
    std::wstring suffix;
    if (!isDir) {
        SplitAssetName(filename, stem, suffix);
    }
    std::error_code ec;
    std::wstring candidate = destDir + L"\\" + filename;
    for (int i = 1; fs::exists(candidate, ec); ++i) {
        candidate = destDir + L"\\" + stem + L" (" + std::to_wstring(i) + L")" + suffix;
    }
    return candidate;
}

// インポート対象外のソース (.meta サイドカー / OS ゴミファイル / 隠しファイル)。
// 外部プロジェクト由来の .meta を持ち込むと GUID 衝突の恐れがあるため必ず除外する
bool ShouldSkipSource(const fs::path& p)
{
    const std::wstring name = p.filename().wstring();
    if (name.empty() || name[0] == L'.') {
        return true;
    }
    if (AssetDatabase::IsMetaPath(name)) {
        return true;
    }
    std::wstring lower = name;
    for (wchar_t& c : lower) {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return lower == L"desktop.ini" || lower == L"thumbs.db";
}

// コピー成功後の登録: .meta 生成 + 実行時 GUID テーブルへ反映。
// ScanAndSync は起動時 1 回のみなので、静的 EnsureMeta だけだと再起動まで GUID 解決できない
void RegisterImported(EngineContext& ctx, const std::wstring& destPath)
{
    if (ctx.assetDb) {
        ctx.assetDb->GuidForPath(destPath, /*createIfMissing=*/true);
    } else {
        AssetDatabase::EnsureMeta(destPath);
    }
}

// フォルダを再帰コピー。fs::copy(recursive) ではなくファイル単位で回すことで、
// 途中エラーでも継続でき、.meta 除外と件数計上を同時に行える
void CopyDirRecursive(EngineContext& ctx, const fs::path& srcDir, const fs::path& dstDir,
                      ImportResult& result)
{
    std::error_code ec;
    fs::create_directories(dstDir, ec);
    if (!fs::is_directory(dstDir, ec)) {
        MYE_LOG_ERROR("import: could not create folder: %s", WideToUtf8(dstDir.wstring()).c_str());
        result.failed++;
        return;
    }
    for (const fs::directory_entry& entry : fs::directory_iterator{ srcDir, ec }) {
        if (entry.is_directory(ec)) {
            CopyDirRecursive(ctx, entry.path(), dstDir / entry.path().filename(), result);
        } else if (ShouldSkipSource(entry.path())) {
            result.skipped++;
        } else {
            const fs::path dst = dstDir / entry.path().filename();
            ec.clear();
            if (fs::copy_file(entry.path(), dst, ec) && !ec) {
                RegisterImported(ctx, dst.wstring());
                result.imported++;
            } else {
                MYE_LOG_ERROR("import: copy failed: %s (%s)",
                              WideToUtf8(entry.path().wstring()).c_str(), ec.message().c_str());
                result.failed++;
            }
        }
    }
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

bool AttachScriptToEntity(EngineContext& ctx, Selection& selection, UndoStack& undo,
                          const std::wstring& csPath, EntityID target)
{
    if (AssetDatabase::ClassifyPath(csPath) != AssetType::Script) {
        return false; // .cs 以外は対象外 (呼び出し側で他ペイロード処理に振り分ける)
    }
    World& world = ctx.scene->GetWorld();
    if (!world.IsAlive(target)) {
        return false;
    }
    // クラス名 = ファイル名 stem。生成 .cs は namespace 無し → C# FullName == stem == 登録名
    const std::string className = WideToUtf8(fs::path(csPath).stem().wstring());
    ComponentTypeId t = ComponentRegistry::Get().FindByName(className);
    if (t == kInvalidComponentType) {
        // 未コンパイルの可能性 → エンジン内 Roslyn でコンパイル (RegisterTypes まで走る) して再解決
        CompileCSharpScripts(ctx);
        t = ComponentRegistry::Get().FindByName(className);
    }
    if (t == kInvalidComponentType) {
        MYE_LOG_WARN("cannot attach script '%s': component not registered "
                     "(is it under assets\\scripts and compiled?)",
                     className.c_str());
        return false;
    }
    if (world.HasComponent(target, t)) {
        MYE_LOG_WARN("script '%s' is already attached to this entity", className.c_str());
        return false;
    }
    // Add Component と同一の Undo 雛形 (InspectorWindow の Add Component 経路と一致)
    const uint64_t fid = ctx.scene->EnsureFileId(target);
    undo.BeginRecord("Attach Script", selection);
    undo.CaptureBefore(*ctx.scene, fid);
    world.AddComponentRaw(target, t);
    world.ApplyStructuralChanges();
    undo.CaptureAfter(*ctx.scene, fid);
    undo.EndRecord(selection);
    selection.SelectOnly(fid);
    MYE_LOG_INFO("attached script '%s'", className.c_str());
    return true;
}

ImportResult ImportExternalPaths(EngineContext& ctx, const std::vector<std::wstring>& srcs,
                                 const std::wstring& destDir)
{
    ImportResult result;
    std::error_code ec;
    if (!fs::is_directory(destDir, ec)) {
        MYE_LOG_ERROR("import: destination is not a folder: %s", WideToUtf8(destDir).c_str());
        result.failed = static_cast<int>(srcs.size());
        return result;
    }
    const std::wstring destKey = NormalizePathKey(destDir);
    for (const std::wstring& src : srcs) {
        const fs::path srcPath{ src };
        if (!fs::exists(srcPath, ec)) {
            MYE_LOG_ERROR("import: source not found: %s", WideToUtf8(src).c_str());
            result.failed++;
            continue;
        }
        if (fs::is_directory(srcPath, ec)) {
            // 自分自身/自分の子孫フォルダへのドロップは無限再帰コピーになるため拒否
            const std::wstring srcKey = NormalizePathKey(src);
            if (destKey == srcKey || destKey.rfind(srcKey + L"\\", 0) == 0) {
                MYE_LOG_WARN("import: cannot copy folder into itself: %s", WideToUtf8(src).c_str());
                result.skipped++;
                continue;
            }
            const std::wstring dst = MakeUniquePath(destDir, srcPath.filename().wstring(), true);
            CopyDirRecursive(ctx, srcPath, dst, result);
        } else {
            if (ShouldSkipSource(srcPath)) {
                result.skipped++;
                continue;
            }
            // 表示中フォルダ内のファイルをそのまま落とした場合は no-op (誤複製防止)
            if (NormalizePathKey(srcPath.parent_path().wstring()) == destKey) {
                result.skipped++;
                continue;
            }
            const std::wstring dst = MakeUniquePath(destDir, srcPath.filename().wstring(), false);
            ec.clear();
            if (fs::copy_file(srcPath, dst, ec) && !ec) {
                RegisterImported(ctx, dst);
                result.imported++;
            } else {
                MYE_LOG_ERROR("import: copy failed: %s (%s)", WideToUtf8(src).c_str(),
                              ec.message().c_str());
                result.failed++;
            }
        }
    }
    MYE_LOG_INFO("[import] %d file(s) -> %s (%d skipped, %d failed)", result.imported,
                 WideToUtf8(destDir).c_str(), result.skipped, result.failed);
    return result;
}

namespace {

// fs::rename + .meta 同伴 + assetDb テーブル更新 (移動/リネーム共通の後段、M30b/M30d)。
// .meta の同伴が GUID 永続 (= シーン参照維持) の核
bool PerformAssetRelocate(EngineContext& ctx, const std::wstring& src, const std::wstring& dst,
                          bool isDir)
{
    std::error_code ec;
    fs::rename(src, dst, ec);
    if (ec) {
        MYE_LOG_ERROR("[assets] relocate failed: %s -> %s (%s)", WideToUtf8(src).c_str(),
                      WideToUtf8(dst).c_str(), ec.message().c_str());
        return false;
    }
    // フォルダは配下の .meta ごと rename 済みなので個別移動は不要
    if (!isDir) {
        const std::wstring srcMeta = src + L".meta";
        if (fs::exists(srcMeta, ec)) {
            std::error_code mec;
            fs::rename(srcMeta, dst + L".meta", mec);
            if (mec) {
                // 本体は移動済み。.meta が残ると次回スキャンで新 GUID 採番になるだけ (非致命)
                MYE_LOG_WARN("[assets] failed to move .meta for %s (%s)",
                             WideToUtf8(src).c_str(), mec.message().c_str());
            }
        }
    }
    if (ctx.assetDb) {
        ctx.assetDb->MoveAsset(src, dst); // 実行時テーブルの旧キー除去 + 新パス再登録
    }
    MYE_LOG_INFO("[assets] moved: %s -> %s", WideToUtf8(src).c_str(), WideToUtf8(dst).c_str());
    return true;
}

} // namespace

std::wstring MoveAssetToFolder(EngineContext& ctx, const std::wstring& srcPath,
                               const std::wstring& destDir)
{
    std::error_code ec;
    const fs::path src{ srcPath };
    if (!fs::exists(src, ec) || !fs::is_directory(destDir, ec)
        || AssetDatabase::IsMetaPath(srcPath)) {
        return {};
    }
    const std::wstring destKey = NormalizePathKey(destDir);
    if (NormalizePathKey(src.parent_path().wstring()) == destKey) {
        return {}; // 同一フォルダへの移動 = no-op
    }
    const bool isDir = fs::is_directory(src, ec);
    if (isDir) {
        // 自分自身/自分の子孫への移動は不可 (ImportExternalPaths と同じ判定)
        const std::wstring srcKey = NormalizePathKey(srcPath);
        if (destKey == srcKey || destKey.rfind(srcKey + L"\\", 0) == 0) {
            MYE_LOG_WARN("[assets] cannot move folder into itself: %s",
                         WideToUtf8(srcPath).c_str());
            return {};
        }
    }
    const std::wstring dst = MakeUniquePath(destDir, src.filename().wstring(), isDir);
    return PerformAssetRelocate(ctx, srcPath, dst, isDir) ? dst : std::wstring();
}

std::wstring RenameAsset(EngineContext& ctx, const std::wstring& srcPath,
                         const std::string& newName)
{
    std::error_code ec;
    const fs::path src{ srcPath };
    if (newName.empty() || !fs::exists(src, ec) || AssetDatabase::IsMetaPath(srcPath)) {
        return {};
    }
    const std::wstring newNameW = Utf8ToWide(newName);
    if (newNameW.empty() || newNameW.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
        MYE_LOG_WARN("[assets] rename: invalid name: %s", newName.c_str());
        return {};
    }
    const bool isDir = fs::is_directory(src, ec);
    std::wstring newFilename;
    if (isDir) {
        newFilename = newNameW;
    } else {
        // 拡張子/複合サフィックスは維持し、stem だけを差し替える (Unity 同様)
        std::wstring stem;
        std::wstring suffix;
        SplitAssetName(src.filename().wstring(), stem, suffix);
        newFilename = newNameW + suffix;
    }
    if (newFilename == src.filename().wstring()) {
        return {}; // 変更なし = no-op
    }
    const std::wstring parent = src.parent_path().wstring();
    const std::wstring dst = MakeUniquePath(parent, newFilename, isDir);
    return PerformAssetRelocate(ctx, srcPath, dst, isDir) ? dst : std::wstring();
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
