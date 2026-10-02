#pragma once
#include <string>

#include "Engine/Engine/Loop/EngineLoop.h"

namespace mye {

// ネットワーク窓 (M52i): セッションの状態・ping・ロールバック統計・確定ハッシュを見る。
//
// ★セッションは**起動時**にハンドシェイクで「同じものを走らせているか」を照合して張るもので、
//   走行中に張り直せる形にすると照合の意味が消える (--net-host / --net-join / --net-connect が口)。
//   そのため窓の「接続」ボタンは**エディタをもう 1 つ起動し直す** (--net-connect 付き)。
//   この窓自身のセッションは変えない。
// ★中身は EngineContext::net (NetRuntimeInfo = 毎フレーム更新される POD) だけを読む。
//   Editor 層から NetSession の実装や Winsock を触らないための境界。
class NetWindow {
public:
    bool open = false; // 既定は非表示 (ネットを張っていなければ中身が無い)
    void OnImGui(EngineContext& ctx);

private:
    // 接続ボタンが起動するコマンドライン引数を組む。
    // 失敗 (HOST:PORT が空 / 引用符・制御文字を含む) は空文字列
    static std::wstring BuildConnectArgs(const std::wstring& projectRoot, const std::string& hostPort,
                                         const std::string& playerSessionId);
    bool LaunchClient(const EngineContext& ctx) const;

    char hostPort_[128] = "127.0.0.1:7777";
    char playerSessionId_[128] = "";
    bool wasOpen_ = false; // 前フレームで開いていたか (開いた瞬間の前面化用)
    int launchResult_ = 0; // 0 = 未実行 / 1 = 起動した / -1 = 失敗 (引数不正 or ShellExecute 失敗)
};

} // namespace mye
