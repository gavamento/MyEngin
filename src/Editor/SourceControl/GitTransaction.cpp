#include "Editor/SourceControl/GitTransaction.h"

#include <algorithm>
#include <filesystem>

#include "Editor/EditorSettings.h"
#include "Editor/PartTagNames.h"
#include "Editor/PhysicsLayerNames.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/AnimatorController.h"
#include "Engine/Engine/AssetDatabase.h"
#include "Engine/Engine/Audio/AudioSystem.h"
#include "Engine/Engine/EngineLoop.h"
#include "Engine/Engine/RenderSystem.h"
#include "Engine/Platform/InputActions.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/ImGuiTheme.h"

#include "imgui.h"

namespace fs = std::filesystem;

namespace mye {

namespace {

// 4 窓の直列化 (ファイル読み + JSON dump) の再評価間隔。
// ★毎フレームやると、Animation 窓を開いているだけで 60 Hz で JSON を作り続ける。
//   人間の操作 (保存 → もう一度押す) には 500 ms で十分間に合う
constexpr int kDirtyCacheMs = 500;

// パスを toplevel 相対 '/' 区切りに直す (リポジトリの外なら空)
std::string RelativeToRoot(const std::wstring& absPath, const std::wstring& root)
{
    if (absPath.empty() || root.empty()) {
        return {};
    }
    std::error_code ec;
    const fs::path rel = fs::relative(fs::path(absPath), fs::path(root), ec);
    if (ec || rel.empty()) {
        return {};
    }
    std::string out = WideToUtf8(rel.wstring());
    for (char& c : out) {
        if (c == '\\') {
            c = '/';
        }
    }
    if (out.rfind("..", 0) == 0) {
        return {}; // リポジトリの外
    }
    return out;
}

// 大小を無視した末尾一致 (suffix は小文字で渡すこと)。
// ★git は大小を保って返すが Windows のファイル系は区別しない = "Foo.PNG" と
//   "foo.png" は同じもの。ここで揃えないと、拡張子が大文字のときだけ分類から漏れる
bool EndsWithCase(const std::string& s, const char* suffix)
{
    const size_t n = std::char_traits<char>::length(suffix);
    if (s.size() < n) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        char a = s[s.size() - n + i];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (a != suffix[i]) {
            return false;
        }
    }
    return true;
}

} // namespace

std::vector<GateBlocker> ComputeBlockers(const GateInputs& in)
{
    std::vector<GateBlocker> out;
    // ★列挙順で全件。「最初の 1 件で止める」を選ぶと、直しては押し、直しては押しを
    //   繰り返すことになる (実際に阻んでいるのが 3 つあるのは珍しくない)
    auto add = [&out](bool cond, GateBlocker b) {
        if (cond) {
            out.push_back(b);
        }
    };
    add(in.sceneDirty, GateBlocker::SceneDirty);
    add(in.actorEdit, GateBlocker::ActorEdit);
    add(in.animationDirty, GateBlocker::AnimationDirty);
    add(in.controllerDirty, GateBlocker::ControllerDirty);
    add(in.mixerDirty, GateBlocker::MixerDirty);
    add(in.projectSettingsDirty, GateBlocker::ProjectSettingsDirty);
    add(in.playing, GateBlocker::Playing);
    add(in.netActive, GateBlocker::NetActive);
    add(in.buildRunning, GateBlocker::BuildRunning);
    add(in.scriptBuildRunning, GateBlocker::ScriptBuildRunning);
    add(in.opInFlight, GateBlocker::OpInFlight);
    add(in.mergeInProgress, GateBlocker::MergeInProgress);
    add(in.serviceUnavailable, GateBlocker::ServiceUnavailable);
    return out;
}

