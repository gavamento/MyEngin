#include "Editor/EditorApp.h"

#include <algorithm>
#include <cstdio> // M56c: HZB のミップ番号入りメニューラベルを訳文書式から組む
#include <filesystem>
#include <vector>

#include <Windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include "Editor/AssetOps.h"
#include "Editor/CreateMenu.h"
#include "Editor/EditorGlobalSettings.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Localization.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/DemoContent.h"
#include "Engine/Engine/EntityNaming.h"
#include "Engine/Engine/HotReload/DllReloader.h"
#include "Engine/Engine/HotReload/ReloadHub.h"
#include "Engine/Engine/ModelLoader.h"
#include "Engine/Engine/Prefab.h"
#include "Engine/Engine/Project.h"
#include "Engine/Engine/Script/ScriptHost.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Platform/Win32Window.h"
#include "Engine/Engine/Replay/TimeTravel.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/SceneSerializer.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/ShaderManager.h"

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder API
#include "ImGuizmo/ImGuizmo.h"

namespace mye {
namespace {

// M56c: Rendering > HZB に並べる段数。960x540 のピラミッドは 11 段あるが、上の方は
// 数テクセルしか無くて可視化しても情報が無い — 「段ごとに四角が倍になる」が読み取れる
// 範囲だけをメニューに出す。それより上を見たいときは --hzb-debug N で任意の段を指定できる
// (指定が段数を超えたら DeferredPath 側が最上段で頭打ちにする)
constexpr int kHzbDebugMenuMips = 6;

} // namespace

void EditorApp::OnStart(EngineContext& ctx)
{
    ctx.shaders->Load("forward_lit");
    // 設定の置き場 (M26): プロジェクト起動時は <project>\.mye\、レガシー時は従来の assets\ 直下
    const std::wstring settingsDir = ctx.projectRoot.empty()
        ? ctx.assetsRoot
        : ctx.projectRoot + L"\\" + kProjectLocalDir;
    settings_.Load(settingsDir);
    // 名前付きレイアウト: ImGui ini + パネル開閉フラグを <settingsDir>\layouts に保存/復元
    layouts_.Init(settingsDir + L"\\layouts",
                  { { "Hierarchy", &hierarchy_.open },
                    { "Inspector", &inspector_.open },
                    { "Console", &console_.open },
                    { "Scene", &sceneView_.open },
                    { "Game", &gameView_.open },
                    { "Assets", &assetBrowser_.open },
                    { "Animation", &animation_.open },
                    { "Animator", &animatorController_.open },
                    { "Search", &search_.open },
                    { "Profiler", &profiler_.open },
                    { "Timeline", &timeline_.open },
                    { "Network", &net_.open },
                    { "Particle Settings", &particleSettings_.open },
                    { "Sound Generator", &soundGen_.open },
                    { "Audio Mixer", &audioMixer_.open },
                    { "Project Settings", &projectSettings_.open },
                    { "Build Settings", &buildSettings_.open },
                    { "Stats", &showStats_ } });
    if (!sceneOverride.empty()) {
        scenePath_ = sceneOverride;
    } else if (rtShowcase) {
        // M46i: ショーケースの保存先は専用パスにする。ここを main.scene.json のままにすると
        // Ctrl+S ひとつで既定デモシーンが置き換わり、golden.rep の入力が変わってしまう
        scenePath_ = ctx.assetsRoot + L"\\scenes\\rt_showcase.scene.json";
    } else if (partsShowcase) {
        // M48g: 部位追従のリプレイ被覆シーン。**cache\ に置く** (git 非追跡) —
        // モデル由来のサブアセット ID は「正規化した**絶対パス**のハッシュ」なので、
        // 保存した .scene.json はチェックアウト先に依存する = コミットできない。
        // 版管理された唯一の正解は BuildPartsShowcaseScene (コード) 側で、
        // replay_verify.bat は毎回そこから組み直してから記録する
        scenePath_ = L"cache\\parts_showcase.scene.json";
    } else if (flowShowcase) {
        // M51j: フロー統合デモのタイトルシーン。ファイルは EnsureFlowShowcaseScenes が
        // assets\scenes\ に生成する (gitignore 済み) — タイトル⇄ゲームの遷移が
        // LoadScene("scenes/flow_*.scene.json") = assets 相対解決のため、cache\ には置けない
        scenePath_ = ctx.assetsRoot + L"\\scenes\\flow_title.scene.json";
    } else if (localDemo) {
        // M52g: コードから毎回組む (ファイルは作らない)。パスだけ cache\ へ振っておくと、
        // 万一 Ctrl+S されても既定デモシーン = golden.rep の入力を潰さない
        scenePath_ = L"cache\\local_players.scene.json";
    } else if (netDemo) {
        scenePath_ = L"cache\\net_duel.scene.json"; // M52i (同上)
    } else if (renderShowcase) {
        scenePath_ = L"cache\\render_showcase.scene.json"; // M54a (同上)
    } else if (terrainShowcase) {
        scenePath_ = L"cache\\terrain_showcase.scene.json"; // M58c (同上)
    } else if (physicsShowcase) {
        scenePath_ = L"cache\\physics_showcase.scene.json"; // M59d (同上)
    } else if (jointShowcase) {
        scenePath_ = L"cache\\joint_showcase.scene.json"; // M60i (同上)
    } else if (fogShowcase) {
        scenePath_ = L"cache\\fog_showcase.scene.json"; // M57追補 (同上)
    } else {
        scenePath_ = ctx.assetsRoot + L"\\scenes\\main.scene.json";
        ProjectManifest manifest; // ブートシーンはマニフェスト優先 (M26)
        if (!ctx.projectRoot.empty() && LoadProjectManifest(ctx.projectRoot, manifest)) {
            scenePath_ = ProjectBootScenePath(ctx.projectRoot, manifest);
        }
    }
    ctx.reloadHub->SetActiveScenePath(scenePath_);
    rebuildDockLayout_ = !std::filesystem::exists(ctx.imguiIniPath);

    // リソース (メッシュ/マテリアル/モデル) は毎回登録する。
    // シーンファイルは AssetID しか持たないため、実体の登録は起動側の責務
    // (パスベースの完全なアセット解決は AssetManager の将来拡張)
    RegisterDemoContent(ctx);
    RegisterAssetLibraries(ctx); // シーンロード前に .prefab/.anim を登録 (参照解決のため)
    // ショーケース材質は無条件で登録する (M50a)。名前キーの少数登録で冪等・非ハッシュなので
    // 常時登録が安全 — フラグゲートだと保存済みショーケースを --scene で直接開いたときに
    // 材質が解決されず、キューブ群が描画されない
    RegisterRtShowcaseContent(ctx);
    RegisterPartsShowcaseContent(ctx);
    RegisterFlowShowcaseContent(ctx); // M51j (flow_* 材質。少数の名前キー登録なので常時)
    RegisterLocalPlayersContent(ctx); // M52g (mp_* 材質。同上の理由で常時)
    RegisterNetDuelContent(ctx);      // M52i (duel_* 材質。同上)
    RegisterRenderShowcaseContent(ctx); // M54a (rdemo_* 材質。同上)
    RegisterTerrainShowcaseContent(ctx); // M58c (tdemo_* 材質。同上)
    RegisterPhysicsShowcaseContent(ctx); // M59d (pdemo_* 材質。同上)
    RegisterJointShowcaseContent(ctx);   // M60i (jdemo_* 材質 + 車輪メッシュ。同上)
    RegisterFogShowcaseContent(ctx);     // M57追補 (fdemo_* 材質。同上)
    RegisterParticleShowcaseContent(ctx); // M63a (vdemo_* 材質 + 手続きテクスチャ。同上)
    if (flowShowcase) {
        // 両シーンファイルを確保してからタイトルを普通のロード経路で開く。
        // ここで組む = GameLogic.dll / C# コンパイル済み (EngineLoop が OnStart 前に実施)
        // なので FlowTitleDriver / FlowMenu のアタッチが解決できる
        EnsureFlowShowcaseScenes(ctx);
    }
    undo_.SetPrefabLibrary(ctx.prefabs); // 編集直後の override リスト記録 (M48e)
    if (std::filesystem::exists(scenePath_)) {
        SceneSerializer::LoadFromFile(*ctx.scene, scenePath_);
        // ロード直後 1 回だけ: 閉じている間に更新されたプレハブへ非 override を追随させる (M48e)
        Prefab::RefreshNonOverridden(*ctx.scene, *ctx.prefabs);
    } else if (rtShowcase) {
        BuildRtShowcaseScene(ctx); // M46i
    } else if (partsShowcase) {
        BuildPartsShowcaseScene(ctx); // M48g
    } else if (localDemo) {
        BuildLocalPlayersScene(ctx); // M52g
    } else if (netDemo) {
        BuildNetDuelScene(ctx); // M52i
    } else if (renderShowcase) {
        BuildRenderShowcaseScene(ctx); // M54a
    } else if (terrainShowcase) {
        BuildTerrainShowcaseScene(ctx, terrainLodDistance, terrainSkirtDepth); // M58c / M58e
    } else if (physicsShowcase) {
        BuildPhysicsShowcaseScene(ctx); // M59d
    } else if (jointShowcase) {
        BuildJointShowcaseScene(ctx); // M60i
    } else if (fogShowcase) {
        BuildFogShowcaseScene(ctx); // M57追補
    } else if (particleShowcase) {
        BuildParticleShowcaseScene(ctx); // M63a
    } else {
        BuildDemoScene(ctx, perfRate, perfMax);
    }
    if (saveSceneOnStart) {
        std::filesystem::create_directories(std::filesystem::path(scenePath_).parent_path());
        SceneSerializer::SaveToFile(*ctx.scene, scenePath_);
    }
    if (!selectName.empty()) {
        ctx.scene->GetWorld().ApplyStructuralChanges();
        if (GameObject g = ctx.scene->Find(selectName)) {
            selection_.SelectOnly(ctx.scene->EnsureFileId(g.Id()));
            MYE_LOG_INFO("selected '%s' (fileId %llu)", selectName.c_str(),
                         static_cast<unsigned long long>(selection_.primary));
        }
    }
    if (!editActorPath.empty()) {
        // M48k: ダブルクリック相当の口。他の検証フラグと同じく起動直後に 1 回だけ効く
        OpenActorEdit(ctx, std::filesystem::absolute(editActorPath).wstring());
    }
    if (!packageDir.empty()) {
        // M51j: CLI パッケージ (GUI の「ビルドを作成」と同じパイプライン)
        buildSettings_.StartCliPackage(std::filesystem::absolute(packageDir).wstring(),
                                       packageDds, packageZip, packageBoot);
    }
    if (autoPlay && !actorEdit_) { // 編集モード中の Play は禁止 (ツールバーでも無効化している)
        playMode_.Play(*ctx.scene);
    }
    if (startDeferred) {
        ctx.renderPath = ctx.renderPathDeferred;
    }

    // ---- フィードバック層の初期化 (M27b) ----
    {
        ProjectManifest manifest;
        if (!ctx.projectRoot.empty() && LoadProjectManifest(ctx.projectRoot, manifest)) {
            projectName_ = manifest.name;
        }
    }
    savedStateSerial_ = undo_.StateSerial(); // ロード直後 = clean
    {
        wchar_t title[256] = {};
        GetWindowTextW(static_cast<HWND>(ctx.window->Hwnd()), title, 256);
        baseTitle_ = title;
    }
    lastDllVersion_ = ctx.dllReloader->Version();
    lastReloadCount_ = ctx.reloadHub->ReloadCount();
    // エクスプローラーからのファイルドロップを受理する (エディタ専用機能なのでここで有効化。
    // ゲーム実行系は DragAcceptFiles を呼ばないため対象外のまま)
    DragAcceptFiles(static_cast<HWND>(ctx.window->Hwnd()), TRUE);
    // 管理者権限で起動されたエディタ (管理者 VS からの F5 等) は UIPI が中権限エクスプローラー
    // からの WM_DROPFILES を遮断し、ドラッグが禁止カーソルになる。シェル D&D に必要な
    // 3 メッセージ (WM_DROPFILES / WM_COPYDATA / WM_COPYGLOBALDATA) のみ下位権限から許可する。
    // 非昇格時は実質 no-op。エディタ限定の緩和で、Runtime.exe には適用しない
    {
        const HWND hwnd = static_cast<HWND>(ctx.window->Hwnd());
        constexpr UINT kCopyGlobalData = 0x0049; // WM_COPYGLOBALDATA (ヘッダ未定義の内部メッセージ)
        ChangeWindowMessageFilterEx(hwnd, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
        ChangeWindowMessageFilterEx(hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
        ChangeWindowMessageFilterEx(hwnd, kCopyGlobalData, MSGFLT_ALLOW, nullptr);
    }
    // × ボタン (WM_CLOSE) の横取り + WM_DROPFILES の受信。ハンドラは DefWindowProc より先に走る
    ctx.window->AddMsgHandler([this](void*, uint32_t msg, uint64_t wparam, int64_t, int64_t& result) {
        if (msg == WM_CLOSE) {
            closeRequested_ = true;
            result = 0;
            return true;
        }
        if (msg == WM_DROPFILES) {
            HDROP drop = reinterpret_cast<HDROP>(wparam);
            PendingFileDrop pd;
            POINT pt = {};
            pd.inClientArea = (DragQueryPoint(drop, &pt) != FALSE); // クライアント座標
            pd.clientX = static_cast<float>(pt.x);
            pd.clientY = static_cast<float>(pt.y);
            const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                const UINT len = DragQueryFileW(drop, i, nullptr, 0); // 終端を除く必要長
                std::vector<wchar_t> buf(len + 1);
                DragQueryFileW(drop, i, buf.data(), len + 1);
                pd.paths.emplace_back(buf.data(), len);
            }
            DragFinish(drop); // 分岐より先に必ず解放 (シェル側の HDROP リーク防止)
            pendingFileDrops_.push_back(std::move(pd));
            result = 0;
            return true;
        }
        return false;
    });

    MYE_LOG_INFO("EditorApp started (%u entities)", ctx.scene->GetWorld().AliveCount());
}

void EditorApp::OnTick(EngineContext& ctx)
{
    // ゲームロジック (GameLogic.dll のスクリプト) は Play 中のみ実行される
    ctx.simulateScripts = playMode_.ConsumeSimulateTick();
}

void EditorApp::OnRenderViews(EngineContext& ctx)
{
    if (actorEdit_) {
        // ミニシーン編集モード (M48k): SceneView だけアセットのミニシーンを描く。
        // **ミニシーンはエンジンの tick に載っていない** ので、WorldMatrix はここで自前に
        // 更新する (TransformSystem は純関数なので World を渡すだけでよい)
        Scene* const mainScene = ctx.scene;
        ctx.scene = &actorEdit_->scene;
        actorEdit_->scene.GetWorld().ApplyStructuralChanges();
        actorTransform_.Update(actorEdit_->scene.GetWorld());
        sceneView_.OnRenderViews(ctx, selection_); // selection_ は編集中アセット側 (中身を入替済み)
        ctx.scene = mainScene;
    } else {
        sceneView_.OnRenderViews(ctx, selection_);
    }
    gameView_.OnRenderViews(ctx); // GameView は常に本シーン (編集モードでも実行結果を見せる)
    preview_.OnRenderViews(ctx);  // アセットサムネイル生成 (D3D 描画はこのフェーズのみ)

    // ---- 反射プローブのベイク (M56e) ----
    // ★ここでしか焼かない (メニューのコールバックから直接呼ばない — 理由は EditorApp.h)。
    //   焼くのは押された次の 1 フレームだけ = 「見えたら焼く」自動ベイクは存在しない
    if (probeBakeRequested_) {
        probeBakeRequested_ = false;
        probeBaker_.assetsRoot = ctx.assetsRoot;
        // キャプチャは Forward 固定 (共有 GBuffer を 128^2 へ縮めない。EngineLoop と同じ理由)
        if (probeBaker_.Bake(ctx.scene->GetWorld(), *ctx.device, *ctx.renderPathForward,
                             *ctx.shaders, *ctx.resources, sceneView_.CameraPosition(), 0.1f,
                             500.0f, probePreview_)) {
            showProbePreview_ = true;
            probePreviewAdHoc_ = true; // 窓は「今焼いたもの」を出す
            toasts_.Notify(LogLevel::Info, Tr(StrId::Probe_Baked));
        } else {
            toasts_.Notify(LogLevel::Warn, Tr(StrId::Probe_BakeFailed));
        }
    }
    // ---- M56f: シーンに置いた反射プローブを全部焼いて描画へ載せる ----
    // ★同じ理由でここでしか焼かない。焼き終えた束を RenderSystem へ向けるところまでが
    //   1 組 — 向けないと「焼いたのに何も変わらない」になる
    if (probeBakeAllRequested_) {
        probeBakeAllRequested_ = false;
        probeBaker_.assetsRoot = ctx.assetsRoot;
        if (ctx.renderSystem != nullptr) {
            // 作り直す前に参照を切る (BakeAll は先頭で束を破棄する = ぶら下がる)
            ctx.renderSystem->reflectionProbes = nullptr;
        }
        if (probeBaker_.BakeAll(ctx.scene->GetWorld(), *ctx.device, *ctx.renderPathForward,
                                *ctx.shaders, *ctx.resources, probeSet_)) {
            if (ctx.renderSystem != nullptr) {
                ctx.renderSystem->reflectionProbes = &probeSet_.set;
            }
            probePreviewAdHoc_ = false;
            probePreviewIndex_ = 0;
            showProbePreview_ = true;
            toasts_.Notify(LogLevel::Info, Tr(StrId::Probe_BakedAll));
        } else {
            toasts_.Notify(LogLevel::Warn, Tr(StrId::Probe_BakeAllFailed));
        }
    }
}

void EditorApp::OnShutdown(EngineContext& ctx)
{
    // M56f: 束を捨てる前に参照を切る → デバイスが生きているうちにテクスチャを解放する
    if (ctx.renderSystem != nullptr) {
        ctx.renderSystem->reflectionProbes = nullptr;
    }
    probeSet_.Clear();
    probePreview_ = {};
    probeBaker_.Shutdown();
}

// 焼いた 6 面のサムネイル (M56e)。**Inspector ではなく専用の小窓**にしてある —
// 反射プローブのコンポーネント自体は M56f で入るので、それまで「どのエンティティの
// インスペクタに出すか」が決まらない。並びは十字 (ProbeWriteFacesPng と同一) で、
// 隣り合う面が画面上でも隣り合う = 面の向きが壊れていれば継ぎ目の段差で分かる
void EditorApp::DrawProbePreview()
{
    if (!showProbePreview_) {
        return;
    }
    if (!ImGui::Begin(Tr(StrId::Probe_Preview), &showProbePreview_)) {
        ImGui::End();
        return;
    }
    // M56f: 束を焼いてあるならそちらを出す (「ここでベイク」の 1 個はシーンのプローブとは
    // 別物なので、どちらを見ているかを probePreviewAdHoc_ で分けている)
    const int setCount = static_cast<int>(probeSet_.probes.size());
    const bool showSet = !probePreviewAdHoc_ && setCount > 0;
    if (showSet && probePreviewIndex_ >= setCount) {
        probePreviewIndex_ = setCount - 1;
    }
    if (showSet && probePreviewIndex_ < 0) {
        probePreviewIndex_ = 0;
    }
    const BakedProbe& shown = showSet ? probeSet_.probes[static_cast<size_t>(probePreviewIndex_)]
                                      : probePreview_;
    if (!shown.valid) {
        ImGui::TextUnformatted(Tr(StrId::Probe_NotBaked));
        ImGui::End();
        return;
    }
    if (showSet) {
        ImGui::Text(Tr(StrId::Probe_SetCount), setCount);
        if (setCount > 1) {
            ImGui::SliderInt(Tr(StrId::Probe_PreviewIndex), &probePreviewIndex_, 0, setCount - 1);
        }
    }
    ImGui::Text(Tr(StrId::Probe_Position), shown.position.x, shown.position.y, shown.position.z);
    ImGui::Text(Tr(StrId::Probe_BakeMs), probeBaker_.LastBakeCpuMs());
    ImGui::TextUnformatted(Tr(StrId::Probe_HdrNote));
    static const int kCell[3][4] = {
        { -1, 2, -1, -1 }, // +Y
        { 1, 4, 0, 5 },    // -X +Z +X -Z (水平に一周するパノラマ)
        { -1, 3, -1, -1 }, // -Y
    };
    const float side = 88.0f;
    const ImVec2 base = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(side * 4.0f, side * 3.0f)); // 先に領域を確保 (スクロール量が正しく出る)
    for (int cy = 0; cy < 3; ++cy) {
        for (int cx = 0; cx < 4; ++cx) {
            const int f = kCell[cy][cx];
            if (f < 0 || !shown.faceSrv[f]) {
                continue;
            }
            ImGui::SetCursorScreenPos(
                ImVec2(base.x + static_cast<float>(cx) * side, base.y + static_cast<float>(cy) * side));
            ImGui::Image(reinterpret_cast<ImTextureID>(shown.faceSrv[f].Get()),
                         ImVec2(side, side));
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(base.x, base.y + side * 3.0f));
    ImGui::End();
}

// ---- ミニシーン編集モード (M48k) ----

void EditorApp::OpenActorEdit(EngineContext& ctx, const std::wstring& path)
{
    if (playMode_.State() != PlayState::Editing) {
        toasts_.Notify(LogLevel::Warn, "アセットを編集するには再生を停止してください");
        return;
    }
    if (actorEdit_) {
        CloseActorEdit(ctx); // 別アセットへ乗り換え (編集モードの入れ子は v1 非対応)
    }
    const uint64_t hash = ctx.prefabs->LoadFromFile(path); // 未登録なら登録 (冪等)
    const PrefabAsset* asset = (hash != 0) ? ctx.prefabs->Get(hash) : nullptr;
    if (asset == nullptr) {
        toasts_.Notify(LogLevel::Error,
                       "アセットを開けませんでした: "
                           + WideToUtf8(std::filesystem::path(path).filename().wstring()));
        return;
    }

    auto edit = std::make_unique<ActorEdit>();
    edit->path = path;
    edit->name = asset->name;
    edit->actorFormat = asset->actorFormat;
    // MakeEditDocument が nextFileId を max(localId)+1 にする — 編集中に足した
    // エンティティが既存 localId と衝突すると、配置済みインスタンスの PrefabLink が壊れる
    if (!SceneSerializer::LoadFromJson(edit->scene, Prefab::MakeEditDocument(*asset))) {
        toasts_.Notify(LogLevel::Error, "アセットの展開に失敗しました");
        return;
    }
    edit->scene.GetWorld().ApplyStructuralChanges();

    // 外側の編集状態を退避して、アセット用の空の Undo/Selection に差し替える
    edit->outerSelection = std::move(selection_);
    edit->outerUndo = std::move(undo_);
    edit->outerSavedSerial = savedStateSerial_;
    selection_ = Selection{};
    undo_ = UndoStack{};
    undo_.SetPrefabLibrary(ctx.prefabs); // 入れ子インスタンスの override 記録 (M48e)
    savedStateSerial_ = undo_.StateSerial(); // 開いた直後 = clean

    actorEdit_ = std::move(edit);
    MYE_LOG_INFO("[actor] editing '%s' (%s)", actorEdit_->name.c_str(),
                 WideToUtf8(actorEdit_->path).c_str());
    toasts_.Notify(LogLevel::Info, "アセットを編集中: " + actorEdit_->name);
}

void EditorApp::SaveActorEdit(EngineContext& ctx)
{
    if (!actorEdit_) {
        return;
    }
    actorEdit_->scene.GetWorld().ApplyStructuralChanges();
    if (Prefab::SaveEdited(actorEdit_->scene, *ctx.prefabs, actorEdit_->path, actorEdit_->name,
                           actorEdit_->actorFormat)) {
        savedStateSerial_ = undo_.StateSerial(); // 現在の状態が保存済み基準になる
        // 配置済みインスタンスへの伝播は ReloadHub (ファイル監視) の既存経路が拾う
        toasts_.Notify(LogLevel::Info, "アセットを保存しました: " + actorEdit_->name);
    } else {
        toasts_.Notify(LogLevel::Error, "アセットを保存できませんでした");
    }
}

void EditorApp::CloseActorEdit(EngineContext& ctx)
{
    (void)ctx;
    if (!actorEdit_) {
        return;
    }
    // 未保存分はここで捨てる (保存はツールバーの保存ボタン / Ctrl+S。
    // dirty なら呼び出し側が確認モーダルを通している)
    selection_ = std::move(actorEdit_->outerSelection);
    undo_ = std::move(actorEdit_->outerUndo);
    savedStateSerial_ = actorEdit_->outerSavedSerial;
    const std::string name = actorEdit_->name;
    actorEdit_.reset();
    MYE_LOG_INFO("[actor] closed '%s' - back to the scene", name.c_str());
}

void EditorApp::OnImGui(EngineContext& ctx)
{
    // ---- ミニシーン編集モード (M48k) ----
    // 全ウィンドウの描画の間だけ `ctx.scene` をアセットのミニシーンへ差し替える。
    // Hierarchy / Inspector / SceneView / AssetBrowser の配置は ctx.scene 駆動なので、
    // これだけで丸ごと「アセットを編集する側」に回る。
    // ★差し替えるのは**この関数の中だけ**: ScriptHost / ManagedHost / ReloadHub が Init 時に
    //   捕まえた Scene* と、EngineLoop がローカルに持つ Scene には触れないので tick 経路は無傷
    Scene* const mainScene = ctx.scene;
    if (actorEdit_) {
        ctx.scene = &actorEdit_->scene;
    }
    // アセットファイル操作エントリの実行に使う (M51i)。毎フレーム設定で常に最新を保証
    undo_.SetFileOpContext(&ctx);

    // ---- タイムトラベルのリング (M52e) ----
    // Play 中だけ回す (編集中は sim が進まないので撮っても意味が無く、
    // ミニシーン編集中は Play 自体が禁止されている)。
    // Stop で SetEnabled(false) → リングごと破棄 = Play セッションを跨いで持ち越さない。
    // ★スクラブ中は EngineLoop が tick を止めているので、再生/ステップの再開要求は
    //   ここ (フレーム側) で拾って明示的にスクラブを抜ける = そこから分岐する
    if (ctx.timeTravel != nullptr) {
        ctx.timeTravel->SetEnabled(playMode_.InPlayMode());
        const PlayState playState = playMode_.State();
        // ★見るのは「今 Playing か」ではなく **Paused → Playing の遷移**。
        //   スクラブは必ずポーズを伴うので、状態だけで判定すると --autoplay のように
        //   最初から Playing のままの経路でシーク要求が即座に取り消される
        if (ctx.timeTravel->Scrubbing()
            && ((playState == PlayState::Playing && prevPlayState_ != PlayState::Playing)
                || playMode_.StepPending())) {
            ctx.timeTravel->EndScrub();
        }
        prevPlayState_ = playState;
    }

    // レイアウトロードは **どの ImGui::Begin よりも前** に実行する — LoadIniSettingsFromDisk が
    // 既存ウィンドウの DockId を差し替え、同フレームの Begin で新ドックに再バインドされる
    layouts_.ApplyPendingLoad();

    // エクスプローラー D&D の消費はグリッド描画 (assetBrowser_) より先に行い、
    // インポートしたファイルが同フレームで表示されるようにする
    ProcessPendingFileDrops(ctx);

    // サイドバー (メニュー → ツールバー → ステータスバー) が WorkArea を先に確保し、
    // 残りに DockSpace を敷く — この順序を同一フレーム内で守ること (逆順だと 1 フレームちらつく)
    DrawMainMenuBar(ctx);
    ActorEditBar editBar;
    if (actorEdit_) {
        editBar.name = actorEdit_->name.c_str();
        editBar.dirty = IsSceneDirty(); // 入替済みなので「今開いている文書」の dirty
    }
    if (toolbar_.OnImGui(ctx, playMode_, selection_, undo_, sceneView_, layouts_, &editBar)) {
        rebuildDockLayout_ = true; // レイアウトの「リセット (既定)」
    }
    if (editBar.saveRequested) {
        SaveActorEdit(ctx);
    }
    // ★exitRequested はここでは処理しない — clean 時の RequestGuardedAction は即時実行で、
    //   CloseActorEdit が actorEdit_->scene (World) をその場で破棄する。ctx.scene は
    //   ミニシーンに差し替え中なので、フレーム残りのウィンドウ描画が破棄済み World を触って
    //   落ちる (M51a のクエリキャッシュ挿入で顕在化した M48k 由来の use-after-free)。
    //   処理は本シーンへ戻した後 (下の ctx.scene = mainScene 直後) で行う
    statusBar_.OnImGui(ctx, projectName_, scenePath_, IsSceneDirty(), playMode_.State(),
                       &console_.open);

    const ImGuiID dockspaceId =
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_None);
    if (rebuildDockLayout_) {
        SetupDockLayout(dockspaceId);
        rebuildDockLayout_ = false;
    }
    ImGuizmo::BeginFrame(); // ImGui NewFrame 後・ギズモ使用前に 1 回

    hierarchy_.OnImGui(ctx, selection_, undo_);
    inspector_.OnImGui(ctx, selection_, undo_, preview_);
    console_.OnImGui(settings_.externalEditorCmd);
    sceneView_.OnImGui(ctx, selection_, undo_, settings_);
    gameView_.OnImGui(ctx, selection_);
    particleSettings_.OnImGui(ctx);
    profiler_.OnImGui(ctx);
    timeline_.OnImGui(ctx, playMode_);
    net_.OnImGui(ctx);
    DrawProbePreview();
    assetBrowser_.OnImGui(ctx, selection_, undo_, settings_.externalEditorCmd, preview_);
    // AssetBrowser で .scene.json がダブルクリックされたら未保存変更ガード経由で開く
    if (std::wstring p = assetBrowser_.TakePendingOpenScene(); !p.empty()) {
        pendingOpenScenePath_ = std::move(p);
        RequestGuardedAction(ctx, PendingAction::OpenSceneAsset);
    }
    // 構成アセットのダブルクリック → ミニシーン編集モード (M48k)
    if (std::wstring p = assetBrowser_.TakePendingOpenActor(); !p.empty()) {
        OpenActorEdit(ctx, p);
        // ★編集中に別 actor へ乗り換えた場合、OpenActorEdit 冒頭の CloseActorEdit が
        //   直前まで ctx.scene が指していた旧ミニシーンの World を破棄している。
        //   残りのフレームが dangling を触らないよう、今の文書 (新ミニシーン、開けなければ
        //   本シーン) へ即座に張り替える (exitRequested の UAF fix e7a584f と同族)
        ctx.scene = actorEdit_ ? &actorEdit_->scene : mainScene;
    }
    animation_.OnImGui(ctx, selection_, undo_);
    animatorController_.OnImGui(ctx, selection_);
    search_.OnImGui(ctx, selection_);
    projectSettings_.OnImGui(ctx, settings_, shortcuts_);
    buildSettings_.OnImGui(ctx);
    // --package (M51j): CLI パッケージが完了したら結果を出して終了する (CI/検証用)
    if (!packageDir.empty() && buildSettings_.PipelineFinished()) {
        const bool ok = buildSettings_.PipelineSucceeded();
        MYE_LOG_INFO("[build] CLI package result: %s", ok ? "PASS" : "FAIL");
        packageExitCode = ok ? 0 : 1; // M52b: CI が終了コードで判定できるようにする
        packageDir.clear(); // 多重報告を防ぐ
        ctx.requestExit = true;
    }
    // 書き出し先は AssetBrowser の表示中フォルダ (未初期化なら窓側が <assets>\audio に落とす)
    soundGen_.OnImGui(ctx, assetBrowser_.CurrentDir());
    // Asset Browser で .mixer.json がダブルクリックされたら Audio Mixer を開く (M45d)
    if (assetBrowser_.TakeOpenMixerRequest()) {
        audioMixer_.FocusOnActive();
    }
    audioMixer_.OnImGui(ctx);

    // ピッキング自動テスト (--pick-test): 指定フレームでビュー中心を選択できるか検証
    if (pickTestFrame >= 0 && static_cast<int64_t>(ctx.frameIndex) == pickTestFrame) {
        if (sceneView_.PickAtCenter(ctx, selection_)) {
            GameObject g = ctx.scene->FindByFileId(selection_.primary);
            MYE_LOG_INFO("PICK TEST: PASS -- hit '%s' (fileId %llu)", g ? g.Name() : "?",
                         static_cast<unsigned long long>(selection_.primary));
        } else {
            MYE_LOG_ERROR("PICK TEST: FAIL -- no entity at view center");
        }
    }

    if (showStats_) {
        if (ImGui::Begin(Tr(StrId::Win_Stats), &showStats_)) {
            const ImGuiIO& io = ImGui::GetIO();
            ImGui::Text(Tr(StrId::Stats_Fps), io.Framerate, 1000.0f / io.Framerate);
            ImGui::Text(Tr(StrId::Stats_Frame),
                        static_cast<unsigned long long>(ctx.frameIndex),
                        static_cast<unsigned long long>(ctx.tickIndex));
            ImGui::Text(Tr(StrId::Stats_Entities), ctx.scene->GetWorld().AliveCount());
            const char* stateName = Tr(StrId::Stats_Editing);
            if (playMode_.State() == PlayState::Playing) { stateName = Tr(StrId::Stats_Playing); }
            if (playMode_.State() == PlayState::Paused) { stateName = Tr(StrId::Stats_Paused); }
            ImGui::Text(Tr(StrId::Stats_PlayState), stateName);
            ImGui::Text(Tr(StrId::Stats_GameLogic),
                        ctx.scriptHost->IsLoaded() ? "loaded" : "not loaded",
                        ctx.dllReloader->Version(), ctx.scriptHost->ScriptTypeCount());
        }
        ImGui::End();
    }

    // ---- 以降はアプリのライフサイクル (シーン読み書き / 終了) なので本シーンに戻す ----
    ctx.scene = mainScene;

    // ミニシーン編集の「戻る」(M48k)。**必ず ctx.scene 復帰後に処理する** — clean 時は
    // 即時実行 = CloseActorEdit がミニシーンの World を破棄するため、差し替え中に実行すると
    // フレーム残りが破棄済み World を触って落ちる。dirty 時のモーダル実行 (DrawSaveConfirmModal)
    // も同じくこの位置以降なので、経路が二つとも復帰後に揃う
    if (editBar.exitRequested) {
        RequestGuardedAction(ctx, PendingAction::ExitActorEdit); // dirty なら確認モーダル
    }

    // ---- フィードバック層 (M27b): 最前面に描く ----
    if (closeRequested_) {
        closeRequested_ = false;
        RequestGuardedAction(ctx, PendingAction::Exit);
    }
    DrawSaveConfirmModal(ctx);
    PollReloadToasts(ctx);
    UpdateWindowTitle(ctx);
    toasts_.OnImGui();
}

void EditorApp::DrawMainMenuBar(EngineContext& ctx)
{
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu(Tr(StrId::Menu_File))) {
        // New/Open/Exit は未保存変更ガード経由 (M27b。dirty なら確認モーダル)。
        // ミニシーン編集中 (M48k) はシーンの入れ替え系を無効化する — 編集モードを
        // 抜けないままシーンを差し替えると、どちらの文書を編集しているのか破綻する
        ImGui::BeginDisabled(actorEdit_ != nullptr);
        if (ImGui::MenuItem(Tr(StrId::Menu_NewScene))) {
            RequestGuardedAction(ctx, PendingAction::NewScene);
        }
        if (ImGui::MenuItem(Tr(StrId::Menu_OpenScene))) {
            RequestGuardedAction(ctx, PendingAction::OpenScene);
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        // 保存は編集モードでも有効 (SaveCurrentScene がアセット保存へ分岐する)
        if (ImGui::MenuItem(Tr(StrId::Menu_SaveScene), shortcuts_.Label(Shortcut::Save))) {
            SaveCurrentScene(ctx);
        }
        ImGui::BeginDisabled(actorEdit_ != nullptr);
        if (ImGui::MenuItem(Tr(StrId::Menu_SaveSceneAs))) {
            SaveSceneAs(ctx);
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::MenuItem(Tr(StrId::Menu_BuildSettings))) {
            buildSettings_.open = true;
        }
        if (ImGui::MenuItem(Tr(StrId::Menu_ProjectSettings))) {
            projectSettings_.open = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem(Tr(StrId::Menu_Exit))) {
            RequestGuardedAction(ctx, PendingAction::Exit);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(Tr(StrId::Menu_View))) {
        ImGui::MenuItem(Tr(StrId::Win_Stats), nullptr, &showStats_);
        // UI 言語 (M47a)。ウィンドウ名は "表示名###安定ID" 形式なので、
        // 切り替えてもドッキング配置とパネル開閉状態は保たれる
        if (ImGui::BeginMenu(Tr(StrId::Menu_Language))) {
            const Lang cur = CurrentLanguage();
            for (const Lang lang : { Lang::Ja, Lang::En }) {
                const StrId label = (lang == Lang::Ja) ? StrId::Menu_LangJapanese
                                                       : StrId::Menu_LangEnglish;
                if (ImGui::MenuItem(Tr(label), nullptr, cur == lang) && cur != lang) {
                    SetLanguage(lang);
                    EditorGlobalSettings globals;
                    globals.Load(); // 他のキーを消さないよう読み直してから保存
                    globals.uiLanguage = lang;
                    globals.Save();
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(Tr(StrId::Menu_RenderPath))) {
            // 実行時切替 (M6.5)。描画のみの変更なのでリプレイ一貫性には影響しない
            const bool isForward = (ctx.renderPath == ctx.renderPathForward);
            if (ImGui::MenuItem(Tr(StrId::Menu_Forward), nullptr, isForward)) {
                ctx.renderPath = ctx.renderPathForward;
            }
            if (ImGui::MenuItem(Tr(StrId::Menu_Deferred), nullptr, !isForward)) {
                ctx.renderPath = ctx.renderPathDeferred;
            }
            ImGui::EndMenu();
        }
        // M54e: 影の内訳 (平行光 CSM / 局所ライトのアトラス) と統計。
        // M40d までは Rendering メニューの「影」1 個で全部を切っていたが、局所ライトの
        // アトラスは 4096^2 = 64MB + タイル数ぶんの深度パスという別勘定のコストなので、
        // CSM を残したまま局所影だけ外せる口が要る。旧トグルはここへ移動した
        if (ImGui::BeginMenu(Tr(StrId::Menu_Shadows))) {
            ImGui::MenuItem(Tr(StrId::Shadow_Directional), nullptr,
                            &ctx.renderSystem->enableShadows);
            // 親 (enableShadows) が off なら局所影も出ないので、子は無効表示にする
            ImGui::BeginDisabled(!ctx.renderSystem->enableShadows);
            ImGui::MenuItem(Tr(StrId::Shadow_LocalLights), nullptr,
                            &ctx.renderSystem->enableLocalShadows);
            ImGui::EndDisabled();
            ImGui::Separator();
            // 統計はヘッドレス撮影では読めない (ProfilerWindow も同じ値を出す)。
            // ★Tr() を書式文字列として渡している = 訳文の % が指定子として解釈される。
            //   ここは意図した書式付き文字列なので正しい (規則 10 が並びを機械検査する)
            const int tiles = ctx.renderSystem->ShadowAtlasTiles();
            if (tiles > 0) {
                ImGui::TextDisabled(Tr(StrId::Shadow_AtlasStats), tiles,
                                    ctx.renderSystem->ShadowAtlasDraws(),
                                    ctx.renderSystem->ShadowAtlasCulledDraws());
            } else {
                ImGui::TextUnformatted(Tr(StrId::Shadow_AtlasIdle));
            }
            ImGui::TextDisabled(Tr(StrId::Shadow_AtlasGpu), ctx.renderSystem->ShadowCsmGpuMs(),
                                ctx.renderSystem->ShadowAtlasGpuMs());
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(Tr(StrId::Menu_Rendering))) {
            // 描画専用トグル (M40d)。sim/hash 非影響
            ImGui::MenuItem(Tr(StrId::Menu_Ssao), nullptr, &ctx.renderSystem->enableSsao);
            ImGui::MenuItem(Tr(StrId::Menu_GpuInstancing), nullptr, &ctx.renderSystem->enableInstancing);
            ImGui::MenuItem(Tr(StrId::Menu_PostFx), nullptr, &ctx.renderSystem->enablePostFx);
            // M46f: レイトレ拡散 GI を最終画像へ合成。off なら BVH の構築すら走らない。
            // 品質パラメータ (解像度/バウンス/蓄積/SVGF) は RT Debug メニュー側と共通
            ImGui::MenuItem(Tr(StrId::Menu_RtGi), nullptr, &ctx.renderSystem->enableRtGi);
            // M46g: 平行光の影を CSM でなくレイトレの可視率で作る (カスケード境界が消える)
            ImGui::MenuItem(Tr(StrId::Menu_RtShadow), nullptr, &ctx.renderSystem->enableRtShadow);
            // M46h: 滑らかな面のスペキュラ環境項をレイトレ反射で置換 (画面外も映る)
            ImGui::MenuItem(Tr(StrId::Menu_RtReflection), nullptr, &ctx.renderSystem->enableRtRefl);
            // M55c: GBuffer RT4 (velocity) の可視化。bool ではなく int なので MenuItem の
            // 選択状態で表し、クリックでトグルする (rtDebugMode の 0/N と同じ流儀)
            {
                int& velDbg = ctx.renderSystem->velocityDebugMode;
                if (ImGui::MenuItem(Tr(StrId::Taa_VelocityDebug), nullptr, velDbg != 0)) {
                    velDbg = (velDbg != 0) ? 0 : 1;
                }
            }
            // M55d: TAA (グローバル設定。シーンカメラに CameraPostFx があればそちらが勝つ)。
            // カメラジッタもこのトグルと連動する — 別々に切れると「TAA 無しでジッタだけ」
            // = 画面が半ピクセル揺れるだけの状態を作れてしまう
            {
                int& taa = ctx.renderSystem->postFxSettings.taaOn;
                if (ImGui::MenuItem(Tr(StrId::Taa_Enable), nullptr, taa != 0)) {
                    taa = (taa != 0) ? 0 : 1;
                }
            }
            // M56c: HZB (min-Z ピラミッド) の可視化。段を選べないと「段が積めているか」を
            // 確かめられない (velocity と違い、絵が変わらないのが正常な段も混ざる) ので
            // トグルではなくサブメニューにする。ピラミッド自体もここが 0 でない間しか組まない
            if (ImGui::BeginMenu(Tr(StrId::Hzb_Debug))) {
                int& hzb = ctx.renderSystem->hzbDebugMip;
                if (ImGui::MenuItem(Tr(StrId::Hzb_DebugOff), nullptr, hzb == 0)) {
                    hzb = 0;
                }
                for (int m = 0; m < kHzbDebugMenuMips; ++m) {
                    // 訳文が書式 ("Mip %d" / "ミップ %d")。ImGui の ID はラベル全体なので、
                    // 段ごとに文字列が違う = 衝突しない (言語で ID が変わるが、この
                    // メニューは状態を持たないので影響しない)
                    char label[32] = {};
                    std::snprintf(label, sizeof(label), Tr(StrId::Hzb_DebugMip), m);
                    if (ImGui::MenuItem(label, nullptr, hzb == m + 1)) {
                        hzb = m + 1;
                    }
                }
                ImGui::EndMenu();
            }
            // M56d: SSR (スクリーンスペース反射)。グローバル設定で、シーンカメラに
            // CameraPostFx があればそちらの ssrOn が勝つ (TAA と同じ規則)。
            // on の間は HZB (min-Z ピラミッド) も一緒に組まれる
            ImGui::MenuItem(Tr(StrId::Ssr_Enable), nullptr, &ctx.renderSystem->enableSsr);
            // M56e: 反射プローブを「今 SceneView が居る場所」で焼く。
            // ★トグルではなく**明示ボタン**。自動ベイクにすると撮影ごとに焼き上がりが
            //   変わって決定的撮影 (M52c) が壊れるので、口はここ 1 つだけにしてある。
            //   押しても即座には焼かない — 次の OnRenderViews まで要求を持ち越す
            if (ImGui::MenuItem(Tr(StrId::Probe_BakeHere))) {
                probeBakeRequested_ = true;
            }
            // M56f: シーンに置いた ReflectionProbeComponent を全部焼いて描画へ載せる。
            // ★ここも**明示ボタン**。焼くまでプローブは絵に 1 ビットも寄与しない
            if (ImGui::MenuItem(Tr(StrId::Probe_BakeAll))) {
                probeBakeAllRequested_ = true;
            }
            // 焼いた束を捨てる (A/B 比較用。プローブ有り/無しをその場で見比べられる)
            if (ImGui::MenuItem(Tr(StrId::Probe_ClearBaked), nullptr, false,
                                !probeSet_.probes.empty())) {
                ctx.renderSystem->reflectionProbes = nullptr; // 先に参照を切る
                probeSet_.Clear();
                probePreviewAdHoc_ = true;
            }
            ImGui::MenuItem(Tr(StrId::Probe_Preview), nullptr, &showProbePreview_);
            // M57e: ボリュメトリックフォグ (フロクセル)。CLI の --froxel と同じ元栓。
            // ★シーンカメラに CameraPostFx があれば **そちらの froxelOn が勝つ** (TAA と
            //   同じ規則) ので、ここを点けても効かないシーンがある
            if (ImGui::BeginMenu(Tr(StrId::Froxel_Menu))) {
                ImGui::MenuItem(Tr(StrId::Froxel_Enable), nullptr,
                                &ctx.renderSystem->enableFroxel);
                ImGui::TextDisabled("%s", Tr(StrId::Froxel_GodrayOff));
                ImGui::Separator();
                // 親が off ならボリュームすら作られない (遅延 Init) ので子は無効表示
                ImGui::BeginDisabled(!ctx.renderSystem->enableFroxel);
                FroxelSettings& fs = ctx.renderSystem->froxelSettings;
                // テンポラルとスライスジッタは **必ずセット**で切れる (片方だけだと霧が
                // 奥行き方向に脈打つだけになる)。FroxelSettings::temporal がその 1 本
                ImGui::MenuItem(Tr(StrId::Froxel_Temporal), nullptr, &fs.temporal);
                ImGui::SetNextItemWidth(160.0f);
                ImGui::SliderFloat(Tr(StrId::Froxel_Density), &fs.density, 0.0f, 0.2f, "%.4f");
                ImGui::SetNextItemWidth(160.0f);
                ImGui::SliderFloat(Tr(StrId::Froxel_Anisotropy), &fs.anisotropy, -0.9f, 0.9f,
                                   "%.2f");
                ImGui::EndDisabled();
                ImGui::Separator();
                // ★Tr() を書式文字列として渡している = 訳文の % が指定子として解釈される。
                //   ここは意図した書式付き文字列 (規則 10 が並びを機械検査する)
                ImGui::TextDisabled(Tr(StrId::Froxel_Grid), froxel::kGridX, froxel::kGridY,
                                    froxel::kGridZ,
                                    froxel::kGridX * froxel::kGridY * froxel::kGridZ);
                ImGui::TextDisabled(Tr(StrId::Froxel_Gpu), ctx.renderSystem->FroxelInjectGpuMs(),
                                    ctx.renderSystem->FroxelTemporalGpuMs(),
                                    ctx.renderSystem->FroxelIntegrateGpuMs());
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(Tr(StrId::Menu_RtDebug))) {
            // M46b: BVH 検証用の可視化。off なら BVH の構築も GPU 転送も走らない
            int& mode = ctx.renderSystem->rtDebugMode;
            if (ImGui::MenuItem(Tr(StrId::Menu_RtDbgOff), nullptr, mode == 0)) {
                mode = 0;
            }
            if (ImGui::MenuItem(Tr(StrId::Menu_RtDbgBvhHeat), nullptr, mode == 1)) {
                mode = 1;
            }
            if (ImGui::MenuItem(Tr(StrId::Menu_RtDbgNormals), nullptr, mode == 2)) {
                mode = 2;
            }
            if (ImGui::MenuItem(Tr(StrId::Menu_RtDbgInstanceId), nullptr, mode == 3)) {
                mode = 3;
            }
            if (ImGui::MenuItem(Tr(StrId::Menu_RtDbgRawGi), nullptr, mode == 4)) {
                mode = 4;
            }
            // M46d: 蓄積結果と履歴長 (赤=履歴なし → 緑=上限まで蓄積) の可視化
            if (ImGui::MenuItem(Tr(StrId::Menu_RtDbgAccumGi), nullptr, mode == 5)) {
                mode = 5;
            }
            if (ImGui::MenuItem(Tr(StrId::Menu_RtDbgHistory), nullptr, mode == 6)) {
                mode = 6;
            }
            // M46e: SVGF 後の GI と、A-Trous を駆動している推定分散 (緑 = 収束)
            if (ImGui::MenuItem(Tr(StrId::Menu_RtDbgSvgfGi), nullptr, mode == 7)) {
                mode = 7;
            }
            if (ImGui::MenuItem(Tr(StrId::Menu_RtDbgVariance), nullptr, mode == 8)) {
                mode = 8;
            }
            // M46g: 太陽の可視率 (白 = 照らされる / 黒 = 影)。RT 影 off でも撃って表示する
            if (ImGui::MenuItem(Tr(StrId::Menu_RtDbgShadowVis), nullptr, mode == 9)) {
                mode = 9;
            }
            // M46h: 反射の生 1spp とデノイズ後 (roughness 超過の面は黒 = 撃っていない)
            if (ImGui::MenuItem(Tr(StrId::Menu_RtDbgRawRefl), nullptr, mode == 10)) {
                mode = 10;
            }
            if (ImGui::MenuItem(Tr(StrId::Menu_RtDbgSvgfRefl), nullptr, mode == 11)) {
                mode = 11;
            }
            ImGui::Separator();
            // M46c: GI の品質。解像度は内部バッファ、バウンスは二次光線の深さ
            float& scale = ctx.renderSystem->rtResolutionScale;
            if (ImGui::MenuItem(Tr(StrId::Menu_RtScale100), nullptr, scale > 0.9f)) {
                scale = 1.0f;
            }
            if (ImGui::MenuItem(Tr(StrId::Menu_RtScale50), nullptr, scale > 0.4f && scale <= 0.9f)) {
                scale = 0.5f;
            }
            if (ImGui::MenuItem(Tr(StrId::Menu_RtScale25), nullptr, scale <= 0.4f)) {
                scale = 0.25f;
            }
            ImGui::Separator();
            int& bounces = ctx.renderSystem->rtBounces;
            if (ImGui::MenuItem(Tr(StrId::Menu_RtBounce1), nullptr, bounces <= 1)) {
                bounces = 1;
            }
            if (ImGui::MenuItem(Tr(StrId::Menu_RtBounce2), nullptr, bounces >= 2)) {
                bounces = 2;
            }
            ImGui::Separator();
            // M46d: 蓄積を切ると 1spp の生ノイズが見える (A/B 比較用)
            ImGui::MenuItem(Tr(StrId::Menu_RtTemporal), nullptr, &ctx.renderSystem->rtTemporal);
            // M46e: 空間フィルタ。蓄積 off では幾何バッファが無いので連動して効かない
            ImGui::MenuItem(Tr(StrId::Menu_RtSvgf), nullptr, &ctx.renderSystem->rtSvgf);
            ImGui::MenuItem(Tr(StrId::Menu_RtFreezeSeed), nullptr, &ctx.renderSystem->rtFreezeSeed);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(Tr(StrId::Menu_ResetLayout))) {
            rebuildDockLayout_ = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(Tr(StrId::Menu_Edit))) {
        const bool editing = playMode_.State() == PlayState::Editing;
        if (ImGui::MenuItem(Tr(StrId::Menu_Undo), shortcuts_.Label(Shortcut::Undo), false,
                            editing && undo_.CanUndo())) {
            undo_.Undo(*ctx.scene, selection_);
        }
        if (ImGui::MenuItem(Tr(StrId::Menu_Redo), shortcuts_.Label(Shortcut::Redo), false,
                            editing && undo_.CanRedo())) {
            undo_.Redo(*ctx.scene, selection_);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(Tr(StrId::Menu_GameObject))) {
        DrawCreateMenuItems(ctx, selection_, undo_); // parent 省略 = ルート生成
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(Tr(StrId::Menu_Window))) {
        // 各パネルの表示トグル (閉じたパネルはここから再表示)。
        // ラベルはウィンドウ名と同じ StrId を使う ("###" 以降は表示されない)
        ImGui::MenuItem(Tr(StrId::Win_Hierarchy), nullptr, &hierarchy_.open);
        ImGui::MenuItem(Tr(StrId::Win_Inspector), nullptr, &inspector_.open);
        ImGui::MenuItem(Tr(StrId::Win_Console), nullptr, &console_.open);
        ImGui::MenuItem(Tr(StrId::Win_Scene), nullptr, &sceneView_.open);
        ImGui::MenuItem(Tr(StrId::Win_Game), nullptr, &gameView_.open);
        ImGui::MenuItem(Tr(StrId::Win_Assets), nullptr, &assetBrowser_.open);
        ImGui::MenuItem(Tr(StrId::Win_Animation), nullptr, &animation_.open);
        ImGui::MenuItem(Tr(StrId::Win_Animator), nullptr, &animatorController_.open);
        ImGui::MenuItem(Tr(StrId::Win_Search), nullptr, &search_.open);
        ImGui::MenuItem(Tr(StrId::Win_Profiler), nullptr, &profiler_.open);
        ImGui::MenuItem(Tr(StrId::Win_Timeline), nullptr, &timeline_.open);
        ImGui::MenuItem(Tr(StrId::Win_Net), nullptr, &net_.open);
        ImGui::MenuItem(Tr(StrId::Win_ParticleSettings), nullptr, &particleSettings_.open);
        ImGui::MenuItem(Tr(StrId::Win_SoundGenerator), nullptr, &soundGen_.open);
        ImGui::MenuItem(Tr(StrId::Win_AudioMixer), nullptr, &audioMixer_.open);
        ImGui::Separator();
        ImGui::MenuItem(Tr(StrId::Win_ProjectSettings), nullptr, &projectSettings_.open);
        ImGui::MenuItem(Tr(StrId::Win_BuildSettings), nullptr, &buildSettings_.open);
        ImGui::EndMenu();
    }

    // Play/Pause/Step は M27c でツールバー (EditorToolbar) へ移設
    ImGui::EndMainMenuBar();

    HandleShortcuts(ctx);
}

void EditorApp::HandleShortcuts(EngineContext& ctx)
{
    // テキスト入力中はエディタショートカットを無効化 (ImGui のテキスト編集/Undo を優先)
    if (ImGui::GetIO().WantTextInput) {
        return;
    }
    if (shortcuts_.Pressed(Shortcut::Save)) {
        SaveCurrentScene(ctx);
    }
    // Undo/Redo・編集操作は編集モードのみ (Play 中の変更は Stop で破棄されるため)
    if (playMode_.State() == PlayState::Editing) {
        if (shortcuts_.Pressed(Shortcut::Undo)) {
            undo_.Undo(*ctx.scene, selection_);
        }
        if (shortcuts_.Pressed(Shortcut::Redo)
            || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z)) {
            undo_.Redo(*ctx.scene, selection_);
        }
        if (shortcuts_.Pressed(Shortcut::Duplicate)) {
            DuplicateSelection(ctx);
        }
        if (shortcuts_.Pressed(Shortcut::Copy)) {
            CopySelection(ctx);
        }
        if (shortcuts_.Pressed(Shortcut::Cut)) {
            CutSelection(ctx);
        }
        if (shortcuts_.Pressed(Shortcut::Paste)) {
            PasteClipboard(ctx);
        }
        if (shortcuts_.Pressed(Shortcut::Delete)) {
            DeleteSelection(ctx);
        }
    }
}

void EditorApp::SaveCurrentScene(EngineContext& ctx)
{
    // ミニシーン編集中は「保存 = アセットの保存」(M48k)。File メニュー / Ctrl+S /
    // 未保存確認モーダルの「保存」が全部ここを通るので、分岐はこの 1 箇所で足りる
    if (actorEdit_) {
        SaveActorEdit(ctx);
        return;
    }
    std::filesystem::create_directories(std::filesystem::path(scenePath_).parent_path());
    if (SceneSerializer::SaveToFile(*ctx.scene, scenePath_)) {
        settings_.lastScenePath = WideToUtf8(scenePath_);
        settings_.Save();
        savedStateSerial_ = undo_.StateSerial(); // この状態が保存済み基準になる (M27b)
        toasts_.Notify(LogLevel::Info,
                       "シーンを保存しました: "
                           + WideToUtf8(std::filesystem::path(scenePath_).filename().wstring()));
    } else {
        toasts_.Notify(LogLevel::Error, "シーンの保存に失敗しました");
    }
}

nlohmann::json EditorApp::GatherSelectionSubtrees(EngineContext& ctx)
{
    nlohmann::json out = nlohmann::json::array();
    std::vector<uint64_t> seen;
    for (uint64_t fid : selection_.ids) {
        GameObject g = ctx.scene->FindByFileId(fid);
        if (!g) {
            continue;
        }
        nlohmann::json sub = SceneSerializer::SubtreeToJson(*ctx.scene, g.Id());
        for (auto& item : sub) {
            const uint64_t f = item.value("fileId", 0ull);
            if (f != 0 && std::find(seen.begin(), seen.end(), f) != seen.end()) {
                continue; // 親と子を同時選択した場合の重複を除く
            }
            if (f != 0) {
                seen.push_back(f);
            }
            out.push_back(std::move(item));
        }
    }
    return out;
}

// 複製 / 貼り付けで生えた各ルートの名前を兄弟内で一意にする (M48b)。
// **1 つずつ改名してから次を計算する** — 先に全候補を計算すると 2 つ目以降も同じ " (1)" を取る。
// CloneSubtree + ApplyStructuralChanges の後、CaptureAfter より前に呼ぶこと
static void UniquifyNewRoots(Scene& scene, const std::vector<uint64_t>& newRoots)
{
    World& w = scene.GetWorld();
    for (uint64_t f : newRoots) {
        GameObject o = scene.FindByFileId(f);
        if (!o) {
            continue;
        }
        const EntityID e = o.Id();
        if (auto* nc = w.GetComponent<NameComponent>(e)) {
            SetEntityName(w, e, MakeUniqueSiblingName(w, w.GetParent(e), nc->value, /*exclude=*/e));
        }
    }
}

void EditorApp::DuplicateSelection(EngineContext& ctx)
{
    if (selection_.Empty()) {
        return;
    }
    const nlohmann::json subtrees = GatherSelectionSubtrees(ctx);
    if (subtrees.empty()) {
        return;
    }
    undo_.BeginRecord("Duplicate", selection_);
    const std::vector<uint64_t> newRoots = SceneSerializer::CloneSubtree(*ctx.scene, subtrees);
    ctx.scene->GetWorld().ApplyStructuralChanges();
    UniquifyNewRoots(*ctx.scene, newRoots);
    selection_.Clear();
    for (uint64_t f : newRoots) {
        selection_.Add(f);
        undo_.CaptureAfter(*ctx.scene, f);
    }
    undo_.EndRecord(selection_);
}

void EditorApp::CopySelection(EngineContext& ctx)
{
    clipboard_ = GatherSelectionSubtrees(ctx);
}

void EditorApp::CutSelection(EngineContext& ctx)
{
    CopySelection(ctx);
    DeleteSelection(ctx);
}

void EditorApp::PasteClipboard(EngineContext& ctx)
{
    if (!clipboard_.is_array() || clipboard_.empty()) {
        return;
    }
    undo_.BeginRecord("Paste", selection_);
    const std::vector<uint64_t> newRoots = SceneSerializer::CloneSubtree(*ctx.scene, clipboard_);
    ctx.scene->GetWorld().ApplyStructuralChanges();
    UniquifyNewRoots(*ctx.scene, newRoots);
    selection_.Clear();
    for (uint64_t f : newRoots) {
        selection_.Add(f);
        undo_.CaptureAfter(*ctx.scene, f);
    }
    undo_.EndRecord(selection_);
}

void EditorApp::DeleteSelection(EngineContext& ctx)
{
    if (selection_.Empty()) {
        return;
    }
    undo_.BeginRecord("Delete", selection_);
    for (uint64_t fid : selection_.ids) {
        GameObject g = ctx.scene->FindByFileId(fid);
        if (g) {
            undo_.CaptureBefore(*ctx.scene, fid);
            ctx.scene->GetWorld().DestroyEntity(g.Id());
        }
    }
    ctx.scene->GetWorld().ApplyStructuralChanges();
    selection_.Clear();
    undo_.EndRecord(selection_); // CaptureAfter 無し → destroyed 扱い
}

void EditorApp::SetupDockLayout(unsigned int dockspaceId)
{
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

    ImGuiID center = dockspaceId;
    const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
    const ImGuiID bottomRight = ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Right, 0.4f, nullptr, &bottom);
    ImGuiID rightBottom = right;
    const ImGuiID rightTop = ImGui::DockBuilderSplitNode(rightBottom, ImGuiDir_Up, 0.7f, nullptr, &rightBottom);

    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Search", left);
    ImGui::DockBuilderDockWindow("Inspector", rightTop);
    ImGui::DockBuilderDockWindow("Stats", rightBottom);
    ImGui::DockBuilderDockWindow("Particle Settings", rightBottom);
    ImGui::DockBuilderDockWindow("Sound Generator", rightBottom);
    ImGui::DockBuilderDockWindow("Audio Mixer", bottom);
    ImGui::DockBuilderDockWindow("Profiler", rightBottom);
    ImGui::DockBuilderDockWindow("Timeline", bottom);
    ImGui::DockBuilderDockWindow("Console", bottom);
    ImGui::DockBuilderDockWindow("Assets", bottomRight);
    ImGui::DockBuilderDockWindow("Animation", bottom);
    ImGui::DockBuilderDockWindow("Animator", bottom);
    ImGui::DockBuilderDockWindow("Scene", center);
    ImGui::DockBuilderDockWindow("Game", center);
    ImGui::DockBuilderFinish(dockspaceId);
}

void EditorApp::SaveSceneAs(EngineContext& ctx)
{
    wchar_t path[MAX_PATH] = L"main.scene.json";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = static_cast<HWND>(ctx.window->Hwnd());
    ofn.lpstrFilter = L"Scene (*.scene.json)\0*.scene.json\0All (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    const std::wstring initialDir = ctx.assetsRoot + L"\\scenes";
    ofn.lpstrInitialDir = initialDir.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT;
    if (GetSaveFileNameW(&ofn)) {
        scenePath_ = path;
        ctx.reloadHub->SetActiveScenePath(scenePath_);
        SaveCurrentScene(ctx);
    }
}

bool EditorApp::OpenScene(EngineContext& ctx)
{
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = static_cast<HWND>(ctx.window->Hwnd());
    ofn.lpstrFilter = L"Scene (*.scene.json)\0*.scene.json\0All (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    const std::wstring initialDir = ctx.assetsRoot + L"\\scenes";
    ofn.lpstrInitialDir = initialDir.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) {
        return false; // キャンセル (シーンは無変更なので dirty 状態も保持)
    }
    return LoadSceneFromPath(ctx, path);
}

// ファイルダイアログを経ない共通ロード経路 (OpenScene と AssetBrowser ダブルクリックが使う)
bool EditorApp::LoadSceneFromPath(EngineContext& ctx, const std::wstring& path)
{
    selection_.Clear();
    undo_.ClearAll();
    if (!SceneSerializer::LoadFromFile(*ctx.scene, path)) {
        toasts_.Notify(LogLevel::Error,
                       "シーンを開けませんでした: "
                           + WideToUtf8(std::filesystem::path(path).filename().wstring()));
        return false;
    }
    Prefab::RefreshNonOverridden(*ctx.scene, *ctx.prefabs); // ロード直後 1 回 (M48e)
    scenePath_ = path;
    ctx.reloadHub->SetActiveScenePath(scenePath_);
    settings_.lastScenePath = WideToUtf8(scenePath_);
    settings_.Save();
    return true;
}

// ---- 未保存変更ガード + フィードバック層 (M27b) ----

void EditorApp::RequestGuardedAction(EngineContext& ctx, PendingAction action)
{
    if (!IsSceneDirty()) {
        ExecuteAction(ctx, action);
        return;
    }
    pendingAction_ = action;
    openSaveConfirm_ = true;
}

void EditorApp::ExecuteAction(EngineContext& ctx, PendingAction action)
{
    switch (action) {
    case PendingAction::NewScene:
        selection_.Clear();
        undo_.ClearAll();
        ctx.scene->Clear();
        savedStateSerial_ = undo_.StateSerial(); // 空シーン = clean
        break;
    case PendingAction::OpenScene:
        if (OpenScene(ctx)) {
            savedStateSerial_ = undo_.StateSerial(); // ロード直後 = clean
        }
        break;
    case PendingAction::OpenSceneAsset:
        if (LoadSceneFromPath(ctx, pendingOpenScenePath_)) {
            savedStateSerial_ = undo_.StateSerial(); // ロード直後 = clean
        }
        pendingOpenScenePath_.clear();
        break;
    case PendingAction::Exit:
        ctx.requestExit = true;
        break;
    case PendingAction::ExitActorEdit:
        CloseActorEdit(ctx); // M48k
        break;
    default:
        break;
    }
}

void EditorApp::DrawSaveConfirmModal(EngineContext& ctx)
{
    if (openSaveConfirm_) {
        openSaveConfirm_ = false;
        ImGui::OpenPopup(Tr(StrId::Popup_UnsavedChanges));
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                   vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(Tr(StrId::Popup_UnsavedChanges), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::TextUnformatted(Tr(StrId::Confirm_UnsavedBody));
    // 編集モード中はアセットのパスを出す (保存されるのはシーンではなくアセットなので)
    ImGui::TextDisabled("%s",
                        WideToUtf8(actorEdit_ ? actorEdit_->path : scenePath_).c_str());
    ImGui::Spacing();
    const PendingAction action = pendingAction_;
    if (ImGui::Button(Tr(StrId::Confirm_Save), ImVec2(110, 0))) {
        SaveCurrentScene(ctx);
        pendingAction_ = PendingAction::None;
        ImGui::CloseCurrentPopup();
        if (!IsSceneDirty()) { // 保存成功時のみ続行 (失敗はトーストで通知済み)
            ExecuteAction(ctx, action);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Confirm_DontSave), ImVec2(110, 0))) {
        pendingAction_ = PendingAction::None;
        ImGui::CloseCurrentPopup();
        ExecuteAction(ctx, action);
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Common_Cancel), ImVec2(110, 0))) {
        pendingAction_ = PendingAction::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void EditorApp::UpdateWindowTitle(EngineContext& ctx)
{
    const bool dirty = IsSceneDirty();
    if (dirty == titleDirtyShown_ || baseTitle_.empty()) {
        return;
    }
    titleDirtyShown_ = dirty;
    const std::wstring title = dirty ? baseTitle_ + L" *" : baseTitle_;
    SetWindowTextW(static_cast<HWND>(ctx.window->Hwnd()), title.c_str());
}

void EditorApp::ProcessPendingFileDrops(EngineContext& ctx)
{
    if (pendingFileDrops_.empty()) {
        return;
    }
    for (PendingFileDrop& pd : pendingFileDrops_) {
        if (!pd.inClientArea || pd.paths.empty()) {
            continue; // タイトルバー等クライアント領域外へのドロップ
        }
        if (!assetBrowser_.open || assetBrowser_.CurrentDir().empty() ||
            !assetBrowser_.IsClientPosInPanel(pd.clientX, pd.clientY)) {
            // 黙って捨てると「無反応」に見えて原因調査が難しいため誘導トーストを出す
            toasts_.Notify(LogLevel::Warn, "インポートは Assets パネル上にドロップしてください");
            continue; // Assets パネル上のドロップのみインポートする (誤操作防止)
        }
        const ImportResult r = ImportExternalPaths(ctx, pd.paths, assetBrowser_.CurrentDir());
        if (r.imported > 0) {
            toasts_.Notify(LogLevel::Info,
                           std::to_string(r.imported) + " 件のアセットをインポートしました");
        }
        if (r.failed > 0) {
            toasts_.Notify(LogLevel::Error,
                           std::to_string(r.failed) + " 件のインポートに失敗しました");
        }
        if (r.imported == 0 && r.failed == 0 && r.skipped > 0) {
            toasts_.Notify(LogLevel::Warn,
                           "インポートをスキップしました (対象外またはドロップ元と同じフォルダ)");
        }
    }
    pendingFileDrops_.clear();
}

void EditorApp::PollReloadToasts(EngineContext& ctx)
{
    const uint32_t dllVersion = ctx.dllReloader->Version();
    if (dllVersion != lastDllVersion_) {
        lastDllVersion_ = dllVersion;
        toasts_.Notify(LogLevel::Info, "GameLogic をホットリロードしました (v"
                                           + std::to_string(dllVersion) + ")");
    }
    const uint64_t reloads = ctx.reloadHub->ReloadCount();
    if (reloads != lastReloadCount_) {
        lastReloadCount_ = reloads;
        toasts_.Notify(LogLevel::Info, "アセットをホットリロードしました");
    }
}

} // namespace mye
