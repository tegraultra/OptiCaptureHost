// OptiCaptureHost
//
// A small D3D12 "host" application. It captures one window with Windows Graphics Capture and shows the picture,
// 1:1, in a borderless click-through overlay window that sits exactly on top of the captured window.
//
// It contains no upscaler and no frame generation of its own. The idea is to put OptiScaler (renamed to dxgi.dll)
// next to this exe: OptiScaler then sees a D3D12 "game" that makes no DLSS/FSR/XeSS call and can run its native
// input path (optical flow, FSR pass, XeFG frame generation, XeLL) on the captured picture. The captured game
// itself is never hooked or modified.
//
// Pipeline per frame:
//   WGC frame (D3D11 texture) -> CopySubresourceRegion into a shared texture -> shared fence ->
//   D3D12 queue waits -> CopyResource into the swap chain back buffer -> Present
//
// Hotkeys:  Ctrl+Shift+F12  quit
//           Ctrl+Shift+F11  toggle click-through (turn it off to use the OptiScaler menu with the mouse)
//           Ctrl+Shift+F10  open / close the OptiScaler menu
// (all changeable, together with VSync, in OptiCaptureLauncher.exe, which stores them in OptiCaptureHost.ini; this
// exe re-reads that file while it runs.)

#include <windows.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <timeapi.h>
#include <tlhelp32.h>
#include <avrt.h>
#include <dwmapi.h>

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <cmath>
#include <string>
#include <vector>

#include "settings.h"

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "avrt.lib")

using Microsoft::WRL::ComPtr;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;
namespace wgd11 = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace
{

// ---------------------------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------------------------

struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")) DxgiInterfaceAccess : ::IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID id, void** object) = 0;
};

constexpr int kHotkeyQuit = 1;
constexpr int kHotkeyInteractive = 2;
constexpr int kHotkeyMenu = 3;

bool IsExtendedVk(int vk)
{
    switch (vk)
    {
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
        return true;
    default:
        return false;
    }
}
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

FILE* g_log = nullptr;

void Log(const char* fmt, ...)
{
    if (!g_log)
        return;

    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);

    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_log, fmt, args);
    va_end(args);

    std::fputc('\n', g_log);
    std::fflush(g_log);
}

void Fail(const wchar_t* text)
{
    MessageBoxW(nullptr, text, L"OptiCaptureHost", MB_OK | MB_ICONERROR | MB_TOPMOST);
}

std::wstring Lower(std::wstring s)
{
    for (auto& c : s)
        c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::string Narrow(const std::wstring& w)
{
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// While the game is the foreground window, Windows (Game Mode, background / efficiency throttling, the GPU scheduler)
// favours the game over every other process, and this host is "another process". The host must never be starved:
// it presents every frame of the picture (and OptiScaler's frame generation runs inside it). So raise its CPU and
// GPU priority and switch off the throttling Windows applies to background processes.
// Performance level (OptiCaptureHost.ini [Advanced] Boost, set in the launcher):
//   0 = nothing changed        1 = CPU priority, MMCSS, no power throttling, 1 ms timer
//   2 = 1 + GPU priority class HIGH and a high-priority D3D12 queue
//   3 = 1 + GPU priority class REALTIME (administrator; HIGH without it) and a high-priority D3D12 queue (default)
// A very high GPU priority helps on strong GPUs, but on a weak GPU it can starve the game itself (and with it the
// capture), so weaker cards should use a lower level.
int g_boost = 3;

void BoostProcess(int level)
{
    g_boost = std::clamp(level, 0, 3);
    Log("Performance level %d", g_boost);
    if (g_boost == 0)
        return;

    if (!SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS))
        Log("SetPriorityClass(HIGH) failed: %lu", GetLastError());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    DWORD task = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Games", &task);
    Log("MMCSS (Games) %s", mm ? "on" : "unavailable");

    PROCESS_POWER_THROTTLING_STATE pt{};
    pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    pt.StateMask = 0; // 0 = throttling off for both
    if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt)))
        Log("Could not switch off power throttling: %lu", GetLastError());

    timeBeginPeriod(1);

    // GPU scheduling class of the whole process (REALTIME needs administrator rights, HIGH does not).
    using SetClassFn = LONG(WINAPI*)(HANDLE, int);
    if (g_boost >= 2)
    if (HMODULE gdi = LoadLibraryW(L"gdi32.dll"))
    {
        auto fn = reinterpret_cast<SetClassFn>(GetProcAddress(gdi, "D3DKMTSetProcessSchedulingPriorityClass"));
        if (fn)
        {
            LONG st = 1;
            if (g_boost >= 3)
                st = fn(GetCurrentProcess(), 5); // REALTIME
            if (st != 0)
                st = fn(GetCurrentProcess(), 4); // HIGH
            Log("GPU scheduling priority class request: NTSTATUS 0x%08X (%s)", static_cast<unsigned>(st), st == 0 ? "ok" : "refused");
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Focus spoof
// ---------------------------------------------------------------------------------------------------------------
// Measured: with the overlay focused, every captured frame becomes two presented frames; as soon as the game has the
// focus (Ctrl+Shift+F11) the presents drop to one per captured frame, i.e. frame generation silently stops. Something
// in the chain (DLSS-G / the Enabler, OptiScaler) only generates frames while the window it presents to is the
// foreground window, and during play the foreground window has to be the game so that it gets keyboard and mouse.
//
// So the modules loaded in this process (OptiScaler, the frame generation DLLs, ...) are told a small lie: while the
// game is the real foreground window, their GetForegroundWindow() returns our overlay. This is done by replacing that
// entry in their import tables; this exe's own code (and Windows itself) keep seeing the truth.
using GetForegroundFn = HWND(WINAPI*)();
GetForegroundFn g_realGetForeground = nullptr;
HWND g_spoofTarget = nullptr;
HWND g_spoofOverlay = nullptr;

HWND WINAPI SpoofedGetForegroundWindow()
{
    const HWND real = g_realGetForeground ? g_realGetForeground() : nullptr;
    return (real && real == g_spoofTarget && g_spoofOverlay) ? g_spoofOverlay : real;
}

int PatchImportsOfModule(HMODULE mod)
{
    int patched = 0;
    __try
    {
        auto* base = reinterpret_cast<BYTE*>(mod);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return 0;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return 0;
        const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress)
            return 0;

        auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
        for (; imp->Name; ++imp)
        {
            auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
            for (; thunk->u1.Function; ++thunk)
            {
                if (thunk->u1.Function != reinterpret_cast<ULONG_PTR>(g_realGetForeground))
                    continue;
                DWORD old = 0;
                if (VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old))
                {
                    thunk->u1.Function = reinterpret_cast<ULONG_PTR>(&SpoofedGetForegroundWindow);
                    VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
                    ++patched;
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return patched;
}

// Patches every module that is not part of Windows (the graphics driver's own folder counts as "not Windows"), once.
void PatchNewModules(std::vector<HMODULE>& seen)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE)
        return;

    wchar_t win[MAX_PATH] = {};
    GetSystemWindowsDirectoryW(win, MAX_PATH);
    const std::wstring winDir = Lower(win);
    const HMODULE self = GetModuleHandleW(nullptr);

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me))
    {
        do
        {
            const HMODULE m = me.hModule;
            if (m == self || std::find(seen.begin(), seen.end(), m) != seen.end())
                continue;
            seen.push_back(m);

            const std::wstring path = Lower(me.szExePath);
            const bool windowsFile = path.rfind(winDir, 0) == 0 && path.find(L"\\driverstore\\") == std::wstring::npos;
            if (windowsFile)
                continue;

            const int n = PatchImportsOfModule(m);
            if (n > 0)
                Log("Focus spoof: %d import(s) patched in %s", n, Narrow(me.szModule).c_str());
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
}

std::wstring WindowTitle(HWND h)
{
    wchar_t buf[512] = {};
    GetWindowTextW(h, buf, 512);
    return buf;
}

bool GetTargetBounds(HWND target, RECT& r)
{
    if (SUCCEEDED(DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
        return true;
    return GetWindowRect(target, &r) != FALSE;
}

// ---------------------------------------------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------------------------------------------

struct Options
{
    std::wstring title;      // --title <text>: capture the first visible window whose title contains this
    int delaySec = 5;        // --delay <s>: without --title, capture the foreground window after this many seconds
    bool vsync = true;       // --vsync 0|1
    int buffers = 3;         // --buffers 2..4
    int overlayAlpha = 254;  // [Advanced] OverlayAlpha 1..255: below 255 DWM must blend the overlay (no direct flip)
    int affinity = 0;        // [Advanced] Affinity: 0 off, 1 WDA_MONITOR, 2 WDA_EXCLUDEFROMCAPTURE (forces DWM composition)
    double paceK = 1.0;      // [Advanced] PaceK: host frames are PaceK / refresh apart with VSync + Arthur (default 1 = base rate equals the refresh rate; 0 = measured from the FG multiplier)
    int startDelayMs = 1500; // [Advanced] StartDelayMs: pause before the swap chain is created
    int testMode = 0; // [Advanced] TestMode: 1 = capture only (nothing is shown), 2 = show without copying new frames (diagnostics)
    bool noLayered = false; // [Advanced] NoLayered=1 (test only): plain overlay window, no layered style, so no click-through
    bool probe = false; // [Advanced] Probe=1: compare small patches of every frame (diagnostic, costs a little GPU time)
    double presentDelayMs = 0.0; // [Advanced] PresentDelayMs: hold each frame this long before Present (keeps it off the DWM compose boundary)
    int gpu = -1;            // --gpu <index>: DXGI adapter index (default: high-performance adapter)
    bool clickThrough = true; // --no-clickthrough
    int menuKey = VK_INSERT;  // --menu-key <vk>: the key that opens OptiScaler's menu (e.g. 0x2D, its default, Insert)
    HWND hwnd = nullptr;      // --hwnd <n>: capture exactly this window (what the launcher passes)
    DWORD parent = 0;         // --parent <pid>: the launcher; quit when it is gone
};

bool ParseOptions(Options& o)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return false;

    bool ok = true;
    for (int i = 1; i < argc; ++i)
    {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return (i + 1 < argc) ? std::wstring(argv[++i]) : std::wstring(); };

        if (a == L"--title")
            o.title = next();
        else if (a == L"--delay")
            o.delaySec = std::clamp(_wtoi(next().c_str()), 0, 120);
        else if (a == L"--vsync")
            o.vsync = _wtoi(next().c_str()) != 0;
        else if (a == L"--buffers")
            o.buffers = std::clamp(_wtoi(next().c_str()), 2, 4);
        else if (a == L"--gpu")
            o.gpu = _wtoi(next().c_str());
        else if (a == L"--no-clickthrough")
            o.clickThrough = false;
        else if (a == L"--hwnd")
            o.hwnd = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(_wcstoui64(next().c_str(), nullptr, 10)));
        else if (a == L"--parent")
            o.parent = static_cast<DWORD>(_wtoi64(next().c_str()));
        else if (a == L"--menu-key")
            o.menuKey = std::clamp(static_cast<int>(wcstol(next().c_str(), nullptr, 0)), 1, 254);
        else
            ok = false;
    }

    LocalFree(argv);
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------
// Window picking
// ---------------------------------------------------------------------------------------------------------------

struct FindCtx
{
    std::wstring needle;
    HWND found = nullptr;
};

BOOL CALLBACK FindProc(HWND h, LPARAM lp)
{
    auto* ctx = reinterpret_cast<FindCtx*>(lp);
    if (!IsWindowVisible(h) || IsIconic(h))
        return TRUE;
    if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
        return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId())
        return TRUE;

    if (Lower(WindowTitle(h)).find(ctx->needle) != std::wstring::npos)
    {
        ctx->found = h;
        return FALSE;
    }
    return TRUE;
}

HWND PickTarget(const Options& o)
{
    if (o.hwnd)
        return IsWindow(o.hwnd) ? o.hwnd : nullptr;

    if (!o.title.empty())
    {
        FindCtx ctx;
        ctx.needle = Lower(o.title);
        EnumWindows(FindProc, reinterpret_cast<LPARAM>(&ctx));
        return ctx.found;
    }

    Log("Switch to the game window now: capturing the foreground window in %d s", o.delaySec);
    MessageBeep(MB_OK);
    Sleep(static_cast<DWORD>(o.delaySec) * 1000);

    HWND h = GetAncestor(GetForegroundWindow(), GA_ROOT);
    if (h)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId())
            return nullptr;
    }
    return h;
}

