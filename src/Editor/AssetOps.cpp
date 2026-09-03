#include "Editor/AssetOps.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include <Windows.h>
#include <shellapi.h>
#include <shobjidl.h> // IFileOperation (ごみ箱削除、M51i)
#include <wrl/client.h>

#include "nlohmann/json.hpp"

#include "Editor/Selection.h"
#include "Editor/SourceControl/ScmHint.h" // M66i: 生成 / 移動 / 削除の直後に status を取り直させる
#include "Editor/Undo/UndoStack.h"
#include "Engine/Core/Localization.h"
#include "Engine/Core/ComponentRegistry.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/NameUtil.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Animation.h"
#include "Engine/Engine/EntityNaming.h"
#include "Engine/Engine/Asset/TerrainAsset.h" // M58f: 地形ブラシの Undo/Redo
#include "Engine/Engine/Asset/TerrainEdit.h"
#include "Engine/Engine/AssetDatabase.h"
#include "Engine/Engine/Audio/AudioMixer.h"
#include "Engine/Engine/Audio/AudioSystem.h"
#include "Engine/Engine/Audio/SoundAsset.h"
#include "Engine/Engine/EngineLoop.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/FbxLoader.h"
#include "Engine/Engine/ModelLoader.h"
#include "Engine/Engine/Physics/PhysMatLibrary.h" // M59a1: .physmat.json の生成/登録
#include "Engine/Engine/Prefab.h"
#include "Engine/Engine/RenderSystem.h" // M58f: 地形ブラシ Undo 後のチャンク再構築
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/SchemaCodegen.h"
#include "Engine/Engine/Script/ManagedHost.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GpuResources.h"

namespace fs = std::filesystem;
using namespace DirectX;

namespace mye {
namespace {

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

// 拡張子判定用の小文字化 (ASCII のみ)。DCC 出力に多い ".FBX" 等の大文字拡張子を取りこぼさない
std::string LowerAscii(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

// C++ スクリプトを置くツリーのルート。
//   プロジェクト起動 = <project> (Hub から開いた通常の動線)
//   レガシー起動      = エンジンリポジトリ (assets\ の親。replay_verify / selftest の経路)
// 旧 RepoRoot() は常に「assets\ の親」を返していたため、プロジェクト起動時に
// <project>\src\GameLogic\Scripts へ書いた上で <project>\tools\build_scripts.bat を
// 探しに行き、ビルドできないまま無言で失敗していた
std::wstring ScriptsRoot(EngineContext& ctx)
{
    if (!ctx.projectRoot.empty()) {
        return ctx.projectRoot;
    }
    return fs::path(ctx.assetsRoot).parent_path().wstring();
}

// cmd.exe は .bat をコンソール ANSI コードページで読むので UTF-8 では書けない。
// 日本語を含むプロジェクトパスを通すためにここだけ CP_ACP へ変換する
std::string WideToAcp(const std::wstring& w)
{
    if (w.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_ACP, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0) {
        return {};
    }
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_ACP, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr,
                        nullptr);
    return s;
}

// 実行中の構成を exe パスから判定 (bin\x64\<Config>\)。構成マクロ分岐は決定論ルールで不可
bool RunningRelease()
{
    const std::wstring exeDir = GetExecutableDir();
    return exeDir.find(L"Release") != std::wstring::npos
        || exeDir.find(L"release") != std::wstring::npos;
}

// ---- 外部ファイルインポート (エクスプローラー D&D) のヘルパー ----

} // namespace

// 既知の複合サフィックス (.scene.json 等) を保ったままファイル名を stem/suffix に分割する。
// 素朴な extension() 分割だと連番付与で "x.prefab (1).json" になり種別判定が壊れる
void SplitAssetName(const std::wstring& filename, std::wstring& stem, std::wstring& suffix)
{
    static const std::wstring kCompound[] = {L".scene.json", L".prefab.json", L".actor.json",
                                             L".anim.json",  L".mat.json",   L".controller.json",
                                             L".sound.json", L".mixer.json", L".physmat.json"};
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

// destDir 直下で衝突しない絶対パスを返す ("name.ext" → "name (1).ext" → ...)。
// 連番規則そのものは nameutil::MakeUniqueNumbered に集約されており、
// エンティティの兄弟名一意化 (MakeUniqueSiblingName) と同じ規則を共有する (M48b)
std::wstring MakeUniquePath(const std::wstring& destDir, const std::wstring& filename, bool isDir)
{
    std::wstring stem = filename;
    std::wstring suffix;
    if (!isDir) {
        SplitAssetName(filename, stem, suffix); // 複合サフィックスはファイル名固有 (移設しない)
    }
    std::error_code ec;
    const std::wstring name = nameutil::MakeUniqueNumbered<wchar_t>(
        stem, suffix, /*budget=*/0,
        [&](const std::wstring& c) { return fs::exists(destDir + L"\\" + c, ec); });
    return destDir + L"\\" + name;
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
    // インポート / 複製の唯一の着地点なので、Source Control のヒントもここで出す
    // (M66i)。数百ファイルの一括インポートでも、セッション側が 1 往復にまとめる
    scmhint::Changed(destPath);
}

// フォルダを再帰コピー。fs::copy(recursive) ではなくファイル単位で回すことで、
// 途中エラーでも継続でき、.meta 除外と件数計上を同時に行える
void CopyDirRecursive(EngineContext& ctx, const fs::path& srcDir, const fs::path& dstDir,
                      ImportResult& result)
{
    std::error_code ec;
    fs::create_directories(dstDir, ec);
    if (!fs::is_directory(dstDir, ec)) {
        MYE_LOG_ERROR(Tr(StrId::Log_ImportMkdirFail), WideToUtf8(dstDir.wstring()).c_str());
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
                MYE_LOG_ERROR(Tr(StrId::Log_ImportCopyFail),
                              WideToUtf8(entry.path().wstring()).c_str(), ec.message().c_str());
                result.failed++;
            }
        }
    }
}

} // namespace

// ファイル名向けサニタイズ。M50b で許可リスト (英数のみ) から禁止リスト
// (\/:*?"<>| + 制御文字) へ緩めた — 非 ASCII (日本語名) のアセット名を通すため。
// 前後の空白と末尾ドット (Windows 不可) を落とし、空になったら fallback
std::string SanitizeFileName(const std::string& in, const char* fallback)
{
    std::string out;
    for (char c : in) {
        const unsigned char u = static_cast<unsigned char>(c);
        const bool bad = u < 0x20 || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?'
            || c == '"' || c == '<' || c == '>' || c == '|';
        if (!bad) {
            out += c;
        }
    }
    const size_t b = out.find_first_not_of(' ');
    const size_t e = out.find_last_not_of(" .");
    if (b == std::string::npos || e == std::string::npos || e < b) {
        return fallback;
    }
    return out.substr(b, e - b + 1);
}