StrId GateBlockerText(GateBlocker b)
{
    switch (b) {
    case GateBlocker::SceneDirty:
        return StrId::GateB_SceneDirty;
    case GateBlocker::ActorEdit:
        return StrId::GateB_ActorEdit;
    case GateBlocker::AnimationDirty:
        return StrId::GateB_AnimationDirty;
    case GateBlocker::ControllerDirty:
        return StrId::GateB_ControllerDirty;
    case GateBlocker::MixerDirty:
        return StrId::GateB_MixerDirty;
    case GateBlocker::ProjectSettingsDirty:
        return StrId::GateB_ProjectSettingsDirty;
    case GateBlocker::Playing:
        return StrId::GateB_Playing;
    case GateBlocker::NetActive:
        return StrId::GateB_NetActive;
    case GateBlocker::BuildRunning:
        return StrId::GateB_BuildRunning;
    case GateBlocker::ScriptBuildRunning:
        return StrId::GateB_ScriptBuildRunning;
    case GateBlocker::OpInFlight:
        return StrId::GateB_OpInFlight;
    case GateBlocker::MergeInProgress:
        return StrId::GateB_MergeInProgress;
    case GateBlocker::ServiceUnavailable:
    default:
        return StrId::GateB_ServiceUnavailable;
    }
}

const DocumentDirty& GitTransaction::CachedDirty(const DirtyProbeFn& probe)
{
    const auto now = std::chrono::steady_clock::now();
    const bool expired = !dirtyValid_
        || std::chrono::duration_cast<std::chrono::milliseconds>(now - dirtyAt_).count()
            >= kDirtyCacheMs;
    if (expired && probe) {
        dirty_ = probe();
        dirtyAt_ = now;
        dirtyValid_ = true;
    }
    return dirty_;
}

bool GitTransaction::CanRunGitWriteOp(const GateInputs& in,
                                      std::vector<GateBlocker>& blockers) const
{
    blockers = ComputeBlockers(in);
    return blockers.empty();
}

void GitTransaction::UpdateActiveSceneRel()
{
    // ★予測 (RequestRevert / diff_names の応答) は ctx を持たないので、
    //   projectRoot_ は OnImGui が毎フレーム控えている値を使う。
    //   ここを実行時 (BeginOp) にしか更新しないと、**最初の 1 回の予測だけ**
    //   activeScene が空 = 「開いているシーンを戻すのに『その場で反映』と出る」
    activeSceneRel_ = hooks_.activeScenePath
        ? RelativeToRoot(hooks_.activeScenePath(), projectRoot_)
        : std::string();
}

void GitTransaction::RequestRevert(std::vector<std::string> paths, int untrackedCount)
{
    if (paths.empty() || phase_ != Phase::Idle) {
        return;
    }
    op_ = OpKind::Revert;
    target_.clear();
    checkoutChanges_.clear();
    reportPaths_.clear();
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    paths_ = std::move(paths);
    untrackedCount_ = untrackedCount;
    UpdateActiveSceneRel();

    // 段階の**予測** (spec §4.1「実行前に予測して確認ダイアログに出す」)。
    // 未追跡は消える = Deleted、それ以外は書き換わる = Modified と見なす。
    // ★予測は楽観側に倒れることがある (`.meta` の guid が変わるかは実行しないと
    //   分からない)。実行後は必ず本物の変更集合で分類し直す
    StageInputs guess;
    guess.activeScene = activeSceneRel_;
    for (const std::string& p : paths_) {
        StageChange c;
        c.path = p;
        c.kind = BatchChange::Kind::Modified;
        guess.changes.push_back(std::move(c));
    }
    predicted_ = Classify(guess);
    phase_ = Phase::Confirm;
}

void GitTransaction::RequestCheckout(std::string target)
{
    if (target.empty() || phase_ != Phase::Idle) {
        return;
    }
    op_ = OpKind::Checkout;
    target_ = std::move(target);
    paths_.clear();
    checkoutChanges_.clear();
    reportPaths_.clear();
    untrackedCount_ = 0;
    predicted_ = ApplyStage::A;
    predictSent_ = false;
    // 実行は確認の後。まず「何が降ってくるか」を聞く (SendPredict)
    phase_ = Phase::Predict;
}

