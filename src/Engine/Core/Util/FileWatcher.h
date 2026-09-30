#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mye {

// ディレクトリ監視 (engine_spec.md 8 章の共通基盤)。
// - 検出はワーカースレッド (ReadDirectoryChangesW, overlapped)
// - 取り出しはメインスレッドのフェーズ 2 (DrainChanges) のみ
// - エディタの保存は 1 回で複数イベントを発火するため、パス毎に
//   「最後のイベントから debounce 時間が経過したもの」だけを放出する
class FileWatcher {
public:
    ~FileWatcher() { Stop(); }

    bool Start(const std::wstring& directory); // 再帰監視
    void Stop();

    // デバウンス済みの変更 (正規化済み絶対パス) を取り出す
    std::vector<std::wstring> DrainChanges();

private:
    void ThreadProc();

    std::wstring root_;
    std::thread thread_;
    void* dirHandle_ = nullptr;  // HANDLE
    void* stopEvent_ = nullptr;  // HANDLE
    std::mutex mutex_;
    struct Pending {
        std::wstring path;    // 正規化済み
        uint64_t lastEventMs; // GetTickCount64
    };
    std::vector<Pending> pending_;
};

} // namespace mye
