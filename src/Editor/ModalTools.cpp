//====================================================================================
//                          ModalTools.cpp
//  MyEngine/ 秋田蓮音                                                      09/16/2026
//                                          --modal-voxelize の実装
//====================================================================================
#include "Editor/ModalTools.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <unordered_set>

#include <chrono>

#include "Engine/Engine/Asset/CookedCache.h"
#include "Engine/Engine/AssetDatabase.h"
#include "Engine/Engine/FbxLoader.h"
#include "Engine/Engine/Modal/ModalSoundLibrary.h"
#include "Engine/Engine/Modal/TriangleSoup.h"
#include "Engine/Engine/Modal/Voxelizer.h"
#include "Engine/Engine/ModelLoader.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/ShaderManager.h"

namespace fs = std::filesystem;

namespace mye {
namespace modaltools {
namespace {

std::string Trim(const std::string& s)
{
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        return {};
    }
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::wstring LowerExt(const std::wstring& path)
{
    std::wstring ext = fs::path(path).extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return ext;
}

// 登録名の "#mesh<N>#part<M>" (FBX) / "#mesh<N>#prim<M>" (glTF) から実際の番号を取り出す。
// 見つからなければ false (呼び出し側が 0/0 にフォールバックする)
bool ParseMeshPrimSuffix(const std::string& name, int& meshIdx, int& primIdx)
{
    const size_t meshPos = name.find("#mesh");
    if (meshPos == std::string::npos) {
        return false;
    }
    size_t p = meshPos + 5;
    size_t q = p;
    while (q < name.size() && std::isdigit(static_cast<unsigned char>(name[q]))) {
        ++q;
    }
    if (q == p) {
        return false;
    }
    meshIdx = std::stoi(name.substr(p, q - p));

    const size_t partPos = name.find("#part", q);
    const size_t primPos = name.find("#prim", q);
    size_t usePos = std::string::npos;
    if (partPos != std::string::npos) {
        usePos = partPos;
    }
    if (primPos != std::string::npos && (usePos == std::string::npos || primPos < usePos)) {
        usePos = primPos;
    }
    if (usePos == std::string::npos) {
        return false;
    }
    p = usePos + 5;
    q = p;
    while (q < name.size() && std::isdigit(static_cast<unsigned char>(name[q]))) {
        ++q;
    }
    if (q == p) {
        return false;
    }
    primIdx = std::stoi(name.substr(p, q - p));
    return true;
}

struct VoxTargetMesh {
    int meshIdx = 0;
    int primIdx = 0;
    const Mesh* mesh = nullptr;
};

// RegisterAssets 呼び出し前後の登録済みメッシュの差分 = そのファイルが新たに持ち込んだメッシュ。
// meshIdx/primIdx で安定ソートする (パース失敗は 0/0 に丸まるが、Enumerate 自体が名前昇順なので
// stable_sort によりファイル内の元の並びは保たれる)
std::vector<VoxTargetMesh> CollectNewMeshes(RenderResources& resources,
                                            const std::vector<AssetEntry>& before)
{
    std::unordered_set<uint64_t> beforeIds; // 会員判定だけに使う (走査順は結果に影響しない)
    beforeIds.reserve(before.size());
    for (const AssetEntry& e : before) {
        beforeIds.insert(e.id.value);
    }

    std::vector<VoxTargetMesh> result;
    for (const AssetEntry& e : resources.meshes.Enumerate()) {
        if (beforeIds.find(e.id.value) != beforeIds.end()) {
            continue;
        }
        VoxTargetMesh t;
        ParseMeshPrimSuffix(e.name, t.meshIdx, t.primIdx); // 失敗時は 0/0 のまま
        t.mesh = resources.meshes.Get(e.id);
        result.push_back(t);
    }
    std::stable_sort(result.begin(), result.end(), [](const VoxTargetMesh& a, const VoxTargetMesh& b) {
        if (a.meshIdx != b.meshIdx) {
            return a.meshIdx < b.meshIdx;
        }
        return a.primIdx < b.primIdx;
    });
    return result;
}

bool WriteMvox(const std::wstring& outDir, const std::string& outName, const modal::VoxelGrid& grid,
              std::string* err)
{
    std::vector<uint8_t> bytes;
    modal::SerializeVox(grid, bytes);
    const std::wstring outPath = outDir + L"\\" + Utf8ToWide(outName) + L".mvox";
    const std::string_view view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!WriteFileReplacing(outPath, view)) {
        if (err) {
            *err = "failed to write " + WideToUtf8(outPath);
        }
        return false;
    }
    return true;
}

bool VoxelizeAndWrite(const std::wstring& outDir, const std::string& outName,
                     const DirectX::XMFLOAT3* pos, size_t n, const uint32_t* idx, size_t m,
                     const std::string& sourceLabel, bool& anyError)
{
    modal::VoxelGrid grid;
    if (!modal::VoxelizeMesh(pos, n, idx, m, grid)) {
        std::fprintf(stderr, "[modal-voxelize] ERROR: voxelize failed: %s (%s)\n",
                     sourceLabel.c_str(), outName.c_str());
        anyError = true;
        return false;
    }
    std::string err;
    if (!WriteMvox(outDir, outName, grid, &err)) {
        std::fprintf(stderr, "[modal-voxelize] ERROR: %s\n", err.c_str());
        anyError = true;
        return false;
    }
    std::printf("[modal-voxelize] %s -> %s.mvox (surface=%u interior=%u)\n", sourceLabel.c_str(),
                outName.c_str(), grid.surfaceCount, grid.interiorCount);
    return true;
}

// ---- --modal-bake (M76e) ----

bool EndsWithLower(const std::wstring& lowerPath, const wchar_t* suffix)
{
    const size_t n = wcslen(suffix);
    return lowerPath.size() >= n && lowerPath.compare(lowerPath.size() - n, n, suffix) == 0;
}

const char* ModalStateName(ModalState s)
{
    switch (s) {
    case ModalState::Missing:
        return "Missing";
    case ModalState::Baking:
        return "Baking";
    case ModalState::Ready:
        return "Ready";
    case ModalState::Failed:
        return "Failed";
    case ModalState::NoModel:
        return "NoModel";
    }
    return "Unknown";
}

} // namespace

int RunModalVoxelizeCli(const std::wstring& listPath, const std::wstring& outDir)
{
    if (listPath.empty() || outDir.empty()) {
        std::fprintf(stderr, "[modal-voxelize] ERROR: --list and --out are required\n");
        return 1;
    }

    std::ifstream listFile(listPath);
    if (!listFile) {
        std::fprintf(stderr, "[modal-voxelize] ERROR: cannot open list: %s\n",
                     WideToUtf8(listPath).c_str());
        return 1;
    }

    std::error_code ec;
    fs::create_directories(outDir, ec); // 既に存在していても構わない

    // モデルのヘッドレス登録用 (SubAssetMigration.cpp と同経路)。GraphicsDevice を持たないので
    // GPU バッファは作られず CPU 側 (positions/indices/aabb) だけが登録される。
    // builtin:// は resources.meshes.Cube() 等を直接呼ぶだけで Init() 自体は不要
    // (Register は device_==nullptr を許容する設計、GpuResources.cpp:161)
    RenderResources resources;
    ShaderManager shaders;

    bool anyError = false;
    int written = 0;
    std::string line;
    while (std::getline(listFile, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        if (trimmed.rfind("builtin://", 0) == 0) {
            const std::string name = trimmed.substr(10);
            AssetID id;
            if (name == "cube") {
                id = resources.meshes.Cube();
            } else if (name == "sphere") {
                id = resources.meshes.Sphere();
            } else if (name == "plane") {
                id = resources.meshes.Plane();
            } else if (name == "quad") {
                id = resources.meshes.Quad();
            } else if (name == "cylinder") {
                id = resources.meshes.Cylinder();
            } else if (name == "capsule") {
                id = resources.meshes.Capsule();
            } else {
                std::fprintf(stderr, "[modal-voxelize] ERROR: unknown builtin mesh: %s\n",
                             trimmed.c_str());
                anyError = true;
                continue;
            }
            const Mesh* mesh = resources.meshes.Get(id);
            if (!mesh || mesh->positions.empty()) {
                std::fprintf(stderr, "[modal-voxelize] ERROR: builtin mesh has no geometry: %s\n",
                             trimmed.c_str());
                anyError = true;
                continue;
            }
            if (VoxelizeAndWrite(outDir, name + "#mesh0#prim0", mesh->positions.data(),
                                 mesh->positions.size(), mesh->indices.data(),
                                 mesh->indices.size(), trimmed, anyError)) {
                ++written;
            }
            continue;
        }

        const std::wstring wpath = Utf8ToWide(trimmed);
        if (!fs::exists(wpath, ec)) {
            std::fprintf(stderr, "[modal-voxelize] ERROR: input not found: %s\n", trimmed.c_str());
            anyError = true;
            continue;
        }

        const std::wstring ext = LowerExt(wpath);
        const std::string stem = WideToUtf8(fs::path(wpath).stem().wstring());

        if (ext == L".off" || ext == L".obj") {
            std::ifstream f(wpath, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
            modal::TriangleSoup soup;
            std::string parseErr;
            const bool ok = (ext == L".off") ? modal::LoadOffText(text, soup, &parseErr)
                                              : modal::LoadObjText(text, soup, &parseErr);
            if (!ok) {
                std::fprintf(stderr, "[modal-voxelize] ERROR: %s: %s\n", trimmed.c_str(),
                             parseErr.c_str());
                anyError = true;
                continue;
            }
            if (VoxelizeAndWrite(outDir, stem + "#mesh0#prim0", soup.positions.data(),
                                 soup.positions.size(), soup.indices.data(), soup.indices.size(),
                                 trimmed, anyError)) {
                ++written;
            }
            continue;
        }

        if (ext == L".fbx" || ext == L".gltf" || ext == L".glb") {
            const std::vector<AssetEntry> before = resources.meshes.Enumerate();
            const bool ok = (ext == L".fbx")
                ? FbxLoader::RegisterAssets(resources, shaders, wpath, /*logErrors=*/true)
                : ModelLoader::RegisterAssets(resources, shaders, wpath, /*logErrors=*/true);
            if (!ok) {
                std::fprintf(stderr, "[modal-voxelize] ERROR: failed to load model: %s\n",
                             trimmed.c_str());
                anyError = true;
                continue;
            }
            const std::vector<VoxTargetMesh> targets = CollectNewMeshes(resources, before);
            if (targets.empty()) {
                std::fprintf(stderr, "[modal-voxelize] ERROR: no meshes found in %s\n",
                             trimmed.c_str());
                anyError = true;
                continue;
            }
            for (const VoxTargetMesh& t : targets) {
                if (!t.mesh || t.mesh->positions.empty()) {
                    continue;
                }
                char suffix[64];
                std::snprintf(suffix, sizeof(suffix), "#mesh%d#prim%d", t.meshIdx, t.primIdx);
                if (VoxelizeAndWrite(outDir, stem + suffix, t.mesh->positions.data(),
                                     t.mesh->positions.size(), t.mesh->indices.data(),
                                     t.mesh->indices.size(), trimmed, anyError)) {
                    ++written;
                }
            }
            continue;
        }

        std::fprintf(stderr, "[modal-voxelize] ERROR: unsupported extension: %s\n", trimmed.c_str());
        anyError = true;
    }

    std::printf("[modal-voxelize] wrote %d file(s)%s\n", written, anyError ? " (with errors)" : "");
    return anyError ? 1 : 0;
}

int RunModalBakeCli(const std::wstring& projectDir)
{
    std::error_code ec;
    const std::wstring assetsRoot = projectDir.empty()
        ? FindAssetsRoot()
        : (fs::absolute(projectDir, ec) / L"assets").wstring();
    if (!fs::is_directory(assetsRoot, ec)) {
        std::fprintf(stderr, "[modal-bake] ERROR: assets root not found: %s\n",
                     WideToUtf8(assetsRoot).c_str());
        return 1;
    }

    // .msfm の読み書きに使うクックキャッシュ。二経路は EngineLoop.cpp の起動配線と同じ規則
    // (分岐は必ず projectDir の有無で判定) — これが無いと BakeSync が Ready にはなっても
    // 2 回目の起動で 1 バイトも再利用されない
    const std::wstring cookedDir =
        (projectDir.empty() ? GetExecutableDir() : fs::absolute(projectDir, ec).wstring())
        + L"\\cache\\cooked";
    CookedCache::Configure(cookedDir, true);

    // guid:// サブアセットキーが SourcePathForSubAssetKey で解決できるように、
    // AssetDatabase を走査して resolver として注入する (SubAssetMigration.cpp の
    // RunMigration と同じ手順)
    AssetDatabase db;
    db.ScanAndSync(assetsRoot);
    db.InstallAsKeyResolver();

    const std::wstring dmnetPath = ResolveDeepModalPath(assetsRoot);
    RenderResources resources;
    ShaderManager shaders;
    ModalSoundLibrary lib;
    lib.Init(&resources);
    lib.SetBackendByName("cpu");
    if (dmnetPath.empty() || !lib.LoadModel(dmnetPath)) {
        std::fprintf(stderr, "[modal-bake] ERROR: no usable .dmnet found (project or engine "
                             "assets\\deepmodal\\deepmodal.dmnet)\n");
        AssetDatabase::UninstallKeyResolver();
        return 2;
    }

    // モデルのヘッドレス登録 (SubAssetMigration.cpp の RunMigration と同じ走査)。
    // GraphicsDevice が無いので GPU バッファは作られず、CPU 側 (positions/indices) だけが揃う
    size_t modelsRegistered = 0;
    for (const auto& e : fs::recursive_directory_iterator(assetsRoot, ec)) {
        if (!e.is_regular_file()) {
            continue;
        }
        const std::wstring p = e.path().wstring();
        std::wstring lower = p;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
        if (EndsWithLower(lower, L".fbx")) {
            modelsRegistered += FbxLoader::RegisterAssets(resources, shaders, p, true) ? 1 : 0;
        } else if (EndsWithLower(lower, L".glb") || EndsWithLower(lower, L".gltf")) {
            modelsRegistered += ModelLoader::RegisterAssets(resources, shaders, p, true) ? 1 : 0;
        }
    }

    std::vector<AssetEntry> meshes = resources.meshes.Enumerate(); // 名前昇順 (決定的な出力順)
    int bakes = 0;
    double totalMs = 0.0;
    for (const AssetEntry& entry : meshes) {
        Mesh* m = resources.meshes.Get(entry.id);
        if (!m || m->positions.empty()) {
            continue;
        }
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = lib.BakeSync(entry.id);
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        const ModalFeatureMap* map = ok ? lib.Get(entry.id) : nullptr;
        std::printf("[modal-bake] %s %s %.2f %u\n", entry.name.c_str(),
                    ModalStateName(ok ? ModalState::Ready : ModalState::Failed), ms,
                    map ? map->validCount : 0u);
        ++bakes;
        totalMs += ms;
    }
    std::printf("[modal-bake] models=%zu bakes=%d bakeMsAvg=%.2f\n", modelsRegistered, bakes,
                bakes > 0 ? totalMs / bakes : 0.0);

    AssetDatabase::UninstallKeyResolver();
    return 0;
}

} // namespace modaltools
} // namespace mye