// MakeUniquePath の公開ラッパ (M50b)。Create Prefab (Hierarchy 側) が同名衝突で
// 既存アセットを黙って上書き = パスハッシュ再登録で既存インスタンスが新ベースへ
// 張り替わる事故を防ぐために使う
std::wstring MakeUniqueAssetPath(const std::wstring& destDir, const std::wstring& filename)
{
    return MakeUniquePath(destDir, filename, /*isDir=*/false);
}

std::wstring CreateFolderAsset(const std::wstring& dir, const std::string& name)
{
    const std::wstring p = dir + L"\\" + Utf8ToWide(SanitizeFileName(name, "New Folder"));
    std::error_code ec;
    if (fs::create_directory(p, ec)) {
        MYE_LOG_INFO(Tr(StrId::Log_CreatedFolder), WideToUtf8(p).c_str());
        return p;
    }
    MYE_LOG_WARN(Tr(StrId::Log_MkdirFail), WideToUtf8(p).c_str());
    return {};
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
        MYE_LOG_ERROR(Tr(StrId::Log_WriteSceneFail), WideToUtf8(path).c_str());
        return {};
    }
    f << root.dump(2);
    MYE_LOG_INFO(Tr(StrId::Log_CreatedScene), WideToUtf8(path).c_str());
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
        MYE_LOG_ERROR(Tr(StrId::Log_WriteAnimFail), WideToUtf8(path).c_str());
        return {};
    }
    MYE_LOG_INFO(Tr(StrId::Log_CreatedAnim), WideToUtf8(path).c_str());
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
    root["emissive"] = 0.0; // M46i: 自己発光の強さ (0 = 発光なし)
    root["texture"] = "";   // assets ルート相対パス (空 = 白テクスチャ)
    root["normalMap"] = ""; // 空 = ノーマルマップなし
    root["transparent"] = false;
    std::ofstream f{ fs::path(path) };
    if (!f) {
        MYE_LOG_ERROR(Tr(StrId::Log_WriteMatFail), WideToUtf8(path).c_str());
        return {};
    }
    f << root.dump(2);
    f.close();
    // 生成直後に登録 → 参照ピッカー / ダブルクリック割り当てで即使える
    if (ctx.resources) {
        ctx.resources->materials.LoadFromFile(path, ctx.resources->textures, ctx.assetsRoot);
    }
    MYE_LOG_INFO(Tr(StrId::Log_CreatedMat), WideToUtf8(path).c_str());
    return path;
}

std::wstring CreateSoundAsset(EngineContext& ctx, const std::wstring& dir, const std::string& name)
{
    const std::string safe = SanitizeFileName(name, "New Sound");
    const std::wstring path = dir + L"\\" + Utf8ToWide(safe) + L".sound.json";
    SoundAsset s;
    s.name = safe;
    s.variations.push_back(SoundVariation{}); // 空スロットを 1 本 (Inspector で clip を選ぶ)
    std::ofstream f{ fs::path(path), std::ios::binary };
    if (!f) {
        MYE_LOG_ERROR(Tr(StrId::Log_WriteSoundFail), WideToUtf8(path).c_str());
        return {};
    }
    f << SoundLibrary::ToJson(s).dump(2);
    f.close();
    // 生成直後に登録 → 参照ピッカー / ダブルクリック試聴で即使える (CreateMaterialAsset 範型)
    if (ctx.sounds) {
        ctx.sounds->LoadFromFile(path);
    }
    MYE_LOG_INFO(Tr(StrId::Log_CreatedSound), WideToUtf8(path).c_str());
    return path;
}

std::wstring CreateActorAsset(EngineContext& ctx, const std::wstring& dir, const std::string& name)
{
    const std::string safe = SanitizeFileName(name, "New Actor");
    // 同名は " (1)" 連番 (M50b)。上書きするとパスハッシュ再登録で既存インスタンスが
    // 空のベースへ黙って張り替わるため、ここの一意化は機能ではなく安全装置
    const std::wstring path =
        MakeUniqueAssetPath(dir, Utf8ToWide(safe) + PrefabLibrary::kActorSuffix);
    // ルート 1 個の最小構成。エンティティ形式は WriteEntity 互換 (fileId=1 / 親なし)
    nlohmann::json rootEntity;
    rootEntity["fileId"] = 1;
    rootEntity["name"] = safe;
    rootEntity["childIndex"] = 0;
    rootEntity["components"] = nlohmann::json::object();
    nlohmann::json doc;
    doc["engine"] = "MyEngine";
    doc["actor"] = 1;
    doc["name"] = safe;
    doc["entities"] = nlohmann::json::array({ rootEntity });

    std::ofstream f{ fs::path(path), std::ios::binary };
    if (!f) {
        MYE_LOG_ERROR(Tr(StrId::Log_WriteActorFail), WideToUtf8(path).c_str());
        return {};
    }
    f << doc.dump(2);
    f.close();
    // 生成直後に登録 → ダブルクリック / D&D で即配置できる (CreateSoundAsset 範型)
    if (ctx.prefabs) {
        ctx.prefabs->LoadFromFile(path);
    }
    MYE_LOG_INFO(Tr(StrId::Log_CreatedActor), WideToUtf8(path).c_str());
    return path;
}

std::wstring CreateMixerAsset(EngineContext& ctx, const std::wstring& dir, const std::string& name)
{
    const std::string safe = SanitizeFileName(name, "New Mixer");
    const std::wstring path = dir + L"\\" + Utf8ToWide(safe) + L".mixer.json";
    MixerAsset m = DefaultMixer(); // Master / BGM / SE / UI
    m.name = safe;
    std::ofstream f{ fs::path(path), std::ios::binary };
    if (!f) {
        MYE_LOG_ERROR(Tr(StrId::Log_WriteMixerFail), WideToUtf8(path).c_str());
        return {};
    }
    f << MixerLibrary::ToJson(m).dump(2);
    f.close();
    // 生成直後に登録 → Audio Mixer 窓のアセット一覧にそのまま出る (CreateSoundAsset 範型)。
    // **アクティブの切り替えはしない** — 作った瞬間に鳴っているバス構成が変わると事故る
    if (ctx.mixers) {
        ctx.mixers->LoadFromFile(path);
    }
    MYE_LOG_INFO(Tr(StrId::Log_CreatedMixer), WideToUtf8(path).c_str());
    return path;
}

std::wstring CreatePhysMatAsset(EngineContext& ctx, const std::wstring& dir, const std::string& name)
{
    (void)ctx; // 署名は他の Create* と揃える (InstantiateAssetAtPath 経由の互換。AssetOps.h 注記)
    const std::string safe = SanitizeFileName(name, "New PhysMat");
    const std::wstring path = dir + L"\\" + Utf8ToWide(safe) + L".physmat.json";
    PhysMat m;
    m.name = safe;
    std::ofstream f{ fs::path(path), std::ios::binary };
    if (!f) {
        MYE_LOG_ERROR(Tr(StrId::Log_WritePhysMatFail), WideToUtf8(path).c_str());
        return {};
    }
    f << PhysMatLibrary::ToJson(m).dump(2);
    f.close();
    // 生成直後に登録 → 参照ピッカー (M59a2 の Collider.physMaterial) で即使える
    if (PhysMatLibrary* pm = physmat::Library()) {
        pm->LoadFromFile(path);
    }
    MYE_LOG_INFO(Tr(StrId::Log_CreatedPhysMat), WideToUtf8(path).c_str());
    return path;
}

