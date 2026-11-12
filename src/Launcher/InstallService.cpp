//====================================================================================
//                          InstallService.cpp
//  MyEngine/ 秋田蓮音                                                      11/10/2026
//                                          配布版のインストール・削除とワーカーの実装
//====================================================================================
#include "Launcher/InstallService.h"

#include <cstdio>
#include <filesystem>

#include <Windows.h>
#include <shellapi.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Platform/DownloadTrust.h"
#include "Engine/Platform/Sha256.h"
#include "Engine/Platform/ZipExtract.h"
#include "Launcher/LauncherSelf.h"

namespace mye {

namespace {

namespace fs = std::filesystem;

std::string NowUtcIso8601()
{
    SYSTEMTIME st = {};
    GetSystemTime(&st);
    char buf[32] = {};
    snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond);
    return buf;
}

std::wstring PidSuffix()
{
    return std::to_wstring(GetCurrentProcessId());
}

void Report(const InstallProgressFn& progress, InstallStage stage, uint64_t done, uint64_t total)
{
    if (progress) {
        progress(stage, done, total);
    }
}

bool IsCancelled(const std::atomic<bool>* cancel)
{
    return cancel && cancel->load(std::memory_order_relaxed);
}

// ローカル取得元 (検証用) の asset は印を付けない
bool IsInternetAsset(const ReleaseAsset& asset)
{
    return asset.url.rfind("https://", 0) == 0;
}

} // namespace

InstallOutcome InstallRelease(const ReleaseSource& source, const ReleaseInfo& release, const std::wstring& root,
                              const std::wstring& enginesDir, const InstallProgressFn& progress,
                              const std::atomic<bool>* cancel)
{
    InstallOutcome out;
    out.version = release.version.text;
    if (!release.version.valid) {
        out.error = "the release has no valid version tag";
        return out;
    }
    const fs::path engines(enginesDir);
    const fs::path target = engines / Utf8ToWide(release.version.text);
    out.installDir = target.wstring();
    std::error_code ec;
    InstallMarker existing;
    if (ReadInstallMarker(target.wstring(), existing)) {
        out.ok = true;
        out.alreadyInstalled = true;
        return out;
    }
    if (fs::exists(target, ec)) {
        // 印の無い同名フォルダは手で置かれた物かもしれないので消さない
        out.error = "the folder already exists but was not installed by the Launcher: " + WideToUtf8(target.wstring());
        return out;
    }

    const ReleaseAsset* zipAsset = FindReleaseAsset(release, EnginePackageAssetName(release.version));
    const ReleaseAsset* hashAsset = FindReleaseAsset(release, EnginePackageHashAssetName(release.version));
    if (!zipAsset || !hashAsset) {
        out.error = "the release has no " + EnginePackageAssetName(release.version) + " (or its .sha256)";
        return out;
    }

    // 期待する SHA-256 を先に取る (取れないなら大きな zip を落とす意味がない)
    Report(progress, InstallStage::Checking, 0, 0);
    std::string hashText;
    std::string expectedHex;
    const HttpResult hashResult = FetchReleaseAssetText(source, *hashAsset, hashText, cancel);
    if (!hashResult.ok || !ParseSha256Text(hashText, expectedHex)) {
        out.cancelled = hashResult.cancelled;
        out.error = hashResult.ok ? "the .sha256 asset is malformed" : hashResult.error;
        return out;
    }

    fs::create_directories(DownloadsDir(root), ec);
    fs::create_directories(engines, ec);
    if (!fs::is_directory(engines, ec)) {
        // 書き込めない場所 (Program Files など) を選んだときは、大きな zip を落とす前に止める
        out.error = "cannot create the folder: " + WideToUtf8(engines.wstring());
        return out;
    }
    const fs::path part = fs::path(DownloadsDir(root)) / (Utf8ToWide(zipAsset->name) + L"." + PidSuffix() + L".part");
    const fs::path staging = engines / (Utf8ToWide(release.version.text) + kStagingInfix + PidSuffix());
    // ここから先はどこで抜けても書きかけを消す
    auto fail = [&](std::string error, bool cancelled) {
        std::error_code cleanupEc;
        fs::remove(part, cleanupEc);
        fs::remove_all(staging, cleanupEc);
        out.ok = false;
        out.cancelled = cancelled;
        out.error = std::move(error);
        return out;
    };

    const HttpResult download = FetchReleaseAsset(
        source, *zipAsset, part.wstring(),
        [&](uint64_t done, uint64_t total) {
            Report(progress, InstallStage::Downloading, done, total != 0 ? total : zipAsset->size);
        },
        cancel);
    if (!download.ok) {
        return fail(download.error, download.cancelled);
    }

    Report(progress, InstallStage::Verifying, 0, 0);
    // ネットワークから落とした物は、ブラウザと同じく「インターネット由来」の印を付けてスキャンを通す (ADR-037 D9)
    std::string zoneId;
    if (IsInternetAsset(*zipAsset)) {
        const DownloadTrustResult trust = MarkAndScanDownload(part.wstring(), zipAsset->url);
        if (!trust.ok) {
            return fail(trust.error, false);
        }
        ReadZoneIdentifier(part.wstring(), zoneId);
    }
    std::string actualHex;
    if (!Sha256FileHex(part.wstring(), actualHex, cancel)) {
        return fail(IsCancelled(cancel) ? "cancelled" : "cannot read the download", IsCancelled(cancel));
    }
    if (actualHex != expectedHex) {
        return fail("SHA-256 mismatch (expected " + expectedHex + ", got " + actualHex + ")", false);
    }

    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    const ZipExtractResult extract = ExtractZip(
        part.wstring(), staging.wstring(), true,
        [&](uint64_t done, uint64_t total) { Report(progress, InstallStage::Extracting, done, total); }, cancel);
    if (!extract.ok) {
        return fail(extract.error, extract.cancelled);
    }

    Report(progress, InstallStage::Finishing, 0, 0);
    // zip の印を展開したファイルへ引き継ぐ (エクスプローラーで展開したときと同じ扱い)。
    // 印の無い exe を書いて起動すると振る舞い検知に「防御の回避」と見なされる
    if (!zoneId.empty()) {
        for (fs::recursive_directory_iterator it(staging, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) && !WriteZoneIdentifier(it->path().wstring(), zoneId)) {
                return fail("cannot mark " + WideToUtf8(it->path().wstring()), false);
            }
        }
    }
    if (!fs::exists(InstalledEditorPath(staging.wstring()), ec)) {
        return fail("the package does not contain bin\\x64\\Release\\Editor.exe", false);
    }
    InstallMarker marker;
    marker.version = release.version.text;
    marker.tag = release.tag;
    marker.sha256 = expectedHex;
    marker.installedAt = NowUtcIso8601();
    if (!WriteInstallMarker(staging.wstring(), marker)) {
        return fail("cannot write install.json", false);
    }
    fs::rename(staging, target, ec);
    if (ec) {
        // 同じ版を別の Launcher が先に入れ終えた場合もここに来る
        InstallMarker raced;
        if (ReadInstallMarker(target.wstring(), raced)) {
            fail(std::string(), false);
            out.ok = true;
            out.alreadyInstalled = true;
            out.error.clear();
            return out;
        }
        return fail("cannot move the installed files into place: " + ec.message(), false);
    }
    fs::remove(part, ec);
    out.ok = true;
    MYE_LOG_INFO("launcher: installed %s to %s", release.version.text.c_str(), WideToUtf8(target.wstring()).c_str());
    return out;
}

