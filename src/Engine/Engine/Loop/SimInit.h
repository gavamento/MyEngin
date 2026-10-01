//====================================================================================
//                          SimInit.h
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          sim 側の起動手順 (EngineLoop / HeadlessSim 共通)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mye {

struct EngineConfig;
struct RenderResources;
struct PadVibrationState;
struct CursorLockState;
struct WindowModeState;
struct ScriptAudioEvent;
struct EffectSpawnRequest;
struct DebugLineCmd;
struct NetRuntimeInfo;
class Scene;
class ScriptHost;
class ManagedHost;
class DllReloader;
class AssetDatabase;
class InputActions;
class MeshColliderLibrary;
class ConvexColliderLibrary;
class FractureLibrary;
class PhysMatLibrary;
class TerrainColliderLibrary;

// EngineLoop::Run と HeadlessSim は GPU / 窓 / オーディオ以外の初期化を**この関数群で共有する**。
// 手で写すと 1 手順の漏れが tick 0 からの MISMATCH になる (スキーマ型の登録順は TypeId を動かす) ので、
// sim の入力になる起動手順はここにだけ置き、呼ぶ順序は呼び出し側の責務として下に書いた順に揃える:
//   ResolveAssetsRoot → InstallSimLibraries → InitSimProjectState → LoadGameLogic
//   → WireScriptServices → InitSimAssets → ConfigureSimCaches → (シーン構築)

// オーディオ再生のバリエーション抽選用 RNG の種。world.Rng() とは別系統 (出力レーンだけが引く)
inline constexpr uint64_t kAudioScriptRngSeed = 0x4D796541536372ull; // "MyeAScr"

// <projectRoot>\assets、無ければ従来の探索 (FindAssetsRoot)。見つからなければ ERROR を出して false
bool ResolveAssetsRoot(const EngineConfig& config, std::wstring& assetsRoot);

// sim が AssetID から形状・設定を引くライブラリ群 (モジュール注入)。resources は Init 済みか
// InitHeadless 済みであること。ポインタはすべて非 null
struct SimLibraries {
    RenderResources* resources = nullptr;
    MeshColliderLibrary* meshColliders = nullptr;
    ConvexColliderLibrary* convexColliders = nullptr;
    FractureLibrary* fractureAssets = nullptr;
    PhysMatLibrary* physMat = nullptr;
    TerrainColliderLibrary* terrainColliders = nullptr;
};
void InstallSimLibraries(const SimLibraries& libs);
// 所有者が死ぬ前に必ず呼ぶ (注入したポインタをプロセス全体から外す)
void UninstallSimLibraries();

// tick が回る前に 1 回だけ決める sim の静的な入力: UI の基準解像度、フォント計測表、タグ名の表。
// 途中で変えると同じ .rep の再生が割れる
void InitSimProjectState(const std::wstring& assetsRoot);

// スキーマ型の登録 → GameLogic.dll のロード。**この順序が TypeId の割り当てを決める**
// (スキーマ型は組込み型の後・スクリプト型の前)。DLL が無くても false を返すだけで継続する
bool LoadGameLogic(Scene& scene, ScriptHost& scriptHost, DllReloader& dllReloader,
                   const std::wstring& assetsRoot, const std::wstring& dllPath,
                   const std::wstring& shadowCopyDir);

// ScriptHost / ManagedHost に渡す共有バッファの束 (所有者は呼び出し側)
struct SimSharedServices {
    std::vector<ScriptAudioEvent>* audioQueue = nullptr;
    std::wstring* pendingScene = nullptr;
    std::vector<EffectSpawnRequest>* effectQueue = nullptr;
    std::vector<DebugLineCmd>* debugLines = nullptr;
    uint64_t* audioHandleSeq = nullptr;
    const InputActions* inputActions = nullptr;
    int* pendingSaveSlot = nullptr;
    int* pendingLoadSlot = nullptr;
    PadVibrationState* padVibration = nullptr;
    const NetRuntimeInfo* netInfo = nullptr;
    CursorLockState* cursorLock = nullptr;
    int* pendingLoadPersistSlot = nullptr;
    WindowModeState* windowMode = nullptr;
};
void WireScriptServices(ScriptHost& scriptHost, ManagedHost& managedHost,
                        const SimSharedServices& services, bool developmentRun);

// 入力アクションマップの読込と AssetDatabase の同期 (path → AssetID の GUID 解決を有効にする)。
// アセット登録 (シーン構築) より前に行うこと
void InitSimAssets(AssetDatabase& assetDatabase, InputActions& inputActions,
                   const std::wstring& assetsRoot);

// sim 索引 (--no-sim-cache) と、アセットクックキャッシュの置き場 / 有効無効を設定する
void ConfigureSimCaches(const EngineConfig& config, const std::wstring& cookedDir);

} // namespace mye