void GitTransaction::SendPredict(SourceControlSession& scm)
{
    predictSent_ = true;
    UpdateActiveSceneRel();
    // ★向きは HEAD -> target。「共通祖先から」ではない — 知りたいのは
    //   **working tree に実際に降ってくるファイル**で、履歴の枝分かれではない
    scm.RequestDiffNames("HEAD", target_,
                         [this](bool ok, const std::vector<StageChange>& changes,
                                const std::string& code, const std::string& detail) {
                             if (!ok) {
                                 responseOk_ = false;
                                 errorCode_ = code;
                                 errorDetail_ = detail;
                                 reportPaths_.clear();
                                 phase_ = Phase::Report;
                                 return;
                             }
                             paths_.clear();
                             paths_.reserve(changes.size());
                             for (const StageChange& c : changes) {
                                 paths_.push_back(c.path);
                             }
                             StageInputs in;
                             in.changes = changes;
                             in.activeScene = activeSceneRel_;
                             // ★予測は楽観側 (`.meta` の guid が変わるかは実行しないと
                             //   分からない)。実行後に必ず分類し直す
                             predicted_ = Classify(in);
                             phase_ = Phase::Confirm;
                         });
}

std::wstring GitTransaction::AbsolutePathOf(EngineContext& ctx, const std::string& rel) const
{
    fs::path p(ctx.projectRoot);
    p /= fs::path(Utf8ToWide(rel));
    return p.lexically_normal().wstring();
}

void GitTransaction::BeginOp(EngineContext& ctx, SourceControlSession& scm)
{
    // ---- 実行前処理 (spec §4.1) ----
    // 1) 音楽を止める。WAV の MusicStream は**ファイルを開いたまま**なので、
    //    git がそのファイルを書き換えられない (共有違反で checkout ごと失敗する)
    if (ctx.audio != nullptr) {
        ctx.audio->StopMusic(0.0f);
    }
    // 2) 進行中の非同期テクスチャデコードを回収する。走ったままだと、
    //    git が書き換えた**後**の中身を古い AssetID で公開してしまう
    if (ctx.resources != nullptr) {
        ctx.resources->textures.WaitForAsyncLoads();
    }
    // 3) ホットリロードを一括モードへ。ここから EndBatch まで watcher は捨てられる
    if (ctx.reloadHub != nullptr) {
        ctx.reloadHub->BeginBatch();
    }

    // 実行前のディスクの様子を控える (kind と guid 変化の判定に使う)
    UpdateActiveSceneRel();
    existedBefore_.clear();
    metaGuidBefore_.clear();
    existedBefore_.reserve(paths_.size());
    metaGuidBefore_.reserve(paths_.size());
    for (const std::string& rel : paths_) {
        const std::wstring abs = AbsolutePathOf(ctx, rel);
        std::error_code ec;
        existedBefore_.push_back(fs::exists(abs, ec));
        uint64_t guid = 0;
        if (EndsWithCase(rel, ".meta")) {
            AssetMeta m;
            if (AssetDatabase::ReadMeta(abs, m)) {
                guid = m.guid;
            }
        }
        metaGuidBefore_.push_back(guid);
    }

    responseOk_ = false;
    errorCode_.clear();
    errorDetail_.clear();
    reportPaths_.clear();
    phase_ = Phase::Running;
    if (op_ == OpKind::Checkout) {
        scm.Checkout(target_, [this](const SourceControlSession::CheckoutResult& r) {
            responseOk_ = r.ok;
            errorCode_ = r.errorCode;
            errorDetail_ = r.errorDetail;
            reportPaths_ = r.errorPaths;
            // ★変更集合は git が返したものをそのまま使う (BuildChangeSet の代わり)。
            //   checkout は「選んだパス」ではなく「2 つのコミットの差」で動くので、
            //   こちらでディスクを舐めて推測すると必ずずれる
            checkoutChanges_ = r.changes;
            phase_ = Phase::Applying;
        });
        return;
    }
    scm.Revert(paths_, [this](bool ok, const std::string& code, const std::string& detail) {
        responseOk_ = ok;
        errorCode_ = code;
        errorDetail_ = detail;
        phase_ = Phase::Applying; // 後処理は次の OnImGui (Poll の中でシーンを読み直さない)
    });
}

