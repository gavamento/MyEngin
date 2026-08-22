#pragma once
#include "Engine/Engine/EngineLoop.h"

namespace mye {

// ネットワーク窓 (M52i): セッションの状態・ping・ロールバック統計・確定ハッシュを見る。
//
// ★**読むだけ**の窓。ここから接続したり切断したりはしない — セッションは起動時に
//   ハンドシェイクで「同じものを走らせているか」を照合して張るものなので、
//   走行中に張り直せる形にすると照合の意味が消える (--net-host / --net-join が唯一の口)。
// ★中身は EngineContext::net (NetRuntimeInfo = 毎フレーム更新される POD) だけを読む。
//   Editor 層から NetSession の実装や Winsock を触らないための境界。
class NetWindow {
public:
    bool open = false; // 既定は非表示 (ネットを張っていなければ中身が無い)
    void OnImGui(EngineContext& ctx);
};

} // namespace mye