std::wstring CreateCppScript(EngineContext& ctx, const std::string& rawName)
{
    const std::string name = SanitizeIdentifier(rawName);
    const std::wstring dir = ScriptsRoot(ctx) + L"\\src\\GameLogic\\Scripts";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::wstring path = dir + L"\\" + Utf8ToWide(name) + L".cpp";
    if (fs::exists(path)) {
        MYE_LOG_WARN(Tr(StrId::Log_ScriptExists), WideToUtf8(path).c_str());
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
        MYE_LOG_ERROR(Tr(StrId::Log_WriteScriptFail), WideToUtf8(path).c_str());
        return {};
    }
    f << t;
    MYE_LOG_INFO(Tr(StrId::Log_CreatedCpp), WideToUtf8(path).c_str());
    MYE_LOG_INFO(Tr(StrId::Log_HintRebuildCpp));
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
        MYE_LOG_WARN(Tr(StrId::Log_CsExists), WideToUtf8(path).c_str());
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
        MYE_LOG_ERROR(Tr(StrId::Log_WriteCsFail), WideToUtf8(path).c_str());
        return {};
    }
    f << t;
    MYE_LOG_INFO(Tr(StrId::Log_CreatedCs), WideToUtf8(path).c_str());
    MYE_LOG_INFO(Tr(StrId::Log_HintCompileCs));
    return path;
}

void CompileCSharpScripts(EngineContext& ctx)
{
    if (!ctx.managedHost) {
        MYE_LOG_WARN(Tr(StrId::Log_CsHostMissing));
        return;
    }
    if (!ctx.managedHost->IsReady()) {
        MYE_LOG_WARN(Tr(StrId::Log_CsHostNotReady));
        return;
    }
    MYE_LOG_INFO(Tr(StrId::Log_CsCompiling));
    // スキーマ定数/アクセサを生成してからコンパイル (M50d)。Compile は assets\scripts を
    // 再帰収集するので Generated\ は追加設定ゼロで混ざる
    schema::WriteCSharpBindings(ctx.assetsRoot);
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
        MYE_LOG_WARN(Tr(StrId::Log_ScriptDup), className.c_str());
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
    MYE_LOG_INFO(Tr(StrId::Log_ScriptAttached), className.c_str());
    return true;
}

bool AssignMaterialToEntity(EngineContext& ctx, Selection& selection, UndoStack& undo,
                            const std::wstring& matPath, EntityID target)
{
    World& world = ctx.scene->GetWorld();
    if (!world.IsAlive(target)) {
        return false;
    }
    const AssetID id =
        ctx.resources->materials.LoadFromFile(matPath, ctx.resources->textures, ctx.assetsRoot);
    if (id.IsNull()) {
        MYE_LOG_WARN(Tr(StrId::Log_MatLoadFail), WideToUtf8(matPath).c_str());
        return false;
    }
    auto* mr = world.GetComponent<MeshRendererComponent>(target);
    if (!mr) {
        MYE_LOG_WARN(Tr(StrId::Log_NoMeshRenderer));
        return false;
    }
    // AssetBrowser ダブルクリック割当と同じ 1 Undo エントリ (選択は変えない)
    const uint64_t fid = ctx.scene->EnsureFileId(target);
    undo.BeginRecord("Assign Material", selection);
    undo.CaptureBefore(*ctx.scene, fid);
    mr->material = id;
    undo.CaptureAfter(*ctx.scene, fid);
    undo.EndRecord(selection);
    return true;
}

ImportResult ImportExternalPaths(EngineContext& ctx, const std::vector<std::wstring>& srcs,
                                 const std::wstring& destDir)
{
    ImportResult result;
    std::error_code ec;
    if (!fs::is_directory(destDir, ec)) {
        MYE_LOG_ERROR(Tr(StrId::Log_ImportNotFolder), WideToUtf8(destDir).c_str());
        result.failed = static_cast<int>(srcs.size());
        return result;
    }
    const std::wstring destKey = NormalizePathKey(destDir);
    for (const std::wstring& src : srcs) {
        const fs::path srcPath{ src };
        if (!fs::exists(srcPath, ec)) {
            MYE_LOG_ERROR(Tr(StrId::Log_ImportNoSource), WideToUtf8(src).c_str());
            result.failed++;
            continue;
        }
        if (fs::is_directory(srcPath, ec)) {
            // 自分自身/自分の子孫フォルダへのドロップは無限再帰コピーになるため拒否
            const std::wstring srcKey = NormalizePathKey(src);
            if (destKey == srcKey || destKey.rfind(srcKey + L"\\", 0) == 0) {
                MYE_LOG_WARN(Tr(StrId::Log_ImportSelf), WideToUtf8(src).c_str());
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
                MYE_LOG_ERROR(Tr(StrId::Log_ImportCopyFail), WideToUtf8(src).c_str(),
                              ec.message().c_str());
                result.failed++;
            }
        }
    }
    MYE_LOG_INFO(Tr(StrId::Log_ImportDone), result.imported,
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
        MYE_LOG_ERROR(Tr(StrId::Log_RelocateFail), WideToUtf8(src).c_str(),
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
                MYE_LOG_WARN(Tr(StrId::Log_MetaMoveFail),
                             WideToUtf8(src).c_str(), mec.message().c_str());
            }
        }
    }
    if (ctx.assetDb) {
        ctx.assetDb->MoveAsset(src, dst); // 実行時テーブルの旧キー除去 + 新パス再登録
    }
    // ★**移動元と移動先の両方**を出す (M66i)。git から見るとリネームは
    //   「旧パスの削除 + 新パスの追加」なので、片方だけだと元の場所のバッジが残る
    scmhint::Changed(src);
    scmhint::Changed(dst);
    MYE_LOG_INFO(Tr(StrId::Log_Relocated), WideToUtf8(src).c_str(), WideToUtf8(dst).c_str());
    return true;
}

// リネーム/移動成功後の Relocate エントリ記録 (M51i)。Undo が pathB→pathA、Redo が
// pathA→pathB を PerformAssetRelocate で再実行する (実行部は ExecuteAssetFileOp)
void PushRelocateOp(UndoStack* undo, const char* label, const std::wstring& src,
                    const std::wstring& dst, bool isDir)
{
    if (!undo) {
        return;
    }
    UndoFileOp op;
    op.kind = UndoFileOp::Kind::Relocate;
    op.pathA = src;
    op.pathB = dst;
    op.isDir = isDir;
    undo->PushFileOp(label, std::move(op));
}

} // namespace

