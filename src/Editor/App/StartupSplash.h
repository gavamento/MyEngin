/*----
 StartupSplash.h Editor 起動スプラッシュ (専用スレッド + Direct2D)
 作成者: 秋田蓮音								10/13/2026
----*/
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "Engine/Engine/Loop/StartupProgress.h"

namespace mye {

struct SplashDesc {
    std::wstring projectDisplayName;
    std::wstring projectPath;
    std::wstring imagePath; // 空または読めないときはメビウスの輪
    std::wstring buildCaption;
};

class StartupSplash : public IStartupObserver {
public:
    StartupSplash();
    ~StartupSplash();

    StartupSplash(const StartupSplash&) = delete;
    StartupSplash& operator=(const StartupSplash&) = delete;

    // スプラッシュスレッドを起動する。失敗時は false (メイン起動は継続可能)
    bool Start(const SplashDesc& desc);
    // スレッドを止めて窓を閉じる (冪等)
    void Close();

    void OnStage(StartupStage stage) override;
    void OnReady() override;

private:
    void RunThread();
    void ThreadPaintFrame();

    SplashDesc desc_;
    std::thread thread_;
    std::mutex stateMutex_;
    StartupStage stage_ = StartupStage::Window;
    std::wstring stageLabel_;
    float progress_ = 0.0f;
    int stepDisplay_ = 1;
    std::atomic<bool> running_{ false };
    std::atomic<bool> closeRequested_{ false };
    void* hwnd_ = nullptr; // スプラッシュスレッドだけが触る
};

} // namespace mye
