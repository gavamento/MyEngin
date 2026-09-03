#include "Editor/SourceControl/ScmHint.h"

namespace mye {
namespace scmhint {
namespace {

// ★std::function ではなく関数ポインタ + user で持つ。エディタは 1 プロセスに
//   1 セッションしか持たず、ここに寿命の長いラムダ (this を捕まえたもの) を
//   置くと「EditorApp を畳んだ後に呼ばれる」経路を自分で作ることになる。
//   解除は SetSink(nullptr, nullptr) の 1 行で済む形にしておく
SinkFn gSink = nullptr;
void* gUser = nullptr;

} // namespace

void SetSink(SinkFn fn, void* user)
{
    gSink = fn;
    gUser = user;
}

void Changed(const std::wstring& absPath)
{
    if (gSink == nullptr || absPath.empty()) {
        return;
    }
    gSink(gUser, absPath);
}

} // namespace scmhint
} // namespace mye