// ---------------------------------------------------------------------------------------------------------------
// The host
// ---------------------------------------------------------------------------------------------------------------

LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_ERASEBKGND)
        return 1;
    if (m == WM_CLOSE) // the launcher's Stop button: end the message loop instead of destroying the window under it
    {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

class Host
{
  public:
    int Run(const Options& opt, const oc::Settings& settings);

  private:
    void RegisterHotkeys();
    void UnregisterHotkeys();
    void PollSettings();
    bool LauncherInFront() const;
    bool InitGraphics();
    bool InitCapture();
    bool InitOverlayAndSwapChain();
    bool CreateShared();
    bool PresentFrame(ID3D11Texture2D* src);
    bool ComputeCrop(UINT capW, UINT capH, int& cx, int& cy, UINT& w, UINT& h);
    bool ApplyGeometry(UINT capW, UINT capH);
    bool RefreshGeometry();
    void LogGeometry();
    void MoveOverlay();
    void EnforceOverlayStyle();
    void ToggleInteractive();
    bool ForceForeground(HWND h);
    void OpenMenu();
    void SendMenuKey(bool up);
    void WaitFenceValue(UINT64 v);
    void WaitGpu();
    void Shutdown();
    void PublishLive(std::chrono::steady_clock::time_point now);

    Options opt_;
    HWND target_ = nullptr;
    HWND overlay_ = nullptr;
    HINSTANCE inst_ = nullptr;
    UINT width_ = 0;
    UINT height_ = 0;
    bool interactive_ = false;
    int menuKeyStage_ = 0; // 0 idle, 1 waiting to press the menu key, 2 waiting to release it
    std::chrono::steady_clock::time_point menuKeyAt_{};
    bool tearing_ = false;
    UINT swapFlags_ = 0;
    HANDLE waitable_ = nullptr; // Lossless-style mode: swap chain frame latency waitable object (owned, closed at shutdown)
    int waitTimeouts_ = 0;
    RECT lastBounds_ = {};
    UINT capW_ = 0; // size of the captured frame (the whole window)
    UINT capH_ = 0;
    int cropX_ = 0; // the part of that frame that is the window's client area (what we show)
    int cropY_ = 0;
    volatile bool targetClosed_ = false;
    bool focusSpoof_ = false;
    std::vector<HMODULE> spoofSeen_;
    std::chrono::steady_clock::time_point spoofScanAt_{};

    oc::Settings st_;
    unsigned long long iniStamp_ = 0;
    unsigned long long iniPending_ = 0;
    HANDLE parentProc_ = nullptr;

    // D3D12 side
    ComPtr<IDXGIFactory6> factory_;
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<ID3D12Device> dev12_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swap_;
    std::vector<ComPtr<ID3D12CommandAllocator>> allocs_;
    std::vector<UINT64> allocFence_;
    // VSync pacing (Arthur): each host frame makes k presents and the Arthur path ignores the sync interval, so the
    // host spaces its own frames k / refresh apart. k is measured from DXGI present counts.
    ComPtr<IDXGIOutput> paceOutput_;
    double refreshHz_ = 0.0;
    double pacePerFrame_ = 1.0;
    UINT paceLastPresents_ = 0;
    UINT64 paceFrames_ = 0;
    std::chrono::steady_clock::time_point paceStatAt_{};
    std::chrono::steady_clock::time_point paceLast_{};
    std::chrono::steady_clock::time_point halfNext_{}; // present mode 5: when the next base frame is due
    bool halfSkip_ = false;                           // present mode 5: the last turn found no new frame, do not wait again
    double waitMs_ = 0.0;    // stats: time spent waiting for the allocator fence / inside Present, per 5 s
    double presentMs_ = 0.0;

    // Live status for the launcher (OptiCaptureHost.status, once a second).
    std::atomic<unsigned> arrivals_{ 0 }; // frames Windows Graphics Capture delivered
    std::chrono::steady_clock::time_point liveAt_{};
    unsigned liveArrivals_ = 0;
    UINT64 liveFrames_ = 0;
    double liveWait_ = 0.0;
    double livePresent_ = 0.0;
    UINT liveDxgiPresents_ = 0;
    bool liveDxgiValid_ = false;
    int lastInterval_ = 0;
    std::vector<double> capHist_; // the last capture rates, to tell a capped game from a slow one

    // Frame time diagnostics, logged every 5 s: gaps between captured frames and between the host's presents, and the
    // longest time the housekeeping (focus spoof scan, window checks) kept the presenting thread busy.
    std::mutex arrMutex_;
    std::vector<double> arrGaps_;
    std::chrono::steady_clock::time_point arrLast_{};
    std::vector<double> presGaps_;
    std::vector<double> latMs_; // age of the captured frame (Windows composed it) when the host hands it to Present
    std::vector<double> srcGaps_; // gaps between the capture time stamps Windows put on the frames
    long long lastSrc100ns_ = 0;
    ComPtr<ID3D11Texture2D> probe_[2]; // tiny staging copies of 32 screen patches: tells whether the picture really changed
    bool probeValid_[2] = { false, false };
    int probeIdx_ = 0;
    unsigned long long lastHash_ = 0;
    bool haveHash_ = false;
    int contentNew_ = 0, contentRep_ = 0, repRun_ = 0, repMax_ = 0;
    // GPU time of the host's own copies (measured with timestamp queries)
    ComPtr<ID3D12QueryHeap> tsHeap_;
    ComPtr<ID3D12Resource> tsRead_;
    UINT64 tsFreq_ = 0;
    double gpu12Ms_ = 0.0, gpu12Max_ = 0.0;
    int gpu12N_ = 0;
    struct Q11
    {
        ComPtr<ID3D11Query> dj, a, b;
        bool pending = false;
    };
    Q11 q11_[4];
    int q11Idx_ = 0;
    bool q11Failed_ = false;
    double gpu11Ms_ = 0.0, gpu11Max_ = 0.0;
    int gpu11N_ = 0;
    std::chrono::steady_clock::time_point presLast_{};
    double houseMaxMs_ = 0.0;
    double spoofMaxMs_ = 0.0;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> frameFence_;
    UINT64 frameFenceValue_ = 0;
    HANDLE fenceEvent_ = nullptr;

    // D3D11 side (WGC needs a D3D11 device) and the shared texture/fence between the two
    ComPtr<ID3D11Device> dev11_;
    ComPtr<ID3D11DeviceContext> ctx11_;
    ComPtr<ID3D11DeviceContext4> ctx11_4_;
    ComPtr<ID3D11Fence> fence11_;
    ComPtr<ID3D12Fence> fence12_;
    UINT64 sharedFenceValue_ = 0;
    ComPtr<ID3D11Texture2D> shared11_;
    ComPtr<ID3D12Resource> shared12_;

    // Capture
    HANDLE frameEvent_ = nullptr;
    wgd11::IDirect3DDevice winrtDevice_{ nullptr };
    wgc::GraphicsCaptureItem item_{ nullptr };
    wgc::Direct3D11CaptureFramePool pool_{ nullptr };
    wgc::GraphicsCaptureSession session_{ nullptr };
};

bool Host::InitGraphics()
{
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_));
    if (FAILED(hr))
    {
        Log("CreateDXGIFactory2 failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    if (opt_.gpu >= 0)
        hr = factory_->EnumAdapters1(static_cast<UINT>(opt_.gpu), &adapter_);
    else
        hr = factory_->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter_));
    if (FAILED(hr))
    {
        Log("No adapter found: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    DXGI_ADAPTER_DESC1 ad{};
    adapter_->GetDesc1(&ad);
    Log("Adapter: %s", Narrow(ad.Description).c_str());

    hr = D3D12CreateDevice(adapter_.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev12_));
    if (FAILED(hr))
    {
        Log("D3D12CreateDevice failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Priority = g_boost >= 2 ? D3D12_COMMAND_QUEUE_PRIORITY_HIGH : D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    hr = dev12_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_));
    if (FAILED(hr))
        return false;

    hr = dev12_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&frameFence_));
    if (FAILED(hr))
        return false;
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // D3D11 device on the same adapter, for Windows Graphics Capture.
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    hr = D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                           static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &dev11_, nullptr, &ctx11_);
    if (FAILED(hr))
    {
        Log("D3D11CreateDevice failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    {
        ComPtr<IDXGIDevice> d;
        if (g_boost >= 2 && SUCCEEDED(dev11_.As(&d)))
            d->SetGPUThreadPriority(7);
    }

    ComPtr<ID3D11Device5> dev11_5;
    if (FAILED(dev11_.As(&dev11_5)) || FAILED(ctx11_.As(&ctx11_4_)))
    {
        Log("ID3D11Device5 / ID3D11DeviceContext4 not available (Windows 10 1703 or newer is required)");
        return false;
    }

    // One fence shared between D3D11 (signals after the copy) and D3D12 (waits before reading).
    hr = dev11_5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence11_));
    if (FAILED(hr))
    {
        Log("ID3D11Device5::CreateFence failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    HANDLE fh = nullptr;
    hr = fence11_->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fh);
    if (FAILED(hr))
        return false;
    hr = dev12_->OpenSharedHandle(fh, IID_PPV_ARGS(&fence12_));
    CloseHandle(fh);
    if (FAILED(hr))
    {
        Log("OpenSharedHandle (fence) failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    BOOL allow = FALSE;
    if (SUCCEEDED(factory_->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
        tearing_ = allow != FALSE;

    return true;
}

bool Host::InitCapture()
{
    frameEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(dev11_.As(&dxgiDevice)))
        return false;

    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put()));
    winrtDevice_ = inspectable.as<wgd11::IDirect3DDevice>();

    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    winrt::check_hresult(interop->CreateForWindow(target_, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item_)));

    auto size = item_.Size();
    capW_ = static_cast<UINT>(size.Width);
    capH_ = static_cast<UINT>(size.Height);
    Log("Capture item: %ux%u", capW_, capH_);
    if (capW_ == 0 || capH_ == 0)
        return false;

    {
        int cx = 0;
        int cy = 0;
        UINT cw = capW_;
        UINT ch = capH_;
        ComputeCrop(capW_, capH_, cx, cy, cw, ch);
        cropX_ = cx;
        cropY_ = cy;
        width_ = cw;
        height_ = ch;
    }
    LogGeometry();

    item_.Closed([this](auto&&, auto&&) { targetClosed_ = true; });

    pool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(winrtDevice_, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
    pool_.FrameArrived([this](auto&&, auto&&) {
        const auto tn = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lk(arrMutex_);
            if (arrLast_.time_since_epoch().count() != 0 && arrGaps_.size() < 4096)
                arrGaps_.push_back(std::chrono::duration<double, std::milli>(tn - arrLast_).count());
            arrLast_ = tn;
        }
        arrivals_.fetch_add(1, std::memory_order_relaxed);
        SetEvent(frameEvent_);
    });

    session_ = pool_.CreateCaptureSession(item_);
    try
    {
        session_.IsCursorCaptureEnabled(false);
    }
    catch (...)
    {
        Log("IsCursorCaptureEnabled not available");
    }
    // Windows 11 captures at most one frame per MinUpdateInterval, which defaults to 1/60 s. On a 75 Hz or faster
    // display that drops every other frame (75 Hz -> 37.5 fps), so ask for every frame DWM composes.
    // Classic mode (Present mode 3, the 2.2.0 behaviour) leaves the Windows default, which caps capture at 60 fps.
    if (st_.presentMode != 3)
    {
        try
        {
            session_.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{ 10000 }); // 1 ms
            Log("Capture MinUpdateInterval set to 1 ms");
        }
        catch (...)
        {
            Log("MinUpdateInterval not available (older Windows): capture may be limited to 60 fps");
        }
    }
    else
    {
        Log("Classic mode: capture left at the Windows default rate");
    }
    try
    {
        session_.IsBorderRequired(false);
    }
    catch (...)
    {
        Log("IsBorderRequired(false) not available (a yellow border may be drawn around the captured window)");
    }
    return true;
}