std::vector<StageChange> GitTransaction::BuildChangeSet(EngineContext& ctx) const
{
    std::vector<StageChange> out;
    out.reserve(paths_.size());
    for (size_t i = 0; i < paths_.size(); ++i) {
        const std::string& rel = paths_[i];
        const std::wstring abs = AbsolutePathOf(ctx, rel);
        std::error_code ec;
        const bool existsNow = fs::exists(abs, ec);
        const bool existedBefore = i < existedBefore_.size() && existedBefore_[i];
        StageChange c;
        c.path = rel;
        // ★kind は「実行の前後でファイルが在るか」から決める。status の状態文字から
        //   推測すると、サイドカーのように status の行を持たないパスで必ず外れる
        if (!existsNow) {
            c.kind = BatchChange::Kind::Deleted;
        } else if (!existedBefore) {
            c.kind = BatchChange::Kind::Added;
        } else {
            c.kind = BatchChange::Kind::Modified;
        }
        if (existsNow && EndsWithCase(rel, ".meta")) {
            AssetMeta m;
            const uint64_t before = i < metaGuidBefore_.size() ? metaGuidBefore_[i] : 0;
            if (AssetDatabase::ReadMeta(abs, m) && before != 0 && m.guid != before) {
                c.metaGuidChanged = true;
            }
        }
        out.push_back(std::move(c));
    }
    return out;
}

void GitTransaction::ResolveMetaGuidChanges(EngineContext& ctx,
                                            std::vector<StageChange>& changes) const
{
    for (StageChange& c : changes) {
        if (!EndsWithCase(c.path, ".meta") || c.kind == BatchChange::Kind::Deleted) {
            continue;
        }
        // 実行**前**に控えた guid を探す (paths_ = 予測された集合)。
        // ★予測と実際は同じ 2 コミットの差なので普通は必ず見つかる。見つからない =
        //   予測の後に外から HEAD が動いた等の想定外 → **重い側 (C) に倒す**。
        //   guid が変わったのに A で流すと、シーンの参照が全部別物を指したまま
        //   保存できてしまう (相手のデータが消える形)
        bool found = false;
        uint64_t before = 0;
        for (size_t i = 0; i < paths_.size(); ++i) {
            if (paths_[i] == c.path) {
                found = true;
                before = i < metaGuidBefore_.size() ? metaGuidBefore_[i] : 0;
                break;
            }
        }
        if (!found) {
            MYE_LOG_WARN("[collab] %s was not in the predicted set - assuming its guid moved",
                         c.path.c_str());
            c.metaGuidChanged = true;
            continue;
        }
        AssetMeta m;
        const std::wstring abs = AbsolutePathOf(ctx, c.path);
        if (AssetDatabase::ReadMeta(abs, m) && before != 0 && m.guid != before) {
            c.metaGuidChanged = true;
        }
    }
}

