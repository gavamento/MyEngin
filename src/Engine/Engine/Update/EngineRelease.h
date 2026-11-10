//====================================================================================
//                          EngineRelease.h
//  MyEngine/ 秋田蓮音                                                      11/10/2026
//                                          配布版の版番号・GitHub Release の解析と取得
//====================================================================================
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Engine/Platform/HttpClient.h"

namespace mye {

// 配布元 (GitHub Releases、M98)。ADR-037
constexpr const char* kReleaseRepoOwner = "gavamento";
constexpr const char* kReleaseRepoName = "MyEngin";
// Release の asset 名の契約。release.yml / make_dist.ps1 と同じ名前にする
constexpr const char* kLauncherAssetName = "MyEngineLauncher.exe";
// 検証用に取得元をローカルフォルダへ向ける環境変数 (中身は GitHub と同じ形の release.json)
constexpr const wchar_t* kReleaseSourceEnv = L"MYE_RELEASE_SOURCE";

// "0.6.8.22" / "v0.6.8.23-rc1" の形。数字は最大 4 段 (足りない段は 0)
struct EngineVersion
{
    std::array<uint32_t, 4> parts = {};
    std::string suffix; // "-" の後ろ。空 = 正式版
    std::string text;   // 先頭の 'v' を外した元の表記 (asset 名に使う)
    bool valid = false;
};

bool ParseEngineVersion(std::string_view text, EngineVersion& out);
// a < b で負、a == b で 0、a > b で正。同じ数字なら正式版がプレリリースより新しい
int CompareEngineVersion(const EngineVersion& a, const EngineVersion& b);
// kEngineVersion
EngineVersion CurrentEngineVersion();

// "MyEngine-<ver>-win64.zip" と、その SHA-256 を書いた "<zip>.sha256"
std::string EnginePackageAssetName(const EngineVersion& version);
std::string EnginePackageHashAssetName(const EngineVersion& version);

struct ReleaseAsset
{
    std::string name;
    std::string url; // https:// か、ローカル取得元ではそのフォルダからの相対パス
    uint64_t size = 0;
};

struct ReleaseInfo
{
    std::string tag;
    EngineVersion version;
    std::string title;
    std::string notes; // リリースノート (Markdown のまま)
    std::string publishedAt;
    bool prerelease = false;
    bool draft = false;
    std::vector<ReleaseAsset> assets;
};

// GitHub REST API の release 1 件 (object) か一覧 (array) を読む。版として読めないタグの release は捨てる
bool ParseGitHubReleases(std::string_view json, std::vector<ReleaseInfo>& out, std::string& error);
// 下書きを除き、includePrerelease でなければプレリリースも除いて、最も新しい版を返す (無ければ nullptr)
const ReleaseInfo* PickLatestRelease(const std::vector<ReleaseInfo>& releases, bool includePrerelease);
const ReleaseAsset* FindReleaseAsset(const ReleaseInfo& release, std::string_view name);
// sha256sum 形式 ("<64 桁の 16 進>  <名前>") の先頭から 16 進を取り出し、小文字にする
bool ParseSha256Text(std::string_view text, std::string& hexOut);

// 取得元。既定は GitHub、--release-source <dir> か MYE_RELEASE_SOURCE でローカルフォルダ
struct ReleaseSource
{
    std::wstring localDir; // 空 = GitHub
};
ReleaseSource ResolveReleaseSource(const std::wstring& cliOverride);

struct FetchReleaseResult
{
    bool ok = false;
    bool found = false; // 取得できたが対象の release が無い
    std::string error;
    ReleaseInfo release;
};
FetchReleaseResult FetchLatestRelease(const ReleaseSource& source, bool includePrerelease,
                                      const std::atomic<bool>* cancel);
// asset を destPath へ落とす (ローカル取得元ではコピー)
HttpResult FetchReleaseAsset(const ReleaseSource& source, const ReleaseAsset& asset, const std::wstring& destPath,
                             const HttpProgressFn& progress, const std::atomic<bool>* cancel);
// 小さな asset (.sha256) を文字列で受け取る
HttpResult FetchReleaseAssetText(const ReleaseSource& source, const ReleaseAsset& asset, std::string& text,
                                 const std::atomic<bool>* cancel);

} // namespace mye