InstallOutcome UninstallEngine(const std::wstring& installDir, const InstallProgressFn& progress)
{
    InstallOutcome out;
    out.installDir = installDir;
    InstallMarker marker;
    if (!ReadInstallMarker(installDir, marker)) {
        out.error = "not a Launcher install: " + WideToUtf8(installDir);
        return out;
    }
    out.version = marker.version;
    Report(progress, InstallStage::Deleting, 0, 0);
    // 先に名前を変える: 動いている exe があれば rename が失敗し、1 ファイルも消さずに止まれる
    const fs::path from(installDir);
    const fs::path doomed = from.parent_path() / (from.filename().wstring() + kDeletingInfix + PidSuffix());
    std::error_code ec;
    fs::rename(from, doomed, ec);
    if (ec) {
        out.error = "the version is in use (close its editor first): " + ec.message();
        return out;
    }
    fs::remove_all(doomed, ec);
    if (ec) {
        // 名前を変えた時点で一覧からは消えている。残りは次回の起動で片付ける
        MYE_LOG_WARN("launcher: leftover files in %s (%s)", WideToUtf8(doomed.wstring()).c_str(), ec.message().c_str());
    }
    out.ok = true;
    return out;
}

void CleanupStaleInstallWork(const std::wstring& root)
{
    std::error_code ec;
    for (const std::wstring& enginesDir : KnownEnginesDirs(root)) {
        for (fs::directory_iterator it(enginesDir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::wstring name = it->path().filename().wstring();
            if (it->is_directory(ec) && IsInstallWorkFolderName(name)) {
                std::error_code removeEc;
                fs::remove_all(it->path(), removeEc);
                MYE_LOG_INFO("launcher: removed leftover %s", WideToUtf8(it->path().wstring()).c_str());
            }
        }
        ec.clear();
    }
    for (fs::directory_iterator it(DownloadsDir(root), ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == L".part") {
            std::error_code removeEc;
            fs::remove(it->path(), removeEc);
        }
    }
}

bool LaunchInstalledEditor(const std::wstring& installDir, const std::wstring& projectDir, std::string& error)
{
    const std::wstring exe = InstalledEditorPath(installDir);
    std::error_code ec;
    if (!fs::exists(exe, ec)) {
        error = "missing " + WideToUtf8(exe);
        return false;
    }
    std::wstring args;
    std::wstring workDir = InstalledBinDir(installDir);
    if (!projectDir.empty()) {
        args = L"--project \"" + projectDir + L"\"";
        workDir = projectDir; // RelaunchSelfWithProject と同じく作業フォルダ = プロジェクト
    }
    const HINSTANCE r = ShellExecuteW(nullptr, L"open", exe.c_str(), args.empty() ? nullptr : args.c_str(),
                                      workDir.c_str(), SW_SHOWNORMAL);
    if (reinterpret_cast<intptr_t>(r) <= 32) {
        error = "ShellExecute failed (" + std::to_string(reinterpret_cast<intptr_t>(r)) + ")";
        return false;
    }
    return true;
}

// ---- LauncherJob ----

LauncherJob::~LauncherJob()
{
    Cancel();
    Join();
}

LauncherJob::Kind LauncherJob::CurrentKind() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return kind_;
}