bool Host::InitOverlayAndSwapChain()
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = inst_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"OptiCaptureHostOverlay";
    RegisterClassExW(&wc);

    RECT r{};
    GetTargetBounds(target_, r);
    lastBounds_ = r;

    DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | (opt_.noLayered ? 0 : WS_EX_LAYERED);
    if (opt_.clickThrough && !opt_.noLayered)
        ex |= WS_EX_TRANSPARENT;
    interactive_ = !opt_.clickThrough;

    overlay_ = CreateWindowExW(ex, wc.lpszClassName, L"OptiCaptureHost", WS_POPUP, r.left + cropX_, r.top + cropY_, static_cast<int>(width_),
                               static_cast<int>(height_), nullptr, nullptr, inst_, nullptr);
    if (!overlay_)
    {
        Log("CreateWindowEx failed: %lu", GetLastError());
        return false;
    }
    if (!opt_.noLayered)
        SetLayeredWindowAttributes(overlay_, 0, static_cast<BYTE>(opt_.overlayAlpha), LWA_ALPHA);
    if (opt_.affinity == 1 || opt_.affinity == 2)
    {
        const BOOL ok = SetWindowDisplayAffinity(overlay_, opt_.affinity == 1 ? WDA_MONITOR : 0x00000011 /* WDA_EXCLUDEFROMCAPTURE */);
        Log("Overlay display affinity %d: %s", opt_.affinity, ok ? "ok" : "failed");
    }

    // The tearing flag can only be chosen when the swap chain is created, so VSync off at start-up is what allows
    // real tearing (lowest latency); switching VSync later in the launcher still changes the sync interval.
    // Lossless-style mode (Present mode 4): always create the swap chain with tearing allowed and a latency waitable
    // object, like Lossless Scaling does, so the sync interval and the frame queue can be chosen freely.
    const bool lsMode = st_.presentMode == 4;
    swapFlags_ = ((!opt_.vsync || lsMode) && tearing_) ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (lsMode)
        swapFlags_ |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = width_;
    sd.Height = height_;
    sd.Format = kFormat;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = static_cast<UINT>(opt_.buffers);
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = swapFlags_;

    ComPtr<IDXGISwapChain1> sc1;
    // The first run after the launcher starts can fail here, while OptiScaler and the frame generation libraries inside
    // this process are still starting up; later runs are fine. A short pause before the first try, and a few retries,
    // make the first run work too. [Advanced] StartDelayMs in OptiCaptureHost.ini changes the pause (default 1500).
    Sleep(static_cast<DWORD>(opt_.startDelayMs));
    HRESULT hr = E_FAIL;
    for (int attempt = 1; attempt <= 6; ++attempt)
    {
        sc1.Reset();
        hr = factory_->CreateSwapChainForHwnd(queue_.Get(), overlay_, &sd, nullptr, nullptr, &sc1);
        if (SUCCEEDED(hr))
            break;
        Log("CreateSwapChainForHwnd failed (try %d of 6): 0x%08X", attempt, static_cast<unsigned>(hr));
        Sleep(1000);
    }
    if (FAILED(hr))
    {
        Log("CreateSwapChainForHwnd failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    factory_->MakeWindowAssociation(overlay_, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    if (FAILED(sc1.As(&swap_)))
        return false;

    if (lsMode)
    {
        const HRESULT lh = swap_->SetMaximumFrameLatency(1);
        waitable_ = FAILED(lh) ? nullptr : swap_->GetFrameLatencyWaitableObject();
        Log("Lossless-style mode: frame latency 1 %s, waitable object %s, tearing %s, %u buffers",
            FAILED(lh) ? "FAILED" : "ok", waitable_ ? "ok" : "missing",
            (swapFlags_ & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) ? "allowed" : "not available", sd.BufferCount);
    }

    allocs_.resize(sd.BufferCount);
    allocFence_.assign(sd.BufferCount, 0);
    for (auto& a : allocs_)
    {
        if (FAILED(dev12_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))))
            return false;
    }
    if (FAILED(dev12_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocs_[0].Get(), nullptr, IID_PPV_ARGS(&list_))))
        return false;
    list_->Close();

    tsHeap_.Reset();
    tsRead_.Reset();
    {
        D3D12_QUERY_HEAP_DESC qh{};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = sd.BufferCount * 2;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = static_cast<UINT64>(sd.BufferCount) * 16;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (!(SUCCEEDED(dev12_->CreateQueryHeap(&qh, IID_PPV_ARGS(&tsHeap_))) &&
              SUCCEEDED(dev12_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        IID_PPV_ARGS(&tsRead_))) &&
              SUCCEEDED(queue_->GetTimestampFrequency(&tsFreq_)) && tsFreq_ != 0))
        {
            tsHeap_.Reset();
            tsRead_.Reset();
        }
    }

    ShowWindow(overlay_, SW_SHOWNOACTIVATE);
    return CreateShared();
}