void GitTransaction::RegisterAdded(EngineContext& ctx,
                                   const std::vector<StageChange>& changes) const
{
    // S5: 他人が足した新規アセットは、登録しないと**再起動するまで見えない**
    // (HandleChange は登録済みアセット以外を no-op にする)。
    // ★方式は「増分登録」= AssetDatabase::GuidForPath(path, createIfMissing=true)。
    //   ScanAndSync の再実行は 3 表を clear してから走査し直すので、
    //   assetkey/assetguid の解決先が一瞬でも空になる窓が開く (シーンの読み直しと
    //   同じフレームで走ると参照が解けない)。増分なら既存の登録に触らない
    if (ctx.assetDb == nullptr || ctx.projectRoot.empty()) {
        return;
    }
    int registered = 0;
    for (const StageChange& c : changes) {
        if (c.kind != BatchChange::Kind::Added || !IsInsideAssets(c.path)
            || IsMetaSidecar(c.path)) {
            continue;
        }
        const std::wstring abs = AbsolutePathOf(ctx, c.path);
        if (AssetDatabase::ClassifyPath(abs) == AssetType::Unknown) {
            continue;
        }
        // .meta が一緒に降ってきていればその guid を採る (GuidForPath はディスクの
        // .meta を先に見る)。無ければここで作られる = チームで guid が揃う
        ctx.assetDb->GuidForPath(abs, true);
        ++registered;
    }
    if (registered > 0) {
        MYE_LOG_INFO("[collab] registered %d new asset(s) in the asset database", registered);
    }
}

void GitTransaction::ApplyStageB(EngineContext& ctx, const std::vector<StageChange>& changes)
{
    bool controllerTouched = false;
    bool terrainTouched = false;
    bool inputTouched = false;
    bool projectSettingsTouched = false;
    bool csTouched = false;
    bool cppTouched = false;
    for (const StageChange& c : changes) {
        if (EndsWithCase(c.path, ".controller.json")) {
            controllerTouched = true;
        }
        if (EndsWithCase(c.path, ".terrain.json") || EndsWithCase(c.path, ".terrain.edit")) {
            terrainTouched = true;
        }
        if (c.path == "assets/input/actions.json") {
            inputTouched = true;
        }
        if (c.path == "assets/project_settings.json") {
            projectSettingsTouched = true;
        }
        if (IsCsScript(c.path)) {
            csTouched = true;
        }
        if (IsCppScript(c.path)) {
            cppTouched = true;
        }
    }

    // ---- ライブラリのキャッシュ無効化 (spec §4.1「開き直し前に無効化」) ----
    // ★どちらも既存の API で足りることを確認済み (未決事項の回答):
    //   ControllerLibrary::LoadFromFile は同じ hash で登録し直す = キャッシュの差し替え、
    //   RenderSystem::InvalidateTerrain は TerrainSystem::Clear の公開口。
    //   よってこの 2 種を C へ格上げする必要は無い
    if (controllerTouched && ctx.controllers != nullptr) {
        for (const StageChange& c : changes) {
            if (!EndsWithCase(c.path, ".controller.json")
                || c.kind == BatchChange::Kind::Deleted) {
                continue;
            }
            const std::wstring abs = AbsolutePathOf(ctx, c.path);
            if (ctx.controllers->Contains(ControllerLibrary::HashForPath(abs))) {
                ctx.controllers->LoadFromFile(abs);
            }
        }
    }
    if (terrainTouched && ctx.renderSystem != nullptr) {
        ctx.renderSystem->InvalidateTerrain();
    }
    if (inputTouched && ctx.inputActions != nullptr) {
        ctx.inputActions->Load(ctx.assetsRoot, true);
    }
    if (projectSettingsTouched) {
        PhysicsLayerNames::Get().Load(ctx.assetsRoot, true);
        PartTagNames::Get().Load(ctx.assetsRoot, true);
    }

    // ---- 開いている文書 ----
    const std::wstring activeAbs = hooks_.activeScenePath ? hooks_.activeScenePath() : std::wstring();
    if (!activeAbs.empty()) {
        std::error_code ec;
        if (!fs::exists(activeAbs, ec)) {
            // アクティブシーンがブランチ側で消えた (spec §4.1)
            if (hooks_.newScene) {
                hooks_.newScene();
            }
            if (hooks_.toast) {
                hooks_.toast(LogLevel::Warn, Tr(StrId::Scm_SceneGone));
            }
        } else if (hooks_.loadScene) {
            // ★段階 B は「開き直す」段階そのもの (spec §4.1)。何が引き金でも必ず
            //   開き直す — 「controller だけ変わったから今回は開き直さない」のような
            //   例外を作ると、部分適用の抜けが**画面上は自然に見える**形で残る。
            //   ゲートが「全文書が保存済み」を保証しているので失うものは無い
            hooks_.loadScene(activeAbs);
        }
    }
    if (csTouched && hooks_.compileCs) {
        hooks_.compileCs();
    }
    if (cppTouched && hooks_.toast) {
        // C++ は DLL を作り直さないと反映されない。エディタは自動でビルドを始めない
        // (数十秒かかるうえ、ユーザーが今それを望んでいるとは限らない)
        hooks_.toast(LogLevel::Warn, Tr(StrId::Scm_RebuildScriptsHint));
    }
}

