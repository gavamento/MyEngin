/*----
 StartupSplash.cpp Editor 起動スプラッシュ (専用スレッド + Direct2D)
 作成者: 秋田蓮音								10/13/2026
----*/
#include "Editor/App/StartupSplash.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

#include "Editor/App/StartupMoebius.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Platform/PathUtil.h"
#include "imgui.h"
#include "Engine/Renderer/ImGui/ImGuiTheme.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace mye {
namespace {

constexpr wchar_t kSplashClass[] = L"MyEngineStartupSplash";
constexpr int kSplashW = 640;
constexpr int kSplashH = 360;
constexpr float kMargin = 32.0f;
constexpr float kProgressH = 2.0f;

template<typename T>
void SafeRelease(T*& ptr)
{
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

D2D1_COLOR_F ToD2D(const ImVec4& c, float alphaScale = 1.0f)
{
    return D2D1::ColorF(c.x, c.y, c.z, c.w * alphaScale);
}

StrId StageToStrId(StartupStage stage)
{
    switch (stage) {
    case StartupStage::Window: return StrId::Startup_StageWindow;
    case StartupStage::Device: return StrId::Startup_StageDevice;
    case StartupStage::Shaders: return StrId::Startup_StageShaders;
    case StartupStage::RenderPaths: return StrId::Startup_StageRenderPaths;
    case StartupStage::Ui: return StrId::Startup_StageUi;
    case StartupStage::Scripts: return StrId::Startup_StageScripts;
    case StartupStage::CSharp: return StrId::Startup_StageCSharp;
    case StartupStage::Audio: return StrId::Startup_StageAudio;
    case StartupStage::Assets: return StrId::Startup_StageAssets;
    case StartupStage::Scene: return StrId::Startup_StageScene;
    case StartupStage::Ready: return StrId::Startup_StageScene;
    }
    return StrId::Startup_StageWindow;
}

LRESULT CALLBACK SplashWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_NCCREATE) {
        return TRUE;
    }
    if (msg == WM_CLOSE || msg == WM_DESTROY) {
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void RegisterSplashClass()
{
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SplashWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kSplashClass;
    RegisterClassExW(&wc);
    registered = true;
}

bool LoadSplashBitmap(ID2D1RenderTarget* rt, const std::wstring& path, ID2D1Bitmap** outBitmap)
{
    *outBitmap = nullptr;
    if (path.empty()) {
        return false;
    }
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic)))) {
        return false;
    }
    IWICBitmapDecoder* decoder = nullptr;
    const HRESULT hrOpen =
        wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hrOpen)) {
        SafeRelease(wic);
        return false;
    }
    IWICBitmapFrameDecode* frame = nullptr;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        SafeRelease(decoder);
        SafeRelease(wic);
        return false;
    }
    IWICFormatConverter* converter = nullptr;
    if (FAILED(wic->CreateFormatConverter(&converter))) {
        SafeRelease(frame);
        SafeRelease(decoder);
        SafeRelease(wic);
        return false;
    }
    if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeMedianCut))) {
        SafeRelease(converter);
        SafeRelease(frame);
        SafeRelease(decoder);
        SafeRelease(wic);
        return false;
    }
    ID2D1Bitmap* bmp = nullptr;
    const HRESULT hrBmp = rt->CreateBitmapFromWicBitmap(converter, nullptr, &bmp);
    SafeRelease(converter);
    SafeRelease(frame);
    SafeRelease(decoder);
    SafeRelease(wic);
    if (FAILED(hrBmp)) {
        return false;
    }
    *outBitmap = bmp;
    return true;
}

void DrawCoverImage(ID2D1RenderTarget* rt, ID2D1Bitmap* bmp, const D2D1_RECT_F& dest)
{
    const D2D1_SIZE_F sz = bmp->GetSize();
    if (sz.width <= 0.0f || sz.height <= 0.0f) {
        return;
    }
    const float destW = dest.right - dest.left;
    const float destH = dest.bottom - dest.top;
    const float scale = (std::max)(destW / sz.width, destH / sz.height);
    const float drawW = sz.width * scale;
    const float drawH = sz.height * scale;
    const D2D1_RECT_F draw = {
        dest.left + (destW - drawW) * 0.5f,
        dest.top + (destH - drawH) * 0.5f,
        dest.left + (destW + drawW) * 0.5f,
        dest.top + (destH + drawH) * 0.5f,
    };
    rt->DrawBitmap(bmp, draw, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
}

} // namespace

