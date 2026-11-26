//====================================================================================
//                          ServerPackage.h
//  MyEngin/ 秋田蓮音                                                       11/27/2026
//                                          GameLift 用サーバ版の組み立てと局所起動確認
//====================================================================================
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mye {

// ---- サーバ版の組み立て ----
// 書き出し済みフォルダ (BuildSettings の出力) から、専用サーバの実行に要る分だけを 1 つのフォルダへ組む。
// 呼び出しは同期 (assets のコピー時間だけ戻らない)。UI から使うときは呼び出し側が作業スレッドに載せる。
// グローバル状態を持たないので別スレッドから呼んでよい。

inline constexpr const wchar_t* kServerBuildInfoName = L"mye_server_build.json";
inline constexpr const wchar_t* kServerBuildInstallBatName = L"install.bat";
inline constexpr const wchar_t* kVcRedistFileName = L"vc_redist.x64.exe";

struct ServerPackageInput {
    std::filesystem::path distDir;   // 書き出し済みフォルダ (Runtime.exe と assets\content_manifest.json が要る)
    std::filesystem::path exeDir;    // Editor.exe のフォルダ (Server.exe と OpenSSL の DLL が要る)
    std::filesystem::path outputDir; // 組み立て先。既存なら中身を入れ替える (前回の組み立て結果だけ)
    std::filesystem::path vsInstallDirOverride; // 空 = vswhere で探す。テストが偽の VS を指すために使う
};

struct ServerPackageResult {
    bool ok = false;
    std::vector<std::string> problems; // 失敗の理由 (UTF-8)。足りない入力を全部挙げる
    std::vector<std::string> warnings; // 続行できる注意 (UTF-8)
    std::filesystem::path outputDir;
    uint64_t contentHash = 0;   // manifest の値。書き出しと同じになる
    uint64_t gameLogicHash = 0; // GameLogic.dll のバイト列のハッシュ (出自の gameVersion)
    bool serverIsDebug = false;
    uint32_t fileCount = 0;
    uint64_t totalBytes = 0;
};

// 4.1.2 の組み立て。失敗しても outputDir は変わらない (一時フォルダで組んでから入れ替える)
ServerPackageResult BuildServerPackage(const ServerPackageInput& input);

// install.bat の中身 (CRLF)。冪等で、vc_redist の戻り値は記録だけして成功で抜ける
std::string MakeServerInstallBat();

// VS のインストール先から VC\Redist\MSVC\<版>\vc_redist.x64.exe の最も新しい版を探す。
// 数値の版 (14.51.36231) を優先し、無ければ v145 のような名前のものを使う。見つからなければ空
std::filesystem::path FindVcRedistIn(const std::filesystem::path& vsInstallDir);

// vswhere -latest -property installationPath の結果。見つからなければ空 (待ちは最大 20 秒)
std::filesystem::path FindVsInstallDirViaVswhere();

// exe のフォルダ名が Debug か (bin\x64\Debug)。Server.exe を exeDir から取るので構成はフォルダ名で分かる
bool IsDebugBuildDir(const std::filesystem::path& exeDir);

// ---- 局所起動確認 ----
// 組んだフォルダの Server.exe を --hosting local --server-timeout 3 で起動し、exit 5 とログの
// "ready on UDP port" で「assets / GameLogic.dll / シーンの同梱漏れが無い」ことを確かめる。
// (VC++ ランタイムが入っていない環境で動くかは分からない。それは実機で確かめる)
// UI スレッドを止めない: Start → Poll (毎フレーム。待たない) → 結果。
enum class ServerLocalCheckState { Idle, Running, Passed, Failed };

class ServerLocalCheck {
public:
    ServerLocalCheck() = default;
    ~ServerLocalCheck();
    ServerLocalCheck(const ServerLocalCheck&) = delete;
    ServerLocalCheck& operator=(const ServerLocalCheck&) = delete;

    // buildDir: 組んだフォルダ (作業ディレクトリにもなる)。logPath: 子の stdout / stderr の置き場で、
    // buildDir の外にすること。起動できなければ false で state = Failed (summary に理由)
    bool Start(const std::filesystem::path& buildDir, const std::filesystem::path& logPath);
    // 状態を進めて返す。終わったら一時ファイル (cache\ など子が作ったもの) を buildDir から消す
    ServerLocalCheckState Poll();
    // 実行中なら子を終了して Idle へ戻す
    void Cancel();

    ServerLocalCheckState State() const { return state_; }
    const std::string& Summary() const { return summary_; }

private:
    void Finish(ServerLocalCheckState state, std::string summary);
    void RemoveCreatedFiles();

    ServerLocalCheckState state_ = ServerLocalCheckState::Idle;
    std::string summary_;
    void* process_ = nullptr;
    uint64_t startedMs_ = 0;
    std::filesystem::path buildDir_;
    std::filesystem::path logPath_;
    std::vector<std::wstring> existingPaths_; // 起動前に buildDir にあったもの (相対・小文字・整列済み)
};

// ---- CLI ----
// Editor.exe --project <proj> --gamelift-server-build <dist>: ウィンドウも D3D も作らず、
// <proj>\cache\gamelift\server-build\ へ組んで局所起動確認まで行う。結果を 1 行 stdout へ (詳細は stderr)。
// exit 0 = 組めて確認も通った / 1 = 失敗
int RunServerBuildCli(const std::wstring& projectDir, const std::wstring& distDir);

} // namespace mye