void LauncherJob::Join()
{
    if (thread_.joinable()) {
        thread_.join();
    }
}

void LauncherJob::SetProgress(InstallStage stage, uint64_t done, uint64_t total)
{
    std::lock_guard<std::mutex> lock(mutex_);
    stage_ = stage;
    done_ = done;
    total_ = total;
}

void LauncherJob::Progress(InstallStage& stage, uint64_t& done, uint64_t& total) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    stage = stage_;
    done = done_;
    total = total_;
}

void LauncherJob::Launch(Kind kind, std::function<void()> body)
{
    if (running_.load()) {
        return; // 画面側がボタンを無効にしている。二重起動は無視する
    }
    Join();
    cancel_.store(false);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        kind_ = kind;
        stage_ = InstallStage::Idle;
        done_ = 0;
        total_ = 0;
    }
    running_.store(true);
    thread_ = std::thread([this, body = std::move(body)] {
        body();
        running_.store(false);
    });
}

void LauncherJob::StartCheck(const ReleaseSource& source, bool includePrerelease)
{
    Launch(Kind::Check, [this, source, includePrerelease] {
        SetProgress(InstallStage::Checking, 0, 0);
        FetchReleaseResult r = FetchLatestRelease(source, includePrerelease, &cancel_);
        std::lock_guard<std::mutex> lock(mutex_);
        check_ = std::move(r);
        hasCheck_ = true;
        stage_ = InstallStage::Idle;
    });
}

void LauncherJob::StartInstall(const ReleaseSource& source, const ReleaseInfo& release, const std::wstring& root,
                               const std::wstring& enginesDir)
{
    Launch(Kind::Install, [this, source, release, root, enginesDir] {
        InstallOutcome r = InstallRelease(
            source, release, root, enginesDir,
            [this](InstallStage stage, uint64_t done, uint64_t total) { SetProgress(stage, done, total); }, &cancel_);
        std::lock_guard<std::mutex> lock(mutex_);
        install_ = std::move(r);
        hasInstall_ = true;
        stage_ = InstallStage::Idle;
    });
}

void LauncherJob::StartUninstall(const std::wstring& installDir)
{
    Launch(Kind::Uninstall, [this, installDir] {
        InstallOutcome r = UninstallEngine(
            installDir, [this](InstallStage stage, uint64_t done, uint64_t total) { SetProgress(stage, done, total); });
        std::lock_guard<std::mutex> lock(mutex_);
        install_ = std::move(r);
        hasInstall_ = true;
        stage_ = InstallStage::Idle;
    });
}

void LauncherJob::StartSelfUpdate(const ReleaseSource& source, const ReleaseInfo& release,
                                  const std::wstring& targetExe)
{
    Launch(Kind::SelfUpdate, [this, source, release, targetExe] {
        InstallOutcome r = DownloadLauncherUpdate(
            source, release, targetExe,
            [this](InstallStage stage, uint64_t done, uint64_t total) { SetProgress(stage, done, total); }, &cancel_);
        std::lock_guard<std::mutex> lock(mutex_);
        install_ = std::move(r);
        hasInstall_ = true;
        stage_ = InstallStage::Idle;
    });
}

bool LauncherJob::TakeCheckResult(FetchReleaseResult& out)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!hasCheck_) {
        return false;
    }
    out = std::move(check_);
    hasCheck_ = false;
    return true;
}

bool LauncherJob::TakeInstallResult(InstallOutcome& out)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!hasInstall_) {
        return false;
    }
    out = std::move(install_);
    hasInstall_ = false;
    return true;
}

} // namespace mye
