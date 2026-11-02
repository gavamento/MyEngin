//====================================================================================
//                          VisualStudioLauncher.h
//  MyEngin/ 秋田蓮音                                                     11/02/2026
//                                          スクリプトを Visual Studio のソリューションごと開く
//====================================================================================
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace mye {

// vswhere (-latest -prerelease) で最新の devenv.exe を探す。結果はプロセス内でキャッシュする
// (vswhere の起動に 100 ms 前後かかるため。インストールの増減はエディタ再起動で拾う)。
// 見つからなければ空
const std::wstring& FindLatestDevenv();

// ファイルを「ソリューションのコンテキストで」Visual Studio に開かせる。所有者は EditorApp (1 個)。
//   - solution を開いている起動中の VS を ROT (DTE) から探し、あればそこへ OpenFile + Edit.GoTo
//   - 無ければ devenv をソリューションだけで起動し、読み込みが落ち着いたら Poll が DTE で開く。
//     ★起動引数でファイルを渡さないのは、プロジェクトの読み込みより先に開かれたファイルが
//       「その他のファイル」扱いになり、IntelliSense も行移動も効かないため (実機で確認)
// DTE は同期のプロセス間 COM 呼び出し。VS がモーダルダイアログ中だと拒否されるので、
// そのときは開かずに WARN を出す (同じソリューションの VS を二重に起動しないため)
class VisualStudioLauncher {
public:
    // 戻り値: 開いた / 起動した なら true (失敗は WARN ログ)
    bool Open(const std::wstring& solution, const std::wstring& file, int line);
    // 毎フレーム呼ぶ。起動待ちが無ければ何もしない (DTE の問い合わせは kPollIntervalMs ごと)
    void Poll();

private:
    struct PendingOpen {
        std::wstring solution;
        std::wstring file;
        int line = 0;
        uint32_t pid = 0;             // 起動した devenv
        uint64_t deadlineMs = 0;      // GetTickCount64 基準。過ぎたら諦める
        uint64_t nextPollMs = 0;
        int lastProjectCount = -1;    // 読み込み中は増えていく
        int stablePolls = 0;          // プロジェクト数が変わらなかった連続回数
    };
    std::optional<PendingOpen> pending_;
};

} // namespace mye