void GitTransaction::ApplyResult(EngineContext& ctx, SourceControlSession& scm)
{
    (void)scm;
    if (!responseOk_) {
        // 失敗。**必ず EndBatch を通す** — 通さないとホットリロードが止まったまま残る
        if (ctx.reloadHub != nullptr) {
            ctx.reloadHub->EndBatch({});
        }
        reportText_.clear();
        if (reportPaths_.empty()) {
            reportPaths_ = paths_; // 対象一覧が来なかったときは要求した集合を出す
        }
        phase_ = Phase::Report;
        return;
    }

    // ★op ごとに違うのは**ここだけ** (「変更集合をどう決めるか")。
    //   revert = 実行前後のディスク、checkout = git が返した before..after の差分
    std::vector<StageChange> changes;
    if (op_ == OpKind::Checkout) {
        changes = std::move(checkoutChanges_);
        checkoutChanges_.clear();
        ResolveMetaGuidChanges(ctx, changes);
    } else {
        changes = BuildChangeSet(ctx);
    }
    StageInputs in;
    in.changes = changes;
    in.activeScene = activeSceneRel_;
    applied_ = Classify(in);

    if (applied_ == ApplyStage::C) {
        // 差し替えでは辻褄が合わない。ReloadHub は空で閉じて再起動を確認する
        if (ctx.reloadHub != nullptr) {
            ctx.reloadHub->EndBatch({});
        }
        restartFailed_ = false;
        phase_ = Phase::Restart;
        return;
    }

    // A / B 共通: 新規資産の登録 → 一括適用
    RegisterAdded(ctx, changes);
    // ★ReloadHub へ渡すのは **A 段階の変更だけ** (spec §4.1 / sub-04: 「B: EndBatch(A集合)」)。
    //   B の引き金 (アクティブシーンなど) まで渡すと、この直後に丸ごと開き直す文書へ
    //   先に ApplyDiff が走る = 捨てられる差分を 1 回適用するだけ無駄で、
    //   「差分適用の結果」と「読み直しの結果」が食い違ったときに原因が読めなくなる
    std::vector<BatchChange> batch;
    batch.reserve(changes.size());
    for (const StageChange& c : changes) {
        if (ClassifyChange(c, activeSceneRel_) != ApplyStage::A) {
            continue;
        }
        BatchChange b;
        b.path = AbsolutePathOf(ctx, c.path);
        b.kind = c.kind;
        batch.push_back(std::move(b));
    }
    if (ctx.reloadHub != nullptr) {
        ctx.reloadHub->EndBatch(batch);
    }
    if (applied_ == ApplyStage::B) {
        ApplyStageB(ctx, changes);
    }
    if (hooks_.toast) {
        char buf[192];
        if (op_ == OpKind::Checkout) {
            std::snprintf(buf, sizeof(buf), Tr(StrId::Scm_CheckoutDone), target_.c_str(),
                          static_cast<int>(changes.size()));
        } else {
            std::snprintf(buf, sizeof(buf), Tr(StrId::Scm_DiscardDone),
                          static_cast<int>(paths_.size()));
        }
        hooks_.toast(LogLevel::Info, buf);
    }
    // ★ログにも残す (トーストは実時間 4 秒で消えるので、後から
    //   「本当に段階 A で済んだのか」を確かめる手段がここしか無い)
    MYE_LOG_INFO("[collab] %s applied: %zu path(s), stage %c",
                 op_ == OpKind::Checkout ? "checkout" : "revert", changes.size(),
                 applied_ == ApplyStage::A ? 'A' : 'B');
    paths_.clear();
    phase_ = Phase::Idle;
}