std::wstring MoveAssetToFolder(EngineContext& ctx, const std::wstring& srcPath,
                               const std::wstring& destDir, UndoStack* undo)
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
            MYE_LOG_WARN(Tr(StrId::Log_MoveSelf),
                         WideToUtf8(srcPath).c_str());
            return {};
        }
    }
    const std::wstring dst = MakeUniquePath(destDir, src.filename().wstring(), isDir);
    if (!PerformAssetRelocate(ctx, srcPath, dst, isDir)) {
        return {};
    }
    PushRelocateOp(undo, "Move Asset", srcPath, dst, isDir);
    return dst;
}

std::wstring RenameAsset(EngineContext& ctx, const std::wstring& srcPath,
                         const std::string& newName, UndoStack* undo)
{
    std::error_code ec;
    const fs::path src{ srcPath };
    if (newName.empty() || !fs::exists(src, ec) || AssetDatabase::IsMetaPath(srcPath)) {
        return {};
    }
    const std::wstring newNameW = Utf8ToWide(newName);
    if (newNameW.empty() || newNameW.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
        MYE_LOG_WARN(Tr(StrId::Log_RenameInvalid), newName.c_str());
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
    if (!PerformAssetRelocate(ctx, srcPath, dst, isDir)) {
        return {};
    }
    PushRelocateOp(undo, "Rename Asset", srcPath, dst, isDir);
    return dst;
}

namespace {

// パス群をごみ箱へ移動する (FOF_ALLOWUNDO、M51i)。IFileOperation を使う —
// SHFileOperationW (ProjectManager) と違い長パスに強く、複数項目 (本体 + .meta) を
// 1 操作に束ねられる (ごみ箱の「元に戻す」も 1 回で済む)。
// エディタ main スレッドは COM 未初期化なので都度初期化する。既に別モデルで初期化済み
// (RPC_E_CHANGED_MODE) ならそのまま続行し、対応する Uninitialize もしない
bool RecycleToBin(const std::vector<std::wstring>& paths)
{
    if (paths.empty()) {
        return true;
    }
    const HRESULT co =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool ok = false;
    {
        Microsoft::WRL::ComPtr<IFileOperation> op;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
                                       IID_PPV_ARGS(&op)))) {
            // ヘッドレス (selftest) でも止まらないよう確認/エラー UI は全部抑止する
            op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT
                                  | FOF_NOERRORUI);
            bool queued = false;
            for (const std::wstring& p : paths) {
                Microsoft::WRL::ComPtr<IShellItem> item;
                if (SUCCEEDED(SHCreateItemFromParsingName(p.c_str(), nullptr,
                                                          IID_PPV_ARGS(&item)))) {
                    queued = SUCCEEDED(op->DeleteItem(item.Get(), nullptr)) || queued;
                }
            }
            if (queued && SUCCEEDED(op->PerformOperations())) {
                BOOL aborted = FALSE;
                op->GetAnyOperationsAborted(&aborted);
                ok = !aborted;
            }
        }
    } // ComPtr は CoUninitialize より先に解放する
    if (co == S_OK || co == S_FALSE) {
        CoUninitialize();
    }
    if (ok) {
        // 削除 (ごみ箱送り) の唯一の実体 (M66i)。本体と `.meta` が同時に来るので、
        // まとめて 1 往復に畳まれる
        for (const std::wstring& p : paths) {
            scmhint::Changed(p);
        }
    }
    return ok;
}

// 複製の実体 (DuplicateAsset と redo が共用、M51i)。**旧 .meta はコピーしない** —
// 複製物には新パスのパスハッシュで新規 GUID を発行する
bool CopyForDuplicate(EngineContext& ctx, const std::wstring& src, const std::wstring& dst,
                      bool isDir)
{
    std::error_code ec;
    if (isDir) {
        ImportResult r;
        CopyDirRecursive(ctx, src, dst, r); // .meta 除外コピー + 1 ファイルずつ新 GUID 登録
        return r.failed == 0;
    }
    if (!fs::copy_file(src, dst, ec) || ec) {
        MYE_LOG_ERROR(Tr(StrId::Log_ImportCopyFail), WideToUtf8(src).c_str(),
                      ec.message().c_str());
        return false;
    }
    // 宛先に孤児 .meta (消えたファイルの残骸) があると EnsureMeta が旧 GUID を「尊重」して
    // 継いでしまう — 死んだ参照が複製物に着地するので、必ず先に除去してから発行する
    fs::remove(dst + L".meta", ec);
    RegisterImported(ctx, dst); // 新パスハッシュ GUID の .meta 発行 + 実行時テーブル反映
    return true;
}

} // namespace

bool DeleteAssetToRecycleBin(EngineContext& ctx, const std::wstring& path)
{
    std::error_code ec;
    if (!fs::exists(path, ec) || AssetDatabase::IsMetaPath(path)) {
        return false;
    }
    std::vector<std::wstring> targets{ path };
    if (!fs::is_directory(path, ec)) {
        const std::wstring meta = path + L".meta";
        if (fs::exists(meta, ec)) {
            targets.push_back(meta); // GUID サイドカーも同伴 (孤児 .meta を残さない)
        }
    } // フォルダは配下の .meta ごと 1 項目で移動する
    if (!RecycleToBin(targets)) {
        MYE_LOG_ERROR(Tr(StrId::Log_DeleteFail), WideToUtf8(path).c_str());
        return false;
    }
    if (ctx.assetDb) {
        // 旧キーの除去だけが起きる — 新パス (= 同じパス) はもう存在しないので
        // MoveAsset の再登録フェーズは何もしない
        ctx.assetDb->MoveAsset(path, path);
    }
    MYE_LOG_INFO(Tr(StrId::Log_DeletedToBin), WideToUtf8(path).c_str());
    return true;
}

std::wstring DuplicateAsset(EngineContext& ctx, const std::wstring& srcPath, UndoStack* undo)
{
    std::error_code ec;
    const fs::path src{ srcPath };
    if (!fs::exists(src, ec) || AssetDatabase::IsMetaPath(srcPath)) {
        return {};
    }
    const bool isDir = fs::is_directory(src, ec);
    const std::wstring dst =
        MakeUniquePath(src.parent_path().wstring(), src.filename().wstring(), isDir);
    if (!CopyForDuplicate(ctx, srcPath, dst, isDir)) {
        return {};
    }
    if (undo) {
        UndoFileOp op;
        op.kind = UndoFileOp::Kind::Duplicate;
        op.pathA = srcPath;
        op.pathB = dst;
        op.isDir = isDir;
        undo->PushFileOp("Duplicate Asset", std::move(op));
    }
    MYE_LOG_INFO(Tr(StrId::Log_Duplicated), WideToUtf8(srcPath).c_str(),
                 WideToUtf8(dst).c_str());
    return dst;
}