StartupSplash::StartupSplash() = default;

StartupSplash::~StartupSplash()
{
    Close();
}

bool StartupSplash::Start(const SplashDesc& desc)
{
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    desc_ = desc;
    closeRequested_.store(false, std::memory_order_release);
    stage_ = StartupStage::Window;
    stageLabel_ = Utf8ToWide(Tr(StageToStrId(stage_)));
    progress_ = StartupProgressFraction(stage_);
    stepDisplay_ = 1;
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this]() { RunThread(); });
    return true;
}

void StartupSplash::Close()
{
    closeRequested_.store(true, std::memory_order_release);
    if (hwnd_) {
        PostMessageW(static_cast<HWND>(hwnd_), WM_CLOSE, 0, 0);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false, std::memory_order_release);
    hwnd_ = nullptr;
}

void StartupSplash::OnStage(StartupStage stage)
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    stage_ = stage;
    stageLabel_ = Utf8ToWide(Tr(StageToStrId(stage)));
    progress_ = StartupProgressFraction(stage);
    const int idx = StartupStageIndex(stage);
    stepDisplay_ = idx >= kStartupProgressSteps ? kStartupProgressSteps : idx + 1;
}

void StartupSplash::OnReady()
{
    Close();
}

void StartupSplash::RunThread()
{
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool coInit = SUCCEEDED(coHr);

    RegisterSplashClass();
    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    const int posX = (screenW - kSplashW) / 2;
    const int posY = (screenH - kSplashH) / 2;
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kSplashClass, L"MyEngine Editor", WS_POPUP | WS_VISIBLE, posX,
                                posY, kSplashW, kSplashH, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        MYE_LOG_WARN("[splash] CreateWindowExW failed");
        running_.store(false, std::memory_order_release);
        if (coInit) {
            CoUninitialize();
        }
        return;
    }
    hwnd_ = hwnd;

    ID2D1Factory* d2dFactory = nullptr;
    IDWriteFactory* dwriteFactory = nullptr;
    ID2D1HwndRenderTarget* rt = nullptr;
    IDWriteTextFormat* fmtTitle = nullptr;
    IDWriteTextFormat* fmtBody = nullptr;
    IDWriteTextFormat* fmtMono = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    ID2D1Bitmap* splashImage = nullptr;

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory))) {
        MYE_LOG_WARN("[splash] D2D1CreateFactory failed");
        DestroyWindow(hwnd);
        hwnd_ = nullptr;
        running_.store(false, std::memory_order_release);
        if (coInit) {
            CoUninitialize();
        }
        return;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&dwriteFactory)))) {
        MYE_LOG_WARN("[splash] DWriteCreateFactory failed");
        SafeRelease(d2dFactory);
        DestroyWindow(hwnd);
        hwnd_ = nullptr;
        running_.store(false, std::memory_order_release);
        if (coInit) {
            CoUninitialize();
        }
        return;
    }

    const D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties();
    const D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps =
        D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(kSplashW, kSplashH));
    if (FAILED(d2dFactory->CreateHwndRenderTarget(rtProps, hwndProps, &rt))) {
        MYE_LOG_WARN("[splash] CreateHwndRenderTarget failed");
        SafeRelease(dwriteFactory);
        SafeRelease(d2dFactory);
        DestroyWindow(hwnd);
        hwnd_ = nullptr;
        running_.store(false, std::memory_order_release);
        if (coInit) {
            CoUninitialize();
        }
        return;
    }

    dwriteFactory->CreateTextFormat(L"Bahnschrift", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_NORMAL, 22.0f, L"en-us", &fmtTitle);
    if (!fmtTitle) {
        dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, 22.0f, L"en-us", &fmtTitle);
    }
    dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &fmtBody);
    dwriteFactory->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &fmtMono);
    rt->CreateSolidColorBrush(ToD2D(themeColor::Text), &brush);

    LoadSplashBitmap(rt, desc_.imagePath, &splashImage);

    const auto startTime = std::chrono::steady_clock::now();
    uint64_t logCursor = logging::TotalWritten();
    LogEntry lastLog{};
    lastLog.message[0] = '\0';

    while (!closeRequested_.load(std::memory_order_acquire)) {
        MSG msg;
        while (PeekMessageW(&msg, hwnd, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT || msg.message == WM_CLOSE) {
                closeRequested_.store(true, std::memory_order_release);
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (closeRequested_.load(std::memory_order_acquire)) {
            break;
        }

        LogEntry batch[8];
        const size_t nLogs = logging::ReadSince(logCursor, batch, 8);
        for (size_t i = 0; i < nLogs; ++i) {
            lastLog = batch[i];
        }

        StartupStage stageCopy = StartupStage::Window;
        std::wstring stageLabelCopy;
        std::wstring projectNameCopy;
        std::wstring projectPathCopy;
        std::wstring buildCaptionCopy;
        float progressCopy = 0.0f;
        int stepCopy = 1;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            stageCopy = stage_;
            stageLabelCopy = stageLabel_;
            projectNameCopy = desc_.projectDisplayName;
            projectPathCopy = desc_.projectPath;
            buildCaptionCopy = desc_.buildCaption;
            progressCopy = progress_;
            stepCopy = stepDisplay_;
        }

        const float elapsed =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - startTime).count();

        startupmoebius::BuildParams mobParams;
        mobParams.rotYRad = elapsed * 0.85f;
        mobParams.rotXRad = 0.55f;
        mobParams.centerX = kSplashW * 0.5f;
        mobParams.centerY = 168.0f;
        mobParams.scale = 52.0f;
        std::vector<startupmoebius::WireSegment2D> segments;
        startupmoebius::BuildWireframe(mobParams, segments);
        std::sort(segments.begin(), segments.end(),
                  [](const startupmoebius::WireSegment2D& a, const startupmoebius::WireSegment2D& b) {
                      return a.eyeZ < b.eyeZ;
                  });

        rt->BeginDraw();
        rt->Clear(ToD2D(themeColor::BgApp));

        if (brush) {
            brush->SetColor(ToD2D(themeColor::Border));
            rt->DrawLine({ kMargin, 88.0f }, { kSplashW - kMargin, 88.0f }, brush, 1.0f);
        }

        // ワードマーク
        if (fmtTitle && brush) {
            brush->SetColor(ToD2D(themeColor::Text));
            const wchar_t* word = L"M Y E N G I N E";
            D2D1_RECT_F titleRect = { kMargin, kMargin, kSplashW * 0.65f, kMargin + 28.0f };
            rt->DrawText(word, static_cast<UINT32>(wcslen(word)), fmtTitle, titleRect, brush);
            brush->SetColor(ToD2D(themeColor::TextDim));
            const wchar_t* editorTag = Utf8ToWide(Tr(StrId::Startup_EditorTag)).c_str();
            D2D1_RECT_F editorRect = { kMargin, 52.0f, kSplashW * 0.65f, 72.0f };
            rt->DrawText(editorTag, static_cast<UINT32>(wcslen(editorTag)), fmtBody, editorRect, brush);
        }

        if (fmtBody && brush && !buildCaptionCopy.empty()) {
            brush->SetColor(ToD2D(themeColor::TextDim));
            D2D1_RECT_F capRect = { kSplashW * 0.35f, kMargin, kSplashW - kMargin, kMargin + 48.0f };
            fmtBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            rt->DrawText(buildCaptionCopy.c_str(), static_cast<UINT32>(buildCaptionCopy.size()), fmtBody, capRect,
                         brush);
            fmtBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }

        const D2D1_RECT_F centerRect = { kMargin, 96.0f, kSplashW - kMargin, 248.0f };
        if (splashImage) {
            DrawCoverImage(rt, splashImage, centerRect);
        } else if (brush) {
            const float zMin = 3.0f;
            const float zMax = 5.5f;
            for (const startupmoebius::WireSegment2D& seg : segments) {
                const float t = (seg.eyeZ - zMin) / (zMax - zMin);
                const float clamped = (std::max)(0.0f, (std::min)(1.0f, t));
                const ImVec4 c = {
                    themeColor::TextDim.x + (themeColor::Text.x - themeColor::TextDim.x) * clamped,
                    themeColor::TextDim.y + (themeColor::Text.y - themeColor::TextDim.y) * clamped,
                    themeColor::TextDim.z + (themeColor::Text.z - themeColor::TextDim.z) * clamped,
                    0.35f + 0.65f * clamped,
                };
                brush->SetColor(ToD2D(c));
                rt->DrawLine({ seg.x0, seg.y0 }, { seg.x1, seg.y1 }, brush, 1.0f);
            }
        }

        // 下部情報
        if (fmtBody && brush) {
            brush->SetColor(ToD2D(themeColor::Text));
            D2D1_RECT_F nameRect = { kMargin, 252.0f, kSplashW - kMargin, 272.0f };
            const std::wstring& nameLine =
                projectNameCopy.empty() ? std::wstring(L"MyEngine Editor") : projectNameCopy;
            rt->DrawText(nameLine.c_str(), static_cast<UINT32>(nameLine.size()), fmtBody, nameRect, brush);
            if (!projectPathCopy.empty()) {
                brush->SetColor(ToD2D(themeColor::TextDim));
                D2D1_RECT_F pathRect = { kMargin, 272.0f, kSplashW - kMargin, 288.0f };
                rt->DrawText(projectPathCopy.c_str(), static_cast<UINT32>(projectPathCopy.size()), fmtBody, pathRect,
                             brush);
            }
            wchar_t stageBuf[256];
            swprintf_s(stageBuf, L"%s  %d / %d", stageLabelCopy.c_str(), stepCopy, kStartupProgressSteps);
            brush->SetColor(ToD2D(themeColor::Text));
            D2D1_RECT_F stageRect = { kMargin, 296.0f, kSplashW - 96.0f, 316.0f };
            rt->DrawText(stageBuf, static_cast<UINT32>(wcslen(stageBuf)), fmtBody, stageRect, brush);

            wchar_t timeBuf[32];
            swprintf_s(timeBuf, L"%.1f s", static_cast<double>(elapsed));
            brush->SetColor(ToD2D(themeColor::TextDim));
            D2D1_RECT_F timeRect = { kSplashW - 120.0f, 296.0f, kSplashW - kMargin, 316.0f };
            fmtBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            rt->DrawText(timeBuf, static_cast<UINT32>(wcslen(timeBuf)), fmtBody, timeRect, brush);
            fmtBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }

        // 進捗バー
        if (brush) {
            const float barY = kSplashH - kMargin - kProgressH;
            const D2D1_RECT_F track = { kMargin, barY, kSplashW - kMargin, barY + kProgressH };
            brush->SetColor(ToD2D(themeColor::Border, 0.5f));
            rt->FillRectangle(track, brush);
            D2D1_RECT_F fill = track;
            fill.right = track.left + (track.right - track.left) * progressCopy;
            brush->SetColor(ToD2D(themeColor::Accent, 0.75f));
            rt->FillRectangle(fill, brush);
        }

        if (fmtMono && brush && lastLog.message[0] != '\0') {
            const ImVec4 logColor = lastLog.level == LogLevel::Error   ? themeColor::Error
                                    : lastLog.level == LogLevel::Warn ? themeColor::Warning
                                                                      : themeColor::TextDim;
            brush->SetColor(ToD2D(logColor));
            const std::wstring logLine = Utf8ToWide(lastLog.message);
            D2D1_RECT_F logRect = { kMargin, 318.0f, kSplashW - kMargin, kSplashH - kMargin - 6.0f };
            rt->DrawText(logLine.c_str(), static_cast<UINT32>(logLine.size()), fmtMono, logRect, brush);
        }

        const HRESULT endHr = rt->EndDraw();
        if (endHr == D2DERR_RECREATE_TARGET) {
            SafeRelease(rt);
            d2dFactory->CreateHwndRenderTarget(rtProps, hwndProps, &rt);
            SafeRelease(splashImage);
            if (rt) {
                LoadSplashBitmap(rt, desc_.imagePath, &splashImage);
            }
        }

        Sleep(16);
    }

    SafeRelease(splashImage);
    SafeRelease(brush);
    SafeRelease(fmtMono);
    SafeRelease(fmtBody);
    SafeRelease(fmtTitle);
    SafeRelease(rt);
    SafeRelease(dwriteFactory);
    SafeRelease(d2dFactory);
    if (hwnd) {
        DestroyWindow(hwnd);
    }
    hwnd_ = nullptr;
    running_.store(false, std::memory_order_release);
    if (coInit) {
        CoUninitialize();
    }
}

} // namespace mye