void GitTransaction::OnImGui(EngineContext& ctx, SourceControlSession& scm)
{
    // ★毎フレーム控える。予測 (diff_names の応答) は ctx を持たないので、
    //   パスの相対化に使えるルートはここで拾った値だけになる
    projectRoot_ = ctx.projectRoot;
    if (phase_ == Phase::Applying) {
        ApplyResult(ctx, scm);
    }
    if (phase_ == Phase::Idle) {
        return;
    }
    if (phase_ == Phase::Predict && !predictSent_) {
        SendPredict(scm);
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    // ★ImGuiCond_Appearing ではなく **Always**。AlwaysAutoResize の窓は
    //   「出た最初のフレームには自分の大きさを知らない」ので、Appearing だと
    //   ずれた位置 (M66d では中央より上) で確定して二度と直らない。
    //   modal は掴んで動かす対象ではないので、毎フレーム中央へ置き直してよい
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                   vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    if (phase_ == Phase::Predict) {
        // 予測の待ち。**モーダルで入力を止める** — ここで別のボタンを押せると、
        // 「切替を頼んだ覚えがあるのに再生が始まる」ような取り違えが起きる
        ImGui::OpenPopup(Tr(StrId::Scm_SwitchTitle));
        if (ImGui::BeginPopupModal(Tr(StrId::Scm_SwitchTitle), nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text(Tr(StrId::Scm_SwitchTo), target_.c_str());
            ImGui::TextDisabled("%s", Tr(StrId::Scm_SwitchChecking));
            ImGui::EndPopup();
        }
        return;
    }

    if (phase_ == Phase::Confirm || phase_ == Phase::Running) {
        // ★確認と実行中で**同じモーダル**を使う。閉じて開き直すと、その 1 フレームだけ
        //   他の窓に入力が通る (押しっぱなしのクリックがそのまま吸われる)。
        //   checkout は Predict の待ちとも同じ題名 = 予測 → 確認 → 実行が 1 枚で流れる
        const char* title =
            Tr(op_ == OpKind::Checkout ? StrId::Scm_SwitchTitle : StrId::Scm_DiscardTitle);
        ImGui::OpenPopup(title);
        if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (op_ == OpKind::Checkout) {
                ImGui::Text(Tr(StrId::Scm_SwitchTo), target_.c_str());
                ImGui::Text(Tr(StrId::Scm_SwitchBody), static_cast<int>(paths_.size()));
            } else {
                ImGui::Text(Tr(StrId::Scm_DiscardBody), static_cast<int>(paths_.size()));
            }
            if (untrackedCount_ > 0) {
                ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Error);
                ImGui::Text(Tr(StrId::Scm_DiscardUntracked), untrackedCount_);
                ImGui::PopStyleColor();
            }
            ImGui::Spacing();
            if (ImGui::BeginChild("###ScmDiscardList", ImVec2(420.0f, 120.0f),
                                  ImGuiChildFlags_Borders)) {
                for (const std::string& p : paths_) {
                    ImGui::TextUnformatted(p.c_str());
                }
            }
            ImGui::EndChild();
            ImGui::TextDisabled("%s",
                                Tr(predicted_ == ApplyStage::C   ? StrId::Scm_StagePlanC
                                       : predicted_ == ApplyStage::B ? StrId::Scm_StagePlanB
                                                                     : StrId::Scm_StagePlanA));
            ImGui::Spacing();
            if (phase_ == Phase::Running) {
                ImGui::TextDisabled("%s", Tr(StrId::Scm_OpRunning));
            } else {
                if (ImGui::Button(Tr(op_ == OpKind::Checkout ? StrId::Scm_SwitchConfirm
                                                             : StrId::Scm_DiscardConfirm),
                                  ImVec2(140, 0))) {
                    BeginOp(ctx, scm);
                }
                ImGui::SameLine();
                if (ImGui::Button(Tr(StrId::Common_Cancel), ImVec2(110, 0))) {
                    paths_.clear();
                    phase_ = Phase::Idle;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
        return;
    }

    if (phase_ == Phase::Report) {
        ImGui::OpenPopup(Tr(StrId::Scm_ResultTitle));
        if (ImGui::BeginPopupModal(Tr(StrId::Scm_ResultTitle), nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Error);
            ImGui::TextWrapped("%s: %s", errorCode_.c_str(), errorDetail_.c_str());
            ImGui::PopStyleColor();
            // ★ローカル変更と重なった checkout の一覧はここが出口 (spec S7)。
            //   「何を破棄すれば進めるか」を出さないと、ユーザーは git のエラー文を
            //   読むしかなくなる (窓の中では読めない)
            if (errorCode_ == collaberr::kLocalChangesOverwritten) {
                ImGui::TextWrapped("%s", Tr(StrId::Scm_OverwriteHint));
            }
            if (!reportPaths_.empty()) {
                if (ImGui::BeginChild("###ScmErrPaths", ImVec2(420.0f, 100.0f),
                                      ImGuiChildFlags_Borders)) {
                    for (const std::string& p : reportPaths_) {
                        ImGui::TextUnformatted(p.c_str());
                    }
                }
                ImGui::EndChild();
            }
            if (ImGui::Button(Tr(StrId::Common_Close), ImVec2(110, 0))) {
                paths_.clear();
                reportPaths_.clear();
                phase_ = Phase::Idle;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        return;
    }

    // Phase::Restart (段階 C)
    //
    // ★ボタンは**『再起動』1 個だけ** (spec §4.1 C 行)。「あとで」も「キャンセル」も置かない。
    //   この時点で working tree は既に入れ替わっており、エディタが持っている
    //   スキーマ / guid はディスクと食い違っている。そのまま編集を続けて保存すると
    //   **未知コンポーネントを読み飛ばした結果が書き戻る** = 相手のデータが消える。
    //   段階 C を設けた理由そのものなので、逃げ道を作らない。
    //   ゲートが「全文書保存済み」を保証しているので、即時再起動で失うものは無い
    ImGui::OpenPopup(Tr(StrId::Scm_RestartTitle));
    if (ImGui::BeginPopupModal(Tr(StrId::Scm_RestartTitle), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", Tr(StrId::Scm_RestartBody));
        if (restartFailed_) {
            // ★失敗しても閉じない。閉じると「あとで」と同じ状態になる。
            //   自力で起動し直してもらうしかないので、そう書いて出す
            ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Error);
            ImGui::TextWrapped("%s", Tr(StrId::Scm_RestartFailed));
            ImGui::PopStyleColor();
        }
        ImGui::Spacing();
        if (ImGui::Button(Tr(StrId::Scm_RestartNow), ImVec2(150, 0))) {
            // 成功すれば ctx.requestExit が立ってこのプロセスは終わる。
            // モーダルを閉じるのは**成功したときだけ**
            const bool ok = hooks_.relaunch ? hooks_.relaunch() : false;
            restartFailed_ = !ok;
            if (ok) {
                phase_ = Phase::Idle;
                paths_.clear();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
}

} // namespace mye