void RecordAssetCreated(UndoStack& undo, const std::wstring& path)
{
    std::error_code ec;
    if (path.empty() || !fs::exists(path, ec)) {
        return;
    }
    UndoFileOp op;
    op.kind = UndoFileOp::Kind::Create;
    op.pathB = path;
    op.isDir = fs::is_directory(path, ec);
    if (!op.isDir) {
        // redo 用の内容スナップショット (Create 直後の小さな JSON 雛形)
        std::ifstream f{ fs::path(path), std::ios::binary };
        op.bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    undo.PushFileOp("Create Asset", std::move(op));
}

namespace {

// 地形ブラシ 1 ストロークの逆/順適用 (M58f)。
// ★**ブラシ本体とまったく同じ永続化経路 (TerrainEdit::SaveEdits) を通す。**
//   「塗るときは A、戻すときは B」と分けると、クックキャッシュの更新漏れが
//   どちらか一方でだけ起きて「Undo したのに絵が戻らない」になる。
//   ディスク上のサイドカーが唯一の真値なので、毎回そこから読み直して当て直す
bool ApplyTerrainPaintOp(EngineContext& ctx, const UndoFileOp& op, bool redo)
{
    std::error_code ec;
    if (op.pathA.empty() || !fs::exists(op.pathA, ec)) {
        MYE_LOG_WARN(Tr(StrId::Log_UndoTargetGone), WideToUtf8(op.pathA).c_str());
        return false;
    }
    TerrainEdit::TerrainPatch patch;
    if (!TerrainEdit::DeserializePatch(op.bytes, patch)) {
        MYE_LOG_WARN("terrain undo: the recorded patch is malformed");
        return false;
    }
    TerrainAsset::TerrainData data;
    if (!TerrainAsset::Load(op.pathA, data)) {
        MYE_LOG_WARN(Tr(StrId::Log_UndoTargetGone), WideToUtf8(op.pathA).c_str());
        return false;
    }
    // 解像度が変わっている (JSON の heightRes を触った) 等でパッチが当たらないときは
    // 何も書かない — 半分だけ巻き戻った地形を残さない
    if (!TerrainEdit::ApplyPatch(data, patch, redo)) {
        MYE_LOG_WARN(Tr(StrId::Terrain_UndoStale), WideToUtf8(op.pathA).c_str());
        return false;
    }
    if (!TerrainEdit::SaveEdits(op.pathA, data)) {
        return false;
    }
    if (ctx.renderSystem != nullptr) {
        ctx.renderSystem->InvalidateTerrain(); // 次フレームでチャンクを焼き直す
    }
    return true;
}

} // namespace

// UndoStack のファイル操作エントリ実行部 (M51i)。宣言は UndoStack.h。
// 「逆操作先消滅時は WARN + no-op」— Explorer 側でファイルが動いた後の Undo で
// 何かを上書きしたり例外で落ちたりしないことを最優先にする
bool ExecuteAssetFileOp(EngineContext* ctx, const UndoFileOp& op, bool redo)
{
    if (!ctx) {
        MYE_LOG_WARN("asset file op skipped: no engine context (SetFileOpContext missing)");
        return false;
    }
    std::error_code ec;
    switch (op.kind) {
    case UndoFileOp::Kind::Relocate: {
        const std::wstring& from = redo ? op.pathA : op.pathB;
        const std::wstring& to = redo ? op.pathB : op.pathA;
        if (!fs::exists(from, ec)) {
            MYE_LOG_WARN(Tr(StrId::Log_UndoTargetGone), WideToUtf8(from).c_str());
            return false;
        }
        if (fs::exists(to, ec)) {
            MYE_LOG_WARN(Tr(StrId::Log_UndoDestBlocked), WideToUtf8(to).c_str());
            return false;
        }
        return PerformAssetRelocate(*ctx, from, to, op.isDir);
    }
    case UndoFileOp::Kind::Duplicate:
        if (!redo) {
            if (!fs::exists(op.pathB, ec)) {
                MYE_LOG_WARN(Tr(StrId::Log_UndoTargetGone), WideToUtf8(op.pathB).c_str());
                return false;
            }
            return DeleteAssetToRecycleBin(*ctx, op.pathB); // 複製物をごみ箱へ
        }
        if (!fs::exists(op.pathA, ec)) {
            MYE_LOG_WARN(Tr(StrId::Log_UndoTargetGone), WideToUtf8(op.pathA).c_str());
            return false;
        }
        if (fs::exists(op.pathB, ec)) {
            MYE_LOG_WARN(Tr(StrId::Log_UndoDestBlocked), WideToUtf8(op.pathB).c_str());
            return false;
        }
        // 再複製。GUID は新パスのパスハッシュで再発行 = undo 前と同じ値に戻る (パス不変)
        return CopyForDuplicate(*ctx, op.pathA, op.pathB, op.isDir);
    case UndoFileOp::Kind::Create:
        if (!redo) {
            if (!fs::exists(op.pathB, ec)) {
                MYE_LOG_WARN(Tr(StrId::Log_UndoTargetGone), WideToUtf8(op.pathB).c_str());
                return false;
            }
            return DeleteAssetToRecycleBin(*ctx, op.pathB); // 生成物をごみ箱へ
        }
        if (fs::exists(op.pathB, ec)) {
            MYE_LOG_WARN(Tr(StrId::Log_UndoDestBlocked), WideToUtf8(op.pathB).c_str());
            return false;
        }
        if (op.isDir) {
            return fs::create_directories(op.pathB, ec) && !ec;
        }
        {
            // 内容スナップショットを書き戻すだけ。ライブラリ登録 (anims 等) はしない —
            // 参照時の LoadFromFile で遅延解決される。.meta も作らない (元の Create も
            // 作っておらず、次回スキャン / GuidForPath が同じパスハッシュ GUID を発行する)
            std::ofstream f{ fs::path(op.pathB), std::ios::binary };
            if (!f) {
                MYE_LOG_ERROR(Tr(StrId::Log_WriteFail), WideToUtf8(op.pathB).c_str());
                return false;
            }
            f.write(op.bytes.data(), static_cast<std::streamsize>(op.bytes.size()));
            return true;
        }
    case UndoFileOp::Kind::TerrainPaint:
        return ApplyTerrainPaintOp(*ctx, op, redo);
    case UndoFileOp::Kind::None:
    default:
        return false;
    }
}

void InstantiateAssetAtPath(EngineContext& ctx, Selection& selection, UndoStack& undo,
                            const std::wstring& path, const XMFLOAT3* pos, uint64_t parentFileId)
{
    if (!ctx.scene) {
        return;
    }
    const std::string u = WideToUtf8(path);
    const std::string lu = LowerAscii(u); // 拡張子判定は大文字小文字を無視する
    const AssetType dropType = AssetDatabase::ClassifyPath(path);
    undo.BeginRecord("Place Asset", selection);
    uint64_t rootFid = 0;
    if (PrefabLibrary::IsComposePath(path)) { // .actor.json / .prefab.json (M48d)
        const uint64_t hash = ctx.prefabs ? ctx.prefabs->LoadFromFile(path) : 0;
        if (hash != 0) {
            rootFid = Prefab::Instantiate(*ctx.scene, *ctx.prefabs, hash, parentFileId);
            ctx.scene->GetWorld().ApplyStructuralChanges();
        }
    } else if (EndsWith(lu, ".glb") || EndsWith(lu, ".gltf") || EndsWith(lu, ".fbx")) {
        GameObject o = EndsWith(lu, ".fbx")
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
    } else if (dropType == AssetType::Texture) {
        // 画像 → SpriteRenderer 付きオブジェクトとして配置。テクスチャは GUID 安定 ID で
        // 非同期ロード。注: 再起動後はシーン参照だけではロードされず、サムネイル等が
        // RequestLoad するまで白 (UIElementComponent.texture と同じ既存挙動)
        const std::string stem = WideToUtf8(fs::path(path).stem().wstring());
        GameObject o = ctx.scene->CreateGameObjectTracked(stem.empty() ? "Sprite" : stem.c_str());
        auto* sp = o.AddComponent<SpriteRendererComponent>();
        sp->texture = ctx.resources->textures.RequestLoadFileAsync(path);
        if (Texture* t = ctx.resources->textures.Get(sp->texture)) {
            if (t->width > 0 && t->height > 0) {
                // サムネイル等でロード済みなら縦横比を size に反映 (高さ 1 基準)
                sp->size.x = static_cast<float>(t->width) / static_cast<float>(t->height);
            }
        }
        ctx.scene->GetWorld().ApplyStructuralChanges();
        if (parentFileId != 0) {
            GameObject par = ctx.scene->FindByFileId(parentFileId);
            if (par) {
                o.SetParent(par);
                ctx.scene->GetWorld().ApplyStructuralChanges();
            }
        }
        rootFid = ctx.scene->EnsureFileId(o.Id());
    } else if (dropType == AssetType::Sound || dropType == AssetType::Audio) {
        // M45e: 音 → AudioSource 付きオブジェクト (be8e158 の「画像 → SpriteRenderer」と同型)。
        // AudioSource が参照するのは **.sound.json (SoundAsset)** であってクリップではないので、
        // 生の .wav/.ogg を落とされたら隣に .sound.json を 1 本作ってそれを指す —
        // 「ドロップしたのに何も鳴らない AudioSource」を作らないため
        uint64_t soundHash = 0;
        if (ctx.sounds != nullptr) {
            if (dropType == AssetType::Sound) {
                soundHash = ctx.sounds->LoadFromFile(path); // 未登録なら登録 (冪等)
            } else if (ctx.audio != nullptr) {
                const std::wstring dir = fs::path(path).parent_path().wstring();
                const std::string stem = WideToUtf8(fs::path(path).stem().wstring());
                const std::wstring created = CreateSoundAsset(ctx, dir, stem);
                const AssetID clip = ctx.audio->LoadClipFile(path);
                if (!created.empty() && !clip.IsNull()) {
                    const uint64_t h = SoundLibrary::HashForPath(created);
                    if (SoundAsset* s = ctx.sounds->GetMutable(h)) {
                        s->variations[0].clip = clip.value;
                        // 置いた瞬間から 3D で鳴ってほしいので既定を 3D にする
                        // (.sound.json 単体の既定は 2D = 従来の再生と同じ、を保つ)
                        s->spatialBlend = 1.0f;
                        ctx.sounds->SaveToFile(h);
                        soundHash = h;
                    }
                }
            }
        }
        const std::string stem = WideToUtf8(fs::path(path).stem().stem().wstring());
        GameObject o = ctx.scene->CreateGameObjectTracked(stem.empty() ? "Audio Source"
                                                                       : stem.c_str());
        auto* as = o.AddComponent<AudioSourceComponent>();
        as->sound = AssetID{ soundHash };
        ctx.scene->GetWorld().ApplyStructuralChanges();
        if (parentFileId != 0) {
            GameObject par = ctx.scene->FindByFileId(parentFileId);
            if (par) {
                o.SetParent(par);
                ctx.scene->GetWorld().ApplyStructuralChanges();
            }
        }
        rootFid = ctx.scene->EnsureFileId(o.Id());
        if (soundHash == 0) {
            MYE_LOG_WARN(Tr(StrId::Log_PlaceNoSound), u.c_str());
        }
    } else {
        undo.CancelRecord();
        MYE_LOG_WARN("cannot place asset (drag a prefab, model, image, or sound): %s", u.c_str());
        return;
    }
    if (rootFid == 0) {
        undo.CancelRecord();
        MYE_LOG_WARN(Tr(StrId::Log_PlaceFail), u.c_str());
        return;
    }
    if (pos) {
        GameObject r = ctx.scene->FindByFileId(rootFid);
        if (r) {
            r.SetLocalPosition(pos->x, pos->y, pos->z);
        }
    }
    // 兄弟名の一意化 (M48b)。全分岐で ApplyStructuralChanges 済み = root は既に親の子リストに
    // いるので、親は world から引き、**自分自身を exclude** する (でないと必ず " (1)" が付く)。
    // CaptureAfter より前に置くこと (後ろだと Undo/Redo で名前が巻き戻る)
    if (GameObject r = ctx.scene->FindByFileId(rootFid)) {
        World& w = ctx.scene->GetWorld();
        const EntityID re = r.Id();
        if (auto* nc = w.GetComponent<NameComponent>(re)) {
            SetEntityName(w, re,
                          MakeUniqueSiblingName(w, w.GetParent(re), nc->value, /*exclude=*/re));
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

namespace {

// エクスポート glue を <project>\cache\ へ生成する。
// エンジンリポジトリの src\GameLogic\GameLogicMain.cpp と同一内容 — これを生成することで
// プロジェクト側が参照するエンジン資産は Shared\ のヘッダ 4 本だけになる
bool WriteGeneratedMain(const std::wstring& path)
{
    std::ofstream f{ fs::path(path) };
    if (!f) {
        MYE_LOG_ERROR(Tr(StrId::Log_WriteFail), WideToUtf8(path).c_str());
        return false;
    }
    f << "// 自動生成 (MyEngine エディタ)。手編集しても Rebuild Scripts で上書きされる。\n"
         "// エンジンの src\\GameLogic\\GameLogicMain.cpp と同一内容。\n"
         "#include \"Shared/ScriptAPI.h\"\n"
         "\n"
         "extern \"C\" __declspec(dllexport) const MyeScriptModule* GameLogic_GetModule(\n"
         "    const MyeEngineApi* api)\n"
         "{\n"
         "    (void)api;\n"
         "    static MyeScriptModule mod = {};\n"
         "    mod.apiVersion = MYE_API_VERSION;\n"
         "    mod.scripts = mye_script_detail::Registry().data();\n"
         "    mod.scriptCount = static_cast<uint32_t>(mye_script_detail::Registry().size());\n"
         "    return &mod;\n"
         "}\n"
         "\n"
         "extern \"C\" __declspec(dllexport) unsigned int GameLogic_ApiVersion(void)\n"
         "{\n"
         "    return MYE_API_VERSION;\n"
         "}\n";
    return true;
}

// XML 属性値のエスケープ (プロジェクトパスに & や < が混ざっても壊れないように)
std::string XmlEscape(const std::wstring& w)
{
    std::string out;
    for (char c : WideToUtf8(w)) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c; break;
        }
    }
    return out;
}

// <project>\cache\GameLogic.vcxproj を生成する。
// エンジンの build\Common.props を直接 import するので、コンパイラフラグ
// (/std:c++20 /W4 /permissive- /fp:precise /Zi /utf-8 /Zc:preprocessor、構成別 RuntimeLibrary、
// AdditionalIncludeDirectories=$(RepoRoot)src) はエンジン本体と常に一致する。
// import 順は build\GameLogic.vcxproj に厳密に合わせること (Microsoft.Cpp.props の後)
bool WriteGeneratedVcxproj(const std::wstring& path, const std::wstring& engineRepo,
                           const std::wstring& cacheDir,
                           const std::vector<std::wstring>& sources)
{
    std::ofstream f{ fs::path(path), std::ios::binary };
    if (!f) {
        MYE_LOG_ERROR(Tr(StrId::Log_WriteFail), WideToUtf8(path).c_str());
        return false;
    }
    std::string x;
    x += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n";
    x += "<!-- 自動生成 (MyEngine エディタ / Rebuild Scripts)。手編集しても上書きされる -->\r\n";
    x += "<Project DefaultTargets=\"Build\" "
         "xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\r\n";
    x += "  <ItemGroup Label=\"ProjectConfigurations\">\r\n";
    for (const char* cfg : { "Debug", "Release" }) {
        x += std::string("    <ProjectConfiguration Include=\"") + cfg + "|x64\">\r\n";
        x += std::string("      <Configuration>") + cfg + "</Configuration>\r\n";
        x += "      <Platform>x64</Platform>\r\n";
        x += "    </ProjectConfiguration>\r\n";
    }
    x += "  </ItemGroup>\r\n";
    x += "  <PropertyGroup Label=\"Globals\">\r\n";
    x += "    <ProjectGuid>{7E3A1C55-9B24-4E77-8A61-2D5F0C9E4B10}</ProjectGuid>\r\n";
    x += "    <RootNamespace>GameLogic</RootNamespace>\r\n";
    x += "    <WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>\r\n";
    x += "  </PropertyGroup>\r\n";
    x += "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.Default.props\" />\r\n";
    x += "  <PropertyGroup Label=\"Configuration\">\r\n";
    x += "    <ConfigurationType>DynamicLibrary</ConfigurationType>\r\n";
    x += "    <PlatformToolset>v143</PlatformToolset>\r\n";
    x += "    <CharacterSet>Unicode</CharacterSet>\r\n";
    x += "    <UseDebugLibraries Condition=\"'$(Configuration)'=='Debug'\">true"
         "</UseDebugLibraries>\r\n";
    x += "  </PropertyGroup>\r\n";
    x += "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.props\" />\r\n";
    x += "  <Import Project=\"" + XmlEscape(engineRepo) + "\\build\\Common.props\" />\r\n";
    // Common.props の OutDir/IntDir はエンジンリポジトリを指すので import 後に上書きする
    x += "  <PropertyGroup>\r\n";
    x += "    <LinkIncremental>false</LinkIncremental>\r\n";
    x += "    <TargetName>GameLogic</TargetName>\r\n";
    x += "    <OutDir>" + XmlEscape(cacheDir) + "\\</OutDir>\r\n";
    x += "    <IntDir>" + XmlEscape(cacheDir) + "\\obj\\</IntDir>\r\n";
    x += "  </PropertyGroup>\r\n";
    // cache\hot\v{N}\ へコピーした PDB をデバッガが隣から読めるようにする (本家と同じ)。
    // ClCompile の追加 include は cacheDir — 生成ヘッダ (Generated\SchemaComponents.gen.h)
    // を "Generated/..." で引くため (Common.props はエンジン側 src しか通していない)
    x += "  <ItemDefinitionGroup>\r\n";
    x += "    <ClCompile>\r\n";
    x += "      <AdditionalIncludeDirectories>" + XmlEscape(cacheDir)
         + ";%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>\r\n";
    x += "    </ClCompile>\r\n";
    x += "    <Link>\r\n";
    x += "      <AdditionalOptions>/PDBALTPATH:$(TargetName).pdb %(AdditionalOptions)"
         "</AdditionalOptions>\r\n";
    x += "    </Link>\r\n";
    x += "  </ItemDefinitionGroup>\r\n";
    x += "  <ItemGroup>\r\n";
    x += "    <ClCompile Include=\"" + XmlEscape(cacheDir) + "\\GameLogicMain.cpp\" />\r\n";
    for (const std::wstring& s : sources) {
        x += "    <ClCompile Include=\"" + XmlEscape(s) + "\" />\r\n";
    }
    x += "  </ItemGroup>\r\n";
    x += "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.targets\" />\r\n";
    x += "</Project>\r\n";
    f.write(x.data(), static_cast<std::streamsize>(x.size()));
    return true;
}

// プロジェクトの C++ スクリプトのビルド一式 (vcxproj + bat) を <project>\cache\ に生成し、
// bat の絶対パスを返す (空 = 失敗)。C# の [Compile C# Scripts] と同じ
// 「プロジェクト内のソースをエンジンがビルドする」モデル。エンジンリポジトリの vcxproj や
// gen_project_files.ps1 には一切依存しない。vcvars は使わない — MSBuild はツールチェーンを
// 自前で解決するので環境依存が少ない。起動は呼び出し側 (RebuildGameLogic =
// fire-and-forget / StartGameLogicBuild = ハンドルをポーリング) の責務
std::wstring PrepareProjectScriptsBat(EngineContext& ctx)
{
    const std::wstring engineRepo = FindEngineRepoRoot();
    if (engineRepo.empty()) {
        MYE_LOG_ERROR("engine repo not found (src\\Shared\\ScriptAPI.h) - "
                      "C++ scripts need the engine sources to compile");
        return {};
    }
    const std::wstring cacheDir = ctx.projectRoot + L"\\cache";
    const std::wstring scriptsDir = ctx.projectRoot + L"\\src\\GameLogic\\Scripts";
    std::error_code ec;
    fs::create_directories(cacheDir, ec);

    // スクリプト列挙 (決定論のためソート。リンク順 = 静的初期化順に効く)
    std::vector<std::wstring> sources;
    for (const auto& e : fs::directory_iterator(scriptsDir, ec)) {
        if (e.is_regular_file(ec) && e.path().extension() == L".cpp") {
            sources.push_back(e.path().wstring());
        }
    }
    std::sort(sources.begin(), sources.end());

    const std::wstring projPath = cacheDir + L"\\GameLogic.vcxproj";
    // スキーマ型付きヘッダ (M50d)。cache\Generated\ に生成し、vcxproj 側で cacheDir を
    // include 経路に足す → スクリプトからは #include "Generated/SchemaComponents.gen.h"
    {
        const std::wstring genDir = cacheDir + L"\\Generated";
        fs::create_directories(genDir, ec);
        schema::WriteIfChanged(genDir + L"\\SchemaComponents.gen.h",
                               schema::EmitCppHeader(schema::BuildCodegenModel()));
    }
    if (!WriteGeneratedMain(cacheDir + L"\\GameLogicMain.cpp")
        || !WriteGeneratedVcxproj(projPath, engineRepo, cacheDir, sources)) {
        return {};
    }

    // MSBuild の起動は tools\build_scripts.bat と同じ vswhere パターン。
    // 失敗時は pause で窓を残し、コンパイルエラーをそのまま読めるようにする
    const std::wstring cfg = RunningRelease() ? L"Release" : L"Debug";
    const std::wstring batPath = cacheDir + L"\\build_scripts.bat";
    {
        std::ofstream b{ fs::path(batPath), std::ios::binary };
        if (!b) {
            MYE_LOG_ERROR(Tr(StrId::Log_WriteFail), WideToUtf8(batPath).c_str());
            return {};
        }
        const std::wstring bat =
            L"@echo off\r\n"
            L"rem auto-generated by the MyEngine editor (Rebuild Scripts). Do not edit.\r\n"
            L"setlocal\r\n"
            L"cd /d \"%~dp0\"\r\n"
            L"\r\n"
            L"for /f \"usebackq tokens=*\" %%i in (`\"%ProgramFiles(x86)%\\Microsoft Visual "
            L"Studio\\Installer\\vswhere.exe\" -latest -products * -requires "
            L"Microsoft.Component.MSBuild -find MSBuild\\**\\Bin\\MSBuild.exe`) do set "
            L"MSBUILD=%%i\r\n"
            L"if not defined MSBUILD ( echo [scripts] MSBuild not found & pause & exit /b 1 )\r\n"
            L"\r\n"
            L"echo === building C++ scripts (" + cfg + L") ===\r\n"
            L"\"%MSBUILD%\" \"" + projPath + L"\" /p:Configuration=" + cfg
            + L" /p:Platform=x64 /m /v:minimal /nologo || ( echo. & echo [scripts] BUILD FAILED "
              L"-- fix errors above & pause & exit /b 1 )\r\n"
            L"\r\n"
            L"echo.\r\n"
            L"echo === scripts built. The editor will hot-reload within ~0.5s. ===\r\n"
            L"exit /b 0\r\n";
        const std::string acp = WideToAcp(bat);
        b.write(acp.data(), static_cast<std::streamsize>(acp.size()));
    }

    MYE_LOG_INFO(Tr(StrId::Log_BuildingCpp), sources.size(),
                 WideToUtf8(cacheDir).c_str(), WideToUtf8(cfg).c_str());
    return batPath;
}

} // namespace

// ★可視の cmd 窓で bat を投げる `RebuildGameLogic` は M66e で**削除した**。
//   fire-and-forget ではプロセスハンドルが誰の手にも残らず、走っている間
//   `GateBlocker::ScriptBuildRunning` が立たない = ビルドが bin\ と cache\ を
//   書いている最中に checkout / pull が通ってしまう (spec §7 の穴)。
//   起動口は「ハンドルを返す」この 1 本だけにする。
std::vector<BuildErrorLine> ParseBuildErrorLines(const std::string& logUtf8)
{
    std::vector<BuildErrorLine> out;
    for (size_t pos = 0; pos <= logUtf8.size();) {
        const size_t nl = logUtf8.find('\n', pos);
        const size_t end = (nl == std::string::npos) ? logUtf8.size() : nl;
        std::string line = logUtf8.substr(pos, end - pos);
        pos = (nl == std::string::npos) ? logUtf8.size() + 1 : nl + 1;
        const size_t first = line.find_first_not_of(" \t\r");
        const size_t last = line.find_last_not_of(" \t\r");
        if (first == std::string::npos) {
            continue;
        }
        line = line.substr(first, last - first + 1);
        size_t at = line.find(": error ");
        if (at == std::string::npos) {
            at = line.find(": fatal error ");
        }
        if (at == std::string::npos) {
            continue;
        }
        bool dup = false;
        for (const BuildErrorLine& e : out) {
            dup = dup || e.text == line;
        }
        if (dup) {
            continue;
        }
        BuildErrorLine e;
        e.text = line;
        // "C:\\path\\x.cpp(12,34): error C2065: ..." → file / line。
        // 括弧が無い形 ("LINK : fatal error LNK1104: ...") は file/line 無しで出す
        const size_t open = line.rfind('(', at);
        if (open != std::string::npos && open > 0) {
            const size_t close = line.find_first_of(",)", open + 1);
            if (close != std::string::npos && close < at) {
                const std::string num = line.substr(open + 1, close - open - 1);
                if (!num.empty() && num.find_first_not_of("0123456789") == std::string::npos) {
                    e.line = std::atoi(num.c_str());
                    e.file = line.substr(0, open);
                }
            }
        }
        out.push_back(std::move(e));
    }
    return out;
}

void* StartGameLogicBuild(EngineContext& ctx, std::wstring& logPathOut)
{
    // RebuildGameLogic の二経路と同じ bat を使う。違いは起動形態のみ:
    //   - stdout/stderr をログファイルへリダイレクト (エディタは完了後に失敗の尻尾を出せる)
    //   - stdin を NUL に繋ぐ — bat の失敗系 `pause` が EOF を読んで即抜ける = 詰まらない
    //   - CREATE_NO_WINDOW + プロセスハンドル返し = BuildSettings が毎フレームポーリング
    std::wstring bat, workDir, args;
    const std::wstring cfg = RunningRelease() ? L"Release" : L"Debug";
    if (!ctx.projectRoot.empty()) {
        bat = PrepareProjectScriptsBat(ctx);
        workDir = ctx.projectRoot + L"\\cache";
        args = L"cmd.exe /c \"\"" + bat + L"\"\"";
    } else {
        const std::wstring repo = ScriptsRoot(ctx);
        bat = repo + L"\\tools\\build_scripts.bat";
        workDir = repo;
        args = L"cmd.exe /c \"\"" + bat + L"\" " + cfg + L"\"";
    }
    if (bat.empty() || !fs::exists(bat)) {
        if (!bat.empty()) {
            MYE_LOG_ERROR(Tr(StrId::Log_BatMissing), WideToUtf8(bat).c_str());
        }
        return nullptr;
    }
    logPathOut = workDir + L"\\build_scripts.log";

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE log = CreateFileW(logPathOut.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE nulIn = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = log;
    si.hStdError = log;
    si.hStdInput = nulIn;
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> cmdline(args.begin(), args.end());
    cmdline.push_back(L'\0'); // CreateProcessW は書込可能バッファを要求する
    const BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, workDir.c_str(), &si, &pi);
    if (log != INVALID_HANDLE_VALUE) {
        CloseHandle(log); // 子が継承済み — 親側は即クローズでよい
    }
    if (nulIn != INVALID_HANDLE_VALUE) {
        CloseHandle(nulIn);
    }
    if (!ok) {
        MYE_LOG_ERROR("[build] CreateProcess failed for script build (%lu)", GetLastError());
        return nullptr;
    }
    CloseHandle(pi.hThread);
    MYE_LOG_INFO(Tr(StrId::Log_BuildingGameLogic), WideToUtf8(cfg).c_str());
    return pi.hProcess;
}

} // namespace mye