bool Host::CreateShared()
{
    shared11_.Reset();
    shared12_.Reset();

    D3D11_TEXTURE2D_DESC d{};
    d.Width = width_;
    d.Height = height_;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = kFormat;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    HRESULT hr = dev11_->CreateTexture2D(&d, nullptr, &shared11_);
    if (FAILED(hr))
    {
        Log("CreateTexture2D (shared) failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    ComPtr<IDXGIResource1> res;
    if (FAILED(shared11_.As(&res)))
        return false;

    HANDLE h = nullptr;
    hr = res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h);
    if (FAILED(hr))
    {
        Log("CreateSharedHandle failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    hr = dev12_->OpenSharedHandle(h, IID_PPV_ARGS(&shared12_));
    CloseHandle(h);
    if (FAILED(hr))
    {
        Log("OpenSharedHandle (texture) failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    return true;
}

void Host::WaitFenceValue(UINT64 v)
{
    if (v == 0)
        return;
    if (frameFence_->GetCompletedValue() < v)
    {
        frameFence_->SetEventOnCompletion(v, fenceEvent_);
        WaitForSingleObject(fenceEvent_, 2000);
    }
}

void Host::WaitGpu()
{
    if (!queue_ || !frameFence_)
        return;
    ++frameFenceValue_;
    queue_->Signal(frameFence_.Get(), frameFenceValue_);
    WaitFenceValue(frameFenceValue_);
}

bool Host::PresentFrame(ID3D11Texture2D* src)
{
    // 1) D3D11: copy the captured frame into the shared texture, then signal the shared fence.
    D3D11_BOX box{ static_cast<UINT>(cropX_), static_cast<UINT>(cropY_), 0, static_cast<UINT>(cropX_) + width_,
                   static_cast<UINT>(cropY_) + height_, 1 };
    Q11* q = nullptr;
    if (!q11Failed_)
    {
        q = &q11_[q11Idx_];
        q11Idx_ = (q11Idx_ + 1) & 3;
        if (!q->dj)
        {
            D3D11_QUERY_DESC qd{};
            qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            bool ok = SUCCEEDED(dev11_->CreateQuery(&qd, &q->dj));
            qd.Query = D3D11_QUERY_TIMESTAMP;
            ok = ok && SUCCEEDED(dev11_->CreateQuery(&qd, &q->a)) && SUCCEEDED(dev11_->CreateQuery(&qd, &q->b));
            if (!ok)
            {
                q11Failed_ = true;
                q = nullptr;
            }
        }
        else if (q->pending)
        {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dd{};
            UINT64 ta = 0, tb = 0;
            if (ctx11_->GetData(q->dj.Get(), &dd, sizeof(dd), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                ctx11_->GetData(q->a.Get(), &ta, sizeof(ta), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                ctx11_->GetData(q->b.Get(), &tb, sizeof(tb), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK)
            {
                if (!dd.Disjoint && tb > ta && dd.Frequency)
                {
                    const double ms = 1000.0 * static_cast<double>(tb - ta) / static_cast<double>(dd.Frequency);
                    gpu11Ms_ += ms;
                    gpu11Max_ = (std::max)(gpu11Max_, ms);
                    ++gpu11N_;
                }
                q->pending = false;
            }
            else
            {
                q = nullptr; // not ready yet: skip measuring this frame
            }
        }
    }
    if (q)
    {
        ctx11_->Begin(q->dj.Get());
        ctx11_->End(q->a.Get());
    }
    if (opt_.testMode != 2)
        ctx11_->CopySubresourceRegion(shared11_.Get(), 0, 0, 0, 0, src, 0, &box);
    if (q)
    {
        ctx11_->End(q->b.Get());
        ctx11_->End(q->dj.Get());
        q->pending = true;
    }
    ++sharedFenceValue_;
    ctx11_4_->Signal(fence11_.Get(), sharedFenceValue_);
    ctx11_->Flush();

    // 2) D3D12: wait for that copy on the GPU, then copy into the back buffer and present.
    queue_->Wait(fence12_.Get(), sharedFenceValue_);

    const UINT bbIdx = swap_->GetCurrentBackBufferIndex();
    const UINT idx = bbIdx;
    const auto tw0 = std::chrono::steady_clock::now();
    WaitFenceValue(allocFence_[idx]);
    {
        const double w = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tw0).count();
        waitMs_ += w;
        liveWait_ += w;
    }

    ComPtr<ID3D12Resource> bb;
    if (FAILED(swap_->GetBuffer(bbIdx, IID_PPV_ARGS(&bb))))
        return false;

    if (tsRead_ && allocFence_[idx] != 0)
    {
        D3D12_RANGE rr{ static_cast<SIZE_T>(idx) * 16, static_cast<SIZE_T>(idx) * 16 + 16 };
        void* base = nullptr;
        if (SUCCEEDED(tsRead_->Map(0, &rr, &base)) && base)
        {
            const UINT64* t = reinterpret_cast<const UINT64*>(static_cast<const unsigned char*>(base) + static_cast<size_t>(idx) * 16);
            if (t[1] > t[0])
            {
                const double ms = 1000.0 * static_cast<double>(t[1] - t[0]) / static_cast<double>(tsFreq_);
                gpu12Ms_ += ms;
                gpu12Max_ = (std::max)(gpu12Max_, ms);
                ++gpu12N_;
            }
            D3D12_RANGE wr{ 0, 0 };
            tsRead_->Unmap(0, &wr);
        }
    }

    allocs_[idx]->Reset();
    list_->Reset(allocs_[idx].Get(), nullptr);
    if (tsHeap_)
        list_->EndQuery(tsHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, idx * 2);

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = bb.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    list_->ResourceBarrier(1, &b);

    list_->CopyResource(bb.Get(), shared12_.Get());

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    list_->ResourceBarrier(1, &b);

    if (tsHeap_)
    {
        list_->EndQuery(tsHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, idx * 2 + 1);
        list_->ResolveQueryData(tsHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, idx * 2, 2, tsRead_.Get(), static_cast<UINT64>(idx) * 16);
    }
    list_->Close();
    ID3D12CommandList* lists[] = { list_.Get() };
    queue_->ExecuteCommandLists(1, lists);
    bb.Reset();

    const UINT flags = (!opt_.vsync && (swapFlags_ & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    // Frame generation presents several frames per captured frame. Blocking each of them on a vblank caps the base
    // frame rate at refresh/N and adds a queue of latency, so with FG on the interval stays 0 (the flip-model overlay is
    // still composed by DWM at vblank, so it does not tear). A real vsync interval is only used without FG.
    const bool fgActive = st_.frameGenMode != 2;
    UINT interval = (opt_.vsync && !fgActive) ? 1 : 0;
    if (st_.presentMode == 1) interval = 0;
    else if (st_.presentMode == 2) interval = 1;
    else if (st_.presentMode == 3 || st_.presentMode == 5) interval = opt_.vsync ? 1 : 0; // classic: the host's own VSync, FG or not
    else if (st_.presentMode == 4) interval = opt_.vsync ? 1 : 0; // Lossless-style: VSync only when asked for, otherwise tearing
    const auto tp0 = std::chrono::steady_clock::now();
    HRESULT hr = swap_->Present(interval, flags);
    {
        const double p = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tp0).count();
        presentMs_ += p;
        livePresent_ += p;
    }
    lastInterval_ = static_cast<int>(interval);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
    {
        Log("Device lost on Present: 0x%08X (removed reason 0x%08X)", static_cast<unsigned>(hr),
            static_cast<unsigned>(dev12_->GetDeviceRemovedReason()));
        return false;
    }

    ++frameFenceValue_;
    queue_->Signal(frameFence_.Get(), frameFenceValue_);
    allocFence_[idx] = frameFenceValue_;
    return true;
}

// The captured frame is the whole window, with any title bar and border. What we show (and what frame generation
// sees) is only its client area, so work out where that sits inside the frame.
bool Host::ComputeCrop(UINT capW, UINT capH, int& cx, int& cy, UINT& w, UINT& h)
{
    cx = 0;
    cy = 0;
    w = capW;
    h = capH;

    RECT fr{};
    RECT cr{};
    POINT p{ 0, 0 };
    if (!GetTargetBounds(target_, fr) || !GetClientRect(target_, &cr) || !ClientToScreen(target_, &p))
        return false;

    int x = p.x - fr.left;
    int y = p.y - fr.top;
    int cw = cr.right - cr.left;
    int ch = cr.bottom - cr.top;

    if (x < 0)
    {
        cw += x;
        x = 0;
    }
    if (y < 0)
    {
        ch += y;
        y = 0;
    }
    if (x + cw > static_cast<int>(capW))
        cw = static_cast<int>(capW) - x;
    if (y + ch > static_cast<int>(capH))
        ch = static_cast<int>(capH) - y;

    if (cw < 16 || ch < 16)
        return false;

    cx = x;
    cy = y;
    w = static_cast<UINT>(cw);
    h = static_cast<UINT>(ch);
    return true;
}

void Host::LogGeometry()
{
    RECT fr{};
    RECT wr{};
    RECT cr{};
    POINT p{ 0, 0 };
    GetTargetBounds(target_, fr);
    GetWindowRect(target_, &wr);
    GetClientRect(target_, &cr);
    ClientToScreen(target_, &p);

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(target_, MONITOR_DEFAULTTONEAREST), &mi);

    Log("Window rect (%ld,%ld)-(%ld,%ld), frame bounds (%ld,%ld)-(%ld,%ld), client %ldx%ld at (%ld,%ld)", wr.left, wr.top,
        wr.right, wr.bottom, fr.left, fr.top, fr.right, fr.bottom, cr.right - cr.left, cr.bottom - cr.top, p.x, p.y);
    Log("Monitor (%ld,%ld)-(%ld,%ld); showing %ux%u cropped at (%d,%d) from a %ux%u capture", mi.rcMonitor.left,
        mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom, width_, height_, cropX_, cropY_, capW_, capH_);
}

// Called when the captured frame size or the client area changed.
bool Host::ApplyGeometry(UINT capW, UINT capH)
{
    int cx = 0;
    int cy = 0;
    UINT w = capW;
    UINT h = capH;
    ComputeCrop(capW, capH, cx, cy, w, h);

    const bool capChanged = capW != capW_ || capH != capH_;
    const bool outChanged = w != width_ || h != height_;
    Log("Geometry changed: capture %ux%u, showing %ux%u at crop (%d,%d)", capW, capH, w, h, cx, cy);

    WaitGpu();

    capW_ = capW;
    capH_ = capH;
    cropX_ = cx;
    cropY_ = cy;

    if (outChanged)
    {
        width_ = w;
        height_ = h;

        if (!CreateShared())
            return false;

        HRESULT hr = swap_->ResizeBuffers(0, width_, height_, DXGI_FORMAT_UNKNOWN, swapFlags_);
        if (FAILED(hr))
        {
            Log("ResizeBuffers failed: 0x%08X", static_cast<unsigned>(hr));
            return false;
        }
    }

    if (capChanged)
    {
        pool_.Recreate(winrtDevice_, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2,
                       winrt::Windows::Graphics::SizeInt32{ static_cast<int32_t>(capW), static_cast<int32_t>(capH) });
    }

    lastBounds_ = {};
    MoveOverlay();
    return true;
}

// Cheap check, a few times a second: did the client area change inside the same captured frame?
bool Host::RefreshGeometry()
{
    int cx = 0;
    int cy = 0;
    UINT w = 0;
    UINT h = 0;
    if (!ComputeCrop(capW_, capH_, cx, cy, w, h))
        return true;

    if (w != width_ || h != height_)
        return ApplyGeometry(capW_, capH_);

    if (cx != cropX_ || cy != cropY_)
    {
        cropX_ = cx;
        cropY_ = cy;
        lastBounds_ = {};
    }
    return true;
}

void Host::MoveOverlay()
{
    RECT r{};
    if (!GetTargetBounds(target_, r))
        return;

    if (IsIconic(target_))
    {
        ShowWindow(overlay_, SW_HIDE);
        return;
    }
    if (!IsWindowVisible(overlay_))
        ShowWindow(overlay_, SW_SHOWNOACTIVATE);

    EnforceOverlayStyle();

    // OptiScaler can also move or resize the window it presents to (its "Force Borderless" option): put it back on
    // the game's picture.
    RECT orc{};
    if (GetWindowRect(overlay_, &orc) &&
        (orc.left != r.left + cropX_ || orc.top != r.top + cropY_ || orc.right - orc.left != static_cast<LONG>(width_) ||
         orc.bottom - orc.top != static_cast<LONG>(height_)))
    {
        lastBounds_ = {};
    }

    if (r.left != lastBounds_.left || r.top != lastBounds_.top || r.right != lastBounds_.right || r.bottom != lastBounds_.bottom)
    {
        lastBounds_ = r;
        SetWindowPos(overlay_, HWND_TOPMOST, r.left + cropX_, r.top + cropY_, static_cast<int>(width_), static_cast<int>(height_),
                     SWP_NOACTIVATE);
    }
    else
    {
        // Keep the overlay above a game that re-asserts its own topmost state (but not above the launcher window).
        // Only when some other window actually got above the overlay: re-ordering the topmost windows every 250 ms
        // while the game is running makes DWM redo its composition and costs the game frames.
        if (!LauncherInFront() && GetWindow(overlay_, GW_HWNDPREV) != nullptr)
            SetWindowPos(overlay_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

// OptiScaler's "Force Borderless" (and its restyle on swap chain changes) rewrites the extended style of the window it
// presents to, which is our overlay: layered, click-through and no-activate all vanish, and the overlay then keeps the
// mouse and the focus. Put back what we need, a few times a second.
void Host::EnforceOverlayStyle()
{
    if (opt_.noLayered)
        return;
    constexpr LONG_PTR mask = WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_APPWINDOW;
    LONG_PTR want = WS_EX_TOOLWINDOW | WS_EX_LAYERED;
    if (!interactive_)
        want |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;

    const LONG_PTR have = GetWindowLongPtrW(overlay_, GWL_EXSTYLE);
    if ((have & mask) == want)
        return;

    Log("Overlay style was changed from outside (0x%08lX), restoring it", static_cast<unsigned long>(have));
    SetWindowLongPtrW(overlay_, GWL_EXSTYLE, (have & ~mask) | want);
    SetLayeredWindowAttributes(overlay_, 0, static_cast<BYTE>(opt_.overlayAlpha), LWA_ALPHA);
    SetWindowPos(overlay_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    lastBounds_ = {};
}

void Host::ToggleInteractive()
{
    interactive_ = !interactive_;
    LONG_PTR ex = GetWindowLongPtrW(overlay_, GWL_EXSTYLE);
    if (interactive_)
        ex &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
    else
        ex |= static_cast<LONG_PTR>(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
    SetWindowLongPtrW(overlay_, GWL_EXSTYLE, ex);
    SetWindowPos(overlay_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    // Interactive: the overlay takes keyboard focus, so OptiScaler (which only accepts its keys, Insert for the menu,
    // while the window it presents to is the foreground window) actually gets them. Back to click-through: focus
    // returns to the game.
    if (interactive_)
    {
        const bool ok = ForceForeground(overlay_);
        Log("Click-through OFF: overlay takes focus (%s). Mouse and keyboard now go to the overlay / OptiScaler menu.",
            ok ? "ok" : "WINDOWS REFUSED, click once on the picture");
    }
    else
    {
        ForceForeground(target_);
        Log("Click-through ON: focus returned to the game.");
    }
}

// Windows only lets the foreground process hand the foreground to someone else. Attaching to the current foreground
// window's input thread (and a synthetic Alt tap) is the usual way around that for a window the user asked for.
bool Host::ForceForeground(HWND h)
{
    HWND fg = GetForegroundWindow();
    const DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD me = GetCurrentThreadId();

    bool attached = false;
    if (fgThread != 0 && fgThread != me)
        attached = AttachThreadInput(me, fgThread, TRUE) != FALSE;

    keybd_event(VK_MENU, 0, 0, 0);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);

    BringWindowToTop(h);
    SetForegroundWindow(h);
    SetActiveWindow(h);
    SetFocus(h);

    if (attached)
        AttachThreadInput(me, fgThread, FALSE);

    Sleep(60);
    const bool ok = GetForegroundWindow() == h;
    Log("Foreground request: %s (foreground is now \"%s\")", ok ? "ok" : "failed",
        Narrow(WindowTitle(GetForegroundWindow())).c_str());
    return ok;
}

void Host::SendMenuKey(bool up)
{
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = static_cast<WORD>(opt_.menuKey);
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(static_cast<UINT>(opt_.menuKey), MAPVK_VK_TO_VSC));
    in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (IsExtendedVk(opt_.menuKey) ? KEYEVENTF_EXTENDEDKEY : 0);
    SendInput(1, &in, sizeof(in));
}

// Ctrl+Shift+F10: take focus for the overlay and press OptiScaler's menu key for you (a moment later, so that
// OptiScaler has seen the focus change first). Press it again to close the menu.
void Host::OpenMenu()
{
    if (!interactive_)
        ToggleInteractive();
    else
        ForceForeground(overlay_);

    Log("Pressing the menu key (vk 0x%02X) for OptiScaler", opt_.menuKey);
    menuKeyStage_ = 1;
    menuKeyAt_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
}

// Once a second: write what the launcher shows in its live panel.
void Host::PublishLive(std::chrono::steady_clock::time_point now)
{
    const double dt = std::chrono::duration<double>(now - liveAt_).count();
    liveAt_ = now;
    if (dt <= 0.0)
        return;

    oc::LiveStats ls;
    ls.stamp = GetTickCount64();
    ls.pid = GetCurrentProcessId();
    ls.refreshHz = refreshHz_;

    const unsigned arr = arrivals_.load(std::memory_order_relaxed);
    ls.captureFps = static_cast<double>(arr - liveArrivals_) / dt;
    liveArrivals_ = arr;

    ls.presentFps = static_cast<double>(liveFrames_) / dt;
    ls.presentMs = liveFrames_ ? livePresent_ / static_cast<double>(liveFrames_) : 0.0;
    ls.waitMs = liveFrames_ ? liveWait_ / static_cast<double>(liveFrames_) : 0.0;
    liveFrames_ = 0;
    livePresent_ = 0.0;
    liveWait_ = 0.0;

    DXGI_FRAME_STATISTICS fs{};
    if (swap_ && SUCCEEDED(swap_->GetFrameStatistics(&fs)))
    {
        if (liveDxgiValid_)
            ls.dxgiFps = static_cast<double>(fs.PresentCount - liveDxgiPresents_) / dt;
        liveDxgiPresents_ = fs.PresentCount;
        liveDxgiValid_ = true;
    }

    ls.interval = lastInterval_;
    const HWND fg = GetForegroundWindow();
    ls.focus = fg == target_ ? 0 : (fg == overlay_ ? 1 : 2);

    // Does the game look capped? Windows Graphics Capture reports an uncapped game at (at most) the refresh rate, so
    // "at the refresh rate" proves nothing. A capture rate that stays almost exactly the same for several seconds and
    // sits clearly below the refresh rate is what a VSync lock or a frame limiter looks like (a slow game wobbles).
    // Classic mode leaves the capture at Windows' own 60 fps ceiling, so nothing can be said there.
    capHist_.push_back(ls.captureFps);
    if (capHist_.size() > 6)
        capHist_.erase(capHist_.begin());
    if (st_.presentMode != 3 && refreshHz_ > 0.0 && capHist_.size() == 6)
    {
        double mn = capHist_[0];
        double mx = capHist_[0];
        double sum = 0.0;
        for (double v : capHist_)
        {
            mn = (std::min)(mn, v);
            mx = (std::max)(mx, v);
            sum += v;
        }
        const double mean = sum / static_cast<double>(capHist_.size());
        if (mean >= 20.0 && mean < 0.97 * refreshHz_ && (mx - mn) <= 0.02 * mean + 0.6)
        {
            ls.capHint = 1;
            ls.capFps = mean;
        }
    }

    oc::WriteLiveStats(ls);
}

void Host::Shutdown()
{
    DeleteFileW(oc::StatusPath().c_str());
    DeleteFileW((oc::StatusPath() + L".tmp").c_str());
    try
    {
        if (session_)
            session_.Close();
        if (pool_)
            pool_.Close();
    }
    catch (...)
    {
    }

    WaitGpu();
    UnregisterHotkeys();
    if (parentProc_)
        CloseHandle(parentProc_);
    if (waitable_)
    {
        CloseHandle(waitable_);
        waitable_ = nullptr;
    }
    swap_.Reset();
    g_spoofOverlay = nullptr;
    if (overlay_)
        DestroyWindow(overlay_);
    if (fenceEvent_)
        CloseHandle(fenceEvent_);
    if (frameEvent_)
        CloseHandle(frameEvent_);
}

bool Host::LauncherInFront() const
{
    HWND fg = GetForegroundWindow();
    if (!fg)
        return false;
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    return wcscmp(cls, L"OptiCaptureLauncher") == 0;
}

void Host::RegisterHotkeys()
{
    UnregisterHotkeys();
    const struct
    {
        int id;
        oc::Hotkey key;
        const char* name;
    } keys[] = {
        { kHotkeyQuit, st_.stop, "stop" },
        { kHotkeyInteractive, st_.focus, "focus" }, // Ctrl+Shift+F11: game <-> overlay (not shown in the launcher)
        { kHotkeyMenu, st_.menu, "menu" },          // Ctrl+Shift+F10: press OptiScaler's menu key for it
    };

    for (const auto& k : keys)
    {
        if (!RegisterHotKey(nullptr, k.id, k.key.mods | MOD_NOREPEAT, k.key.vk))
            Log("Could not register the %s shortcut (mods %u, vk %u): already used by another program", k.name, k.key.mods, k.key.vk);
    }
}

void Host::UnregisterHotkeys()
{
    UnregisterHotKey(nullptr, kHotkeyQuit);
    UnregisterHotKey(nullptr, kHotkeyInteractive);
    UnregisterHotKey(nullptr, kHotkeyMenu);
}

// The launcher rewrites OptiCaptureHost.ini when the user changes something; pick that up while running. The file is
// written key by key, so wait until its time stamp has stayed the same for one check before reading it.
void Host::PollSettings()
{
    const unsigned long long stamp = oc::FileStamp(oc::SettingsPath());
    if (stamp == iniStamp_)
    {
        iniPending_ = 0;
        return;
    }
    if (stamp != iniPending_)
    {
        iniPending_ = stamp;
        return;
    }
    iniStamp_ = stamp;
    iniPending_ = 0;

    const oc::Settings old = st_;
    oc::LoadSettings(st_);

    if (st_.vsync != old.vsync)
    {
        opt_.vsync = st_.vsync;
        Log("VSync %s", st_.vsync ? "on" : "off");
    }
    if (st_.presentDelay != old.presentDelay)
    {
        opt_.presentDelayMs = static_cast<double>(st_.presentDelay);
        Log("PresentDelayMs = %d (live)", st_.presentDelay);
    }
    if (st_.presentMode != old.presentMode)
        Log("Present mode %d (0 auto, 1 interval 0, 2 interval 1, 3 classic, 4 Lossless-style, 5 classic half refresh) - restart the capture to apply", st_.presentMode);
    if (st_.menuKey != old.menuKey)
    {
        opt_.menuKey = static_cast<int>(st_.menuKey);
        Log("OptiScaler menu key is now vk 0x%02X", st_.menuKey);
    }
    if (st_.stop != old.stop || st_.focus != old.focus || st_.menu != old.menu)
    {
        RegisterHotkeys();
        Log("Shortcuts updated");
    }
}

int Host::Run(const Options& opt, const oc::Settings& settings)
{
    opt_ = opt;
    st_ = settings;
    BoostProcess(st_.boost);

    // [Advanced] Buffers=2..4 in OptiCaptureHost.ini: swap chain buffer count (for experiments).
    opt_.buffers = std::clamp<int>(static_cast<int>(GetPrivateProfileIntW(L"Advanced", L"Buffers", st_.presentMode == 4 ? 2 : opt_.buffers, oc::SettingsPath().c_str())), 2, 4);
    Log("Swap chain buffers: %d", opt_.buffers);
    opt_.overlayAlpha = std::clamp<int>(static_cast<int>(GetPrivateProfileIntW(L"Advanced", L"OverlayAlpha", 254, oc::SettingsPath().c_str())), 1, 255);
    opt_.affinity = std::clamp<int>(static_cast<int>(GetPrivateProfileIntW(L"Advanced", L"Affinity", 0, oc::SettingsPath().c_str())), 0, 2);
    {
        wchar_t kb[32] = {};
        GetPrivateProfileStringW(L"Advanced", L"PaceK", L"1", kb, 32, oc::SettingsPath().c_str());
        opt_.paceK = std::clamp(_wtof(kb), 0.0, 8.0);
    }
    if (st_.presentMode == 3 || st_.presentMode == 4 || st_.presentMode == 5)
        opt_.overlayAlpha = 255; // classic / Lossless-style: plain opaque overlay (lets Windows flip it straight to the screen)
    Log("Overlay alpha %d, display affinity %d, PaceK %.2f", opt_.overlayAlpha, opt_.affinity, opt_.paceK);
    opt_.startDelayMs = std::clamp<int>(static_cast<int>(GetPrivateProfileIntW(L"Advanced", L"StartDelayMs", 1500, oc::SettingsPath().c_str())), 0, 10000);
    opt_.presentDelayMs = static_cast<double>(std::clamp(st_.presentDelay, 0, 12));
    if (opt_.presentDelayMs > 0.0)
        Log("PresentDelayMs = %.0f", opt_.presentDelayMs);
    opt_.probe = GetPrivateProfileIntW(L"Advanced", L"Probe", 0, oc::SettingsPath().c_str()) != 0;
    if (opt_.probe)
        Log("Probe on (picture change check)");
    opt_.noLayered = GetPrivateProfileIntW(L"Advanced", L"NoLayered", 0, oc::SettingsPath().c_str()) != 0;
    if (opt_.noLayered)
        Log("NoLayered test: the overlay is a plain window (mouse clicks will not pass through)");
    opt_.testMode = std::clamp<int>(static_cast<int>(GetPrivateProfileIntW(L"Advanced", L"TestMode", 0, oc::SettingsPath().c_str())), 0, 2);
    if (opt_.testMode)
        Log("TEST MODE %d (diagnostics only: %s)", opt_.testMode, opt_.testMode == 1 ? "frames are captured but nothing is shown" : "the picture is shown but new frames are not copied");

    {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm))
            Log("Primary display: %lux%lu at %lu Hz", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency);
        if (dm.dmDisplayFrequency > 1)
            refreshHz_ = static_cast<double>(dm.dmDisplayFrequency);
    }
    iniStamp_ = oc::FileStamp(oc::SettingsPath());
    inst_ = GetModuleHandleW(nullptr);

    if (opt_.parent != 0)
        parentProc_ = OpenProcess(SYNCHRONIZE, FALSE, opt_.parent);

    if (!wgc::GraphicsCaptureSession::IsSupported())
    {
        Fail(L"Windows Graphics Capture is not supported on this system (Windows 10 1903 or newer is required).");
        return 1;
    }

    target_ = PickTarget(opt_);
    if (!target_)
    {
        Fail(L"No window to capture. Use --title \"part of the window title\", or switch to the game during the countdown.");
        return 1;
    }
    Log("Target window: \"%s\"", Narrow(WindowTitle(target_)).c_str());
    if (opt_.parent == 0)
        MessageBeep(MB_ICONASTERISK);

    if (!InitGraphics() || !InitCapture() || !InitOverlayAndSwapChain())
    {
        Fail(L"Initialisation failed. See OptiCaptureHost.log next to the exe.");
        Shutdown();
        return 1;
    }

    RegisterHotkeys();

    // [Advanced] FocusSpoof=0 in OptiCaptureHost.ini turns the focus spoof off.
    focusSpoof_ = GetPrivateProfileIntW(L"Advanced", L"FocusSpoof", 1, oc::SettingsPath().c_str()) != 0;
    if (focusSpoof_)
    {
        g_realGetForeground = reinterpret_cast<GetForegroundFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetForegroundWindow"));
        if (g_realGetForeground)
        {
            g_spoofTarget = target_;
            g_spoofOverlay = overlay_;
            PatchNewModules(spoofSeen_);
            spoofScanAt_ = std::chrono::steady_clock::now();
            Log("Focus spoof on");
        }
        else
        {
            focusSpoof_ = false;
        }
    }
    else
    {
        Log("Focus spoof off");
    }

    session_.StartCapture();
    liveAt_ = std::chrono::steady_clock::now();
    Log("Capture started (VSync %s). Shortcuts (defaults): Ctrl+Shift+F12 quits, F11 toggles click-through, F10 opens the OptiScaler menu.",
        opt_.vsync ? "on" : "off");
    {
        // Show what OptiScaler.ini says about vsync / frame generation, since OptiScaler can override our Present interval.
        std::ifstream f(oc::ExeDir() + L"OptiScaler.ini");
        std::string line;
        while (std::getline(f, line))
        {
            size_t a = line.find_first_not_of(" \t");
            if (a == std::string::npos || line[a] == ';') continue;
            if (_strnicmp(line.c_str() + a, "ForceVsync", 10) == 0 || _strnicmp(line.c_str() + a, "VsyncInterval", 13) == 0 ||
                _strnicmp(line.c_str() + a, "FGOutput", 8) == 0 || _strnicmp(line.c_str() + a, "FramerateLimit", 14) == 0 ||
                _strnicmp(line.c_str() + a, "UseGamesReflexMarkers", 21) == 0)
                Log("OptiScaler.ini: %s", line.c_str() + a);
        }
    }

    auto lastHousekeeping = std::chrono::steady_clock::now();
    UINT64 frames = 0;
    int slowLogged = 0;
    UINT lastPresentCount = 0;
    UINT lastRefreshCount = 0;
    auto statStart = lastHousekeeping;
    bool running = true;

    while (running)
    {
        MsgWaitForMultipleObjects(1, &frameEvent_, FALSE, 50, QS_ALLINPUT);

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                running = false;
            else if (msg.message == WM_HOTKEY && msg.wParam == kHotkeyQuit)
                running = false;
            else if (msg.message == WM_HOTKEY && msg.wParam == kHotkeyInteractive)
                ToggleInteractive();
            else if (msg.message == WM_HOTKEY && msg.wParam == kHotkeyMenu)
                OpenMenu();
            else
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }

        if (!running)
            break;
        if (targetClosed_ || !IsWindow(target_))
        {
            Log("Captured window closed");
            break;
        }

        const auto now = std::chrono::steady_clock::now();

        if (menuKeyStage_ != 0 && now >= menuKeyAt_)
        {
            if (menuKeyStage_ == 1)
            {
                SendMenuKey(false);
                menuKeyStage_ = 2;
                menuKeyAt_ = now + std::chrono::milliseconds(120);
            }
            else
            {
                SendMenuKey(true);
                menuKeyStage_ = 0;
            }
        }

        if (now - lastHousekeeping > std::chrono::milliseconds(250))
        {
            lastHousekeeping = now;
            const auto hk0 = std::chrono::steady_clock::now();
            if (focusSpoof_ && now - spoofScanAt_ > std::chrono::seconds(1))
            {
                spoofScanAt_ = now; // OptiScaler loads the frame generation DLLs a little later
                PatchNewModules(spoofSeen_);
                spoofMaxMs_ = (std::max)(spoofMaxMs_, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - hk0).count());
            }
            if (parentProc_ && WaitForSingleObject(parentProc_, 0) == WAIT_OBJECT_0)
            {
                Log("Launcher closed");
                break;
            }
            PollSettings();
            if (!RefreshGeometry())
                break;
            MoveOverlay();
            houseMaxMs_ = (std::max)(houseMaxMs_, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - hk0).count());
        }
        if (now - liveAt_ >= std::chrono::seconds(1))
            PublishLive(now);
        if (now - statStart > std::chrono::seconds(5))
        {
            const HWND fg = GetForegroundWindow();
            Log("Presented %.1f fps (focus: %s)", static_cast<double>(frames) / std::chrono::duration<double>(now - statStart).count(),
                fg == target_ ? "game" : (fg == overlay_ ? "overlay" : "other"));
            if (frames > 0)
                Log("  per frame: fence wait %.2f ms, inside Present %.2f ms", waitMs_ / frames, presentMs_ / frames);
            waitMs_ = 0.0;
            presentMs_ = 0.0;

            // Frame times: averages hide stutter, so show the spread (99th percentile, longest gap, how many gaps were
            // more than 1.5 times the average).
            {
                std::vector<double> ag;
                {
                    std::lock_guard<std::mutex> lk(arrMutex_);
                    ag.swap(arrGaps_);
                }
                auto describe = [](std::vector<double>& v, double& avg, double& p99, double& mx, int& slow)
                {
                    avg = p99 = mx = 0.0;
                    slow = 0;
                    if (v.empty())
                        return false;
                    double sum = 0.0;
                    for (double x : v)
                        sum += x;
                    avg = sum / static_cast<double>(v.size());
                    for (double x : v)
                        if (x > avg * 1.5)
                            ++slow;
                    std::sort(v.begin(), v.end());
                    mx = v.back();
                    p99 = v[std::min<size_t>(v.size() - 1, static_cast<size_t>(static_cast<double>(v.size()) * 0.99))];
                    return true;
                };
                double a, p, m;
                int s;
                if (describe(ag, a, p, m, s))
                    Log("  capture gaps: avg %.1f ms, 99%% %.1f ms, max %.1f ms, %d of %zu slow", a, p, m, s, ag.size());
                if (describe(presGaps_, a, p, m, s))
                    Log("  present gaps: avg %.1f ms, 99%% %.1f ms, max %.1f ms, %d of %zu slow", a, p, m, s, presGaps_.size());
                if (describe(latMs_, a, p, m, s))
                    Log("  frame age at Present (negative = frame is for a vblank still ahead): avg %.1f ms, 99%% %.1f ms, max %.1f ms",
                        a - 500.0, p - 500.0, m - 500.0);
                latMs_.clear();
                if (describe(srcGaps_, a, p, m, s))
                    Log("  frame time stamps from Windows: avg %.1f ms, 99%% %.1f ms, max %.1f ms, %d of %zu slow", a, p, m, s, srcGaps_.size());
                srcGaps_.clear();
                if (contentNew_ + contentRep_ > 0)
                    Log("  picture changed: %d new, %d identical to the one before (longest run %d) of %d", contentNew_, contentRep_,
                        repMax_, contentNew_ + contentRep_);
                contentNew_ = contentRep_ = repMax_ = 0;
                if (gpu11N_ > 0 || gpu12N_ > 0)
                    Log("  host GPU time per frame: D3D11 copy %.2f ms (max %.2f), D3D12 copy %.2f ms (max %.2f)",
                        gpu11N_ ? gpu11Ms_ / gpu11N_ : 0.0, gpu11Max_, gpu12N_ ? gpu12Ms_ / gpu12N_ : 0.0, gpu12Max_);
                gpu11Ms_ = gpu11Max_ = gpu12Ms_ = gpu12Max_ = 0.0;
                gpu11N_ = gpu12N_ = 0;
                Log("  housekeeping: longest %.2f ms (focus spoof scan %.2f ms)", houseMaxMs_, spoofMaxMs_);
                presGaps_.clear();
                houseMaxMs_ = 0.0;
                spoofMaxMs_ = 0.0;
            }

            // What DXGI says it really put on screen: presents that reached the display vs display refreshes.
            DXGI_FRAME_STATISTICS fs{};
            if (swap_ && SUCCEEDED(swap_->GetFrameStatistics(&fs)))
            {
                Log("  DXGI stats: presents %u (+%u), refreshes shown %u (+%u), sync refresh %u", fs.PresentCount,
                    fs.PresentCount - lastPresentCount, fs.PresentRefreshCount, fs.PresentRefreshCount - lastRefreshCount,
                    fs.SyncRefreshCount);
                lastPresentCount = fs.PresentCount;
                lastRefreshCount = fs.PresentRefreshCount;
            }
            else
            {
                Log("  DXGI stats unavailable");
            }

            // Which windows are above the overlay right now (anything topmost that covers it costs the overlay its fast path).
            {
                std::string above;
                int n = 0;
                for (HWND w = GetWindow(overlay_, GW_HWNDPREV); w && n < 4; w = GetWindow(w, GW_HWNDPREV))
                {
                    if (!IsWindowVisible(w))
                        continue;
                    above += " [" + Narrow(WindowTitle(w)) + "]";
                    ++n;
                }
                Log("  above the overlay:%s", above.empty() ? " nothing" : above.c_str());
            }
            frames = 0;
            statStart = now;
        }

        // VSync with Arthur frame generation: keep the host's frames k / refresh apart (k = presents per host frame).
        if (opt_.vsync && st_.frameGenMode == 1 && st_.presentMode != 1 && st_.presentMode != 3 && st_.presentMode != 4 && st_.presentMode != 5 && refreshHz_ > 0.0)
        {
            const auto np = std::chrono::steady_clock::now();
            if (opt_.paceK >= 0.25)
                pacePerFrame_ = opt_.paceK;
            else if (np - paceStatAt_ > std::chrono::milliseconds(500))
            {
                DXGI_FRAME_STATISTICS pfs{};
                if (swap_ && SUCCEEDED(swap_->GetFrameStatistics(&pfs)))
                {
                    if (paceFrames_ >= 8 && paceStatAt_.time_since_epoch().count() != 0)
                    {
                        double k = static_cast<double>(pfs.PresentCount - paceLastPresents_) / static_cast<double>(paceFrames_);
                        if (std::fabs(k - std::round(k)) < 0.15)
                            k = std::round(k);
                        if (k >= 1.0 && k <= 8.0)
                            pacePerFrame_ = 0.5 * pacePerFrame_ + 0.5 * k;
                    }
                    paceLastPresents_ = pfs.PresentCount;
                }
                paceFrames_ = 0;
                paceStatAt_ = np;
                if (!paceOutput_ && swap_)
                    swap_->GetContainingOutput(&paceOutput_);
            }
            if (!paceOutput_ && swap_)
                swap_->GetContainingOutput(&paceOutput_);
            // Wait until just before the next slot, then line the frame up with a vblank so the real and generated
            // frames of Arthur land on the display refresh grid.
            const auto target = paceLast_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                std::chrono::duration<double>(pacePerFrame_ / refreshHz_)) -
                                std::chrono::milliseconds(3);
            for (;;)
            {
                const auto left = target - std::chrono::steady_clock::now();
                if (left <= std::chrono::steady_clock::duration::zero())
                    break;
                if (left > std::chrono::milliseconds(2))
                    Sleep(1);
                else
                    YieldProcessor();
            }
            if (paceOutput_ && FAILED(paceOutput_->WaitForVBlank()))
                paceOutput_.Reset();
        }

        // Present mode 5 (Classic, half refresh): hand the frame generator exactly refresh / 2 frames per second (60 Hz -> 30,
        // 75 Hz -> 37.5), no more. Extra frames would only be generated, queued and dropped, and load the GPU. The wait ends on
        // a real vblank, so every base frame is exactly two refreshes after the previous one.
        if (st_.presentMode == 5 && refreshHz_ > 0.0)
        {
            if (halfSkip_)
            {
                halfSkip_ = false;
            }
            else
            {
                const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(2.0 / refreshHz_));
                auto nowT = std::chrono::steady_clock::now();
                if (halfNext_.time_since_epoch().count() == 0 || nowT > halfNext_ + period)
                    halfNext_ = nowT; // first frame, or we fell behind: start over
                if (!paceOutput_ && swap_)
                    swap_->GetContainingOutput(&paceOutput_);
                const auto margin = paceOutput_ ? std::chrono::milliseconds(3) : std::chrono::milliseconds(0);
                for (;;)
                {
                    const auto left = halfNext_ - margin - std::chrono::steady_clock::now();
                    if (left <= std::chrono::steady_clock::duration::zero())
                        break;
                    if (left > std::chrono::milliseconds(2))
                        Sleep(1);
                    else
                        YieldProcessor();
                }
                if (paceOutput_)
                {
                    if (SUCCEEDED(paceOutput_->WaitForVBlank()))
                        halfNext_ = std::chrono::steady_clock::now() + period;
                    else
                    {
                        paceOutput_.Reset();
                        halfNext_ += period;
                    }
                }
                else
                {
                    halfNext_ += period;
                }
            }
        }

        // Take the newest captured frame and drop older ones (lower latency than queueing them).
        wgc::Direct3D11CaptureFrame latest{ nullptr };
        for (;;)
        {
            auto f = pool_.TryGetNextFrame();
            if (!f)
                break;
            latest = f;
        }
        if (!latest)
        {
            halfSkip_ = true;
            continue;
        }
        const auto tGot = std::chrono::steady_clock::now();

        // Lossless-style mode: let the swap chain's frame queue drain first (latency 1), then take the newest captured
        // frame again, so the picture shown is as fresh as possible. If the wait keeps timing out (the presents are not
        // coming through this swap chain), the wait switches itself off.
        if (waitable_)
        {
            if (WaitForSingleObjectEx(waitable_, 20, FALSE) == WAIT_TIMEOUT)
            {
                if (++waitTimeouts_ >= 5)
                {
                    Log("Frame latency wait timed out 5 times in a row: switched off");
                    CloseHandle(waitable_);
                    waitable_ = nullptr;
                }
            }
            else
            {
                waitTimeouts_ = 0;
            }
            for (;;)
            {
                auto f = pool_.TryGetNextFrame();
                if (!f)
                    break;
                latest = f;
            }
        }

        // Hold the frame until a fixed point inside the refresh cycle, then take the newest one again. The frame arrives
        // right when Windows starts composing the next screen; presenting at that moment races the composition, so the
        // frame lands one refresh earlier or later from one frame to the next. A short fixed delay removes the race.
        if (opt_.presentDelayMs > 0.0)
        {
            const auto until = tGot + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                          std::chrono::duration<double, std::milli>(opt_.presentDelayMs));
            for (;;)
            {
                const auto left = until - std::chrono::steady_clock::now();
                if (left <= std::chrono::steady_clock::duration::zero())
                    break;
                if (left > std::chrono::milliseconds(2))
                    Sleep(1);
                else
                    YieldProcessor();
            }
            for (;;)
            {
                auto f = pool_.TryGetNextFrame();
                if (!f)
                    break;
                latest = f;
            }
        }

        const auto cs = latest.ContentSize();
        if (cs.Width <= 0 || cs.Height <= 0)
            continue;
        if (static_cast<UINT>(cs.Width) != capW_ || static_cast<UINT>(cs.Height) != capH_)
        {
            latest = nullptr;
            if (!ApplyGeometry(static_cast<UINT>(cs.Width), static_cast<UINT>(cs.Height)))
                break;
            continue;
        }

        ComPtr<ID3D11Texture2D> tex;
        {
            auto surface = latest.Surface();
            winrt::com_ptr<DxgiInterfaceAccess> access;
            winrt::check_hresult(winrt::get_unknown(surface)->QueryInterface(__uuidof(DxgiInterfaceAccess), access.put_void()));
            winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(tex.GetAddressOf())));
        }

        {
            LARGE_INTEGER qpc{}, freq{};
            QueryPerformanceCounter(&qpc);
            QueryPerformanceFrequency(&freq);
            const double nowMs = 1000.0 * static_cast<double>(qpc.QuadPart) / static_cast<double>(freq.QuadPart);
            const long long src = latest.SystemRelativeTime().count();
            const double age = nowMs - static_cast<double>(src) / 10000.0;
            if (latMs_.size() < 4096 && age > -500.0 && age < 2000.0)
                latMs_.push_back(age + 500.0); // shifted so negative values survive the statistics; shown shifted back
            if (lastSrc100ns_ != 0 && srcGaps_.size() < 4096)
                srcGaps_.push_back(static_cast<double>(src - lastSrc100ns_) / 10000.0);
            lastSrc100ns_ = src;
        }

        // Did the picture really change? 32 small patches spread over the screen are copied to a staging texture and
        // compared with the previous frame (read one frame later, so nothing waits for the GPU).
        {
            D3D11_TEXTURE2D_DESC td{};
            tex->GetDesc(&td);
            if (opt_.probe && (td.Format == DXGI_FORMAT_B8G8R8A8_UNORM || td.Format == DXGI_FORMAT_R8G8B8A8_UNORM) && td.Width >= 256 && td.Height >= 128)
            {
                if (!probe_[0])
                {
                    D3D11_TEXTURE2D_DESC pd{};
                    pd.Width = 64;
                    pd.Height = 32;
                    pd.MipLevels = 1;
                    pd.ArraySize = 1;
                    pd.Format = td.Format;
                    pd.SampleDesc.Count = 1;
                    pd.Usage = D3D11_USAGE_STAGING;
                    pd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    for (int i = 0; i < 2; ++i)
                        dev11_->CreateTexture2D(&pd, nullptr, &probe_[i]);
                }
                if (probe_[0] && probe_[1])
                {
                    const int prev = probeIdx_ ^ 1;
                    D3D11_MAPPED_SUBRESOURCE m{};
                    if (probeValid_[prev] && SUCCEEDED(ctx11_->Map(probe_[prev].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m)))
                    {
                        unsigned long long h = 1469598103934665603ULL;
                        for (UINT y = 0; y < 32; ++y)
                        {
                            const unsigned char* row = static_cast<const unsigned char*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
                            for (UINT x = 0; x < 256; ++x)
                                h = (h ^ row[x]) * 1099511628211ULL;
                        }
                        ctx11_->Unmap(probe_[prev].Get(), 0);
                        probeValid_[prev] = false;
                        if (haveHash_ && h == lastHash_)
                        {
                            ++contentRep_;
                            repMax_ = (std::max)(repMax_, ++repRun_);
                        }
                        else
                        {
                            ++contentNew_;
                            repRun_ = 0;
                        }
                        lastHash_ = h;
                        haveHash_ = true;
                    }
                    for (UINT gy = 0; gy < 4; ++gy)
                        for (UINT gx = 0; gx < 8; ++gx)
                        {
                            const UINT x = (td.Width - 8) * gx / 7;
                            const UINT y = (td.Height - 8) * gy / 3;
                            D3D11_BOX box{ x, y, 0, x + 8, y + 8, 1 };
                            ctx11_->CopySubresourceRegion(probe_[probeIdx_].Get(), 0, gx * 8, gy * 8, 0, tex.Get(), 0, &box);
                        }
                    probeValid_[probeIdx_] = true;
                    probeIdx_ ^= 1;
                }
            }
        }

        const auto t0 = std::chrono::steady_clock::now();
        if (presLast_.time_since_epoch().count() != 0 && presGaps_.size() < 4096)
            presGaps_.push_back(std::chrono::duration<double, std::milli>(t0 - presLast_).count());
        presLast_ = t0;
        paceLast_ = t0;
        if (opt_.testMode == 1)
        {
            static bool hidden = false;
            if (!hidden)
            {
                ShowWindow(overlay_, SW_HIDE);
                hidden = true;
            }
        }
        else if (!PresentFrame(tex.Get()))
            break;
        ++frames;
        ++liveFrames_;
        ++paceFrames_;
        const double presentMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (presentMs > 250.0 && slowLogged < 20)
        {
            ++slowLogged;
            Log("Slow PresentFrame: %.0f ms", presentMs);
        }
    }

    Log("Shutting down");
    Shutdown();
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // The log goes next to the exe.
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p = path;
    p = p.substr(0, p.find_last_of(L"\\/") + 1) + L"OptiCaptureHost.log";
    _wfopen_s(&g_log, p.c_str(), L"w");

    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        // Defaults come from OptiCaptureHost.ini (written by the launcher); command-line options override them.
        oc::Settings settings;
        oc::LoadSettings(settings);

        Options opt;
        opt.vsync = settings.vsync;
        opt.menuKey = static_cast<int>(settings.menuKey);
        if (!ParseOptions(opt))
        {
            Fail(L"Options:\n  --title <text>\n  --delay <seconds> (default 5)\n  --vsync 0|1 (default 1)\n  --buffers 2..4 (default 3)\n"
                 L"  --gpu <index>\n  --no-clickthrough\n  --menu-key <virtual-key code, default 0x2D = Insert>\n"
                 L"  --hwnd <window handle>  (used by the launcher)\n\nTip: use OptiCaptureLauncher.exe, it does all of this for you.");
            return 2;
        }

        Host host;
        return host.Run(opt, settings);
    }
    catch (const winrt::hresult_error& e)
    {
        Log("Exception: 0x%08X %s", static_cast<unsigned>(e.code().value), Narrow(std::wstring(e.message().c_str())).c_str());
        Fail(L"Unexpected error. See OptiCaptureHost.log next to the exe.");
        return 1;
    }
    catch (const std::exception& e)
    {
        Log("Exception: %s", e.what());
        Fail(L"Unexpected error. See OptiCaptureHost.log next to the exe.");
        return 1;
    }
}
