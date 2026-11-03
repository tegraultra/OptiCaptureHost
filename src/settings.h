// settings.h - the settings file shared by the launcher and the capture host.
//
// The launcher writes OptiCaptureHost.ini (next to the exes) every time something changes; the host re-reads it a
// few times a second while it runs, so VSync and the shortcuts change live.
//
// by MK-JH - https://t.me/MKJH_APPS
#pragma once

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace oc
{

struct Hotkey
{
    UINT mods = 0; // MOD_CONTROL | MOD_SHIFT | MOD_ALT | MOD_WIN
    UINT vk = 0;

    bool operator==(const Hotkey& o) const { return mods == o.mods && vk == o.vk; }
    bool operator!=(const Hotkey& o) const { return !(*this == o); }
};

struct Settings
{
    bool vsync = true;
    int frameGenMode = 0; // 0=XeFG, 1=DLSSG, 2=nofg, 3=fsrfg, 5=Combo MFG, 6=Nukem FG, 7=FSR3-4 FG, 8=Nvidia MFG (folders next to the program), 4=Intel FG profile (OptiScaler.ini from the "intel FG" folder)
    int upscalerMode = 0; // 0=xess, 1=ffx, 2=dlss, 3=dlss 3.7 (AMD / GTX 16), 4=fsr 4.1.2, 5=DittoSR
    UINT menuKey = VK_INSERT; // OptiScaler menu key (OptiScaler.ini [Menu] ShortcutKey)
    UINT fpsKey = VK_PRIOR;      // FPS overlay on/off (FpsShortcutKey)
    UINT fpsCycleKey = VK_NEXT;  // FPS overlay type (FpsCycleShortcutKey)
    UINT fgKey = VK_END;         // frame generation on/off (FGShortcutKey)
    int presentMode = 3;      // 0 auto (FG on: no wait, FG off: VSync), 1 always interval 0, 2 always interval 1, 3 classic 2.2.0 behaviour, 4 Lossless-style (tearing, latency 1), 5 classic + exactly refresh/2 base frames, 6 classic + unlimited capture (no half-refresh limit)
    int dlssPreset = 0;       // DLSS model (preset): 0 auto (L when upscaling, E at 1:1), 5 E, 10 J, 11 K, 12 L, 13 M; [Upscale] DlssPreset
    int upscale = 0;          // output scaling: 0 off, 1 bilinear, 2 FSR1 (EASU+RCAS), 3 NIS, 4 FSR3/4 (motion measured from the frames + AMD runtime); read at start
    int upscaleLevel = 0;     // input size: 0 Quality 67%, 1 Balanced 58%, 2 Performance 50%, 3 Native 100% (FSR3/4 only: anti-aliasing, no resize)
    int labelSeconds = 8;     // output scaling: show "720 to 1080" in the corner for this many seconds (0 never, 999 always)
    int nisSharp = 50;        // NIS sharpness x100 (0..100)
    int upscaleSharp = 25;    // FSR1 RCAS sharpness x100 (0 = sharpest, 200 = none), default 0.25
    int mfgCount = 0;         // multi frame generation: 0 auto, 1..5 = 2x..6x (InterpolationCount in OptiScaler.ini)
    int presentDelay = 3;     // ms the host holds each frame before Present, 0..12
    int limitTarget = 0;      // RTSS limiter: 0 off, 1 game, 2 host, 3 global
    int limitMode = 1;        // RTSS limiter mode: 0 async, 1 front edge sync, 2 back edge sync
    int profileAuto = 0;      // apply a saved game profile automatically when its game is picked
    int hideOsd = 1;          // turn the RTSS on-screen display of the game off while the host runs
    int optiFps = 1;          // write OptiScaler.ini [Framerate] FramerateLimit = the monitor's refresh rate at every START
    int limitAuto = 0;        // apply the RTSS limiter every time START is pressed
    int limitFps = 0;         // 0 refresh rate, -2 half refresh rate, else fixed fps
    int boost = 3;            // performance mode, read by the host at start-up: 0 off, 1 CPU, 2 CPU + GPU high, 3 max

    Hotkey stop{ MOD_CONTROL | MOD_SHIFT, VK_F12 };     // close the capture host
    Hotkey focus{ MOD_CONTROL | MOD_SHIFT, VK_F11 };    // game <-> overlay (mouse and keyboard focus)
    Hotkey menu{ MOD_CONTROL | MOD_SHIFT, VK_F10 };     // open / close OptiScaler's menu
    Hotkey launcher{ MOD_CONTROL | MOD_SHIFT, VK_F9 }; // bring the launcher window on top (the launcher registers it)
};

inline std::wstring ExeDir()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s = path;
    const size_t cut = s.find_last_of(L"\\/");
    return cut == std::wstring::npos ? std::wstring() : s.substr(0, cut + 1);
}

inline std::wstring SettingsPath()
{
    return ExeDir() + L"OptiCaptureHost.ini";
}

// "6,123" = modifiers, virtual-key code.
inline Hotkey ParseHotkey(const std::wstring& text, Hotkey fallback)
{
    wchar_t* end = nullptr;
    const unsigned long mods = wcstoul(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != L',')
        return fallback;

    const unsigned long vk = wcstoul(end + 1, &end, 10);
    if (vk == 0 || vk > 254)
        return fallback;

    Hotkey h;
    h.mods = static_cast<UINT>(mods) & (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN);
    h.vk = static_cast<UINT>(vk);
    return h;
}

inline std::wstring HotkeyToString(const Hotkey& h)
{
    return std::to_wstring(h.mods) + L"," + std::to_wstring(h.vk);
}

inline void LoadSettings(Settings& s)
{
    s = Settings{};
    const std::wstring path = SettingsPath();

    s.vsync = GetPrivateProfileIntW(L"General", L"VSync", 1, path.c_str()) != 0;

    s.frameGenMode = GetPrivateProfileIntW(L"Settings", L"FrameGenMode", 0, path.c_str());
    if (s.frameGenMode < 0 || s.frameGenMode > 10 || s.frameGenMode == 3) s.frameGenMode = 0; // 3 (FSR Frame Generation) was removed; 9 = "FSR FG AL2" folder, 10 = "FSR FG LF" folder

    s.mfgCount = GetPrivateProfileIntW(L"Settings", L"MfgCount", 0, path.c_str());
    if (s.mfgCount < 0 || s.mfgCount > 5) s.mfgCount = 0;

    s.upscalerMode = GetPrivateProfileIntW(L"Settings", L"UpscalerMode", 0, path.c_str());
    if (s.upscalerMode < 0 || s.upscalerMode > 5) s.upscalerMode = 0;

    s.presentMode = GetPrivateProfileIntW(L"Advanced", L"PresentMode", 3, path.c_str());
    if (s.presentMode < 0 || s.presentMode > 6) s.presentMode = 3;
    s.upscale = GetPrivateProfileIntW(L"Upscale", L"Method", 0, path.c_str());
    if (s.upscale < 0 || s.upscale > 5) s.upscale = 0;
    s.dlssPreset = GetPrivateProfileIntW(L"Upscale", L"DlssPreset", 0, path.c_str());
    if (s.dlssPreset < 0 || s.dlssPreset > 16) s.dlssPreset = 0;
    s.upscaleLevel = GetPrivateProfileIntW(L"Upscale", L"Level", 0, path.c_str());
    if (s.upscaleLevel < 0 || s.upscaleLevel > 3) s.upscaleLevel = 0;
    s.nisSharp = GetPrivateProfileIntW(L"Upscale", L"NisSharpness", 50, path.c_str());
    if (s.nisSharp < 0 || s.nisSharp > 100) s.nisSharp = 50;
    s.labelSeconds = GetPrivateProfileIntW(L"Upscale", L"LabelSeconds", 8, path.c_str());
    if (s.labelSeconds < 0 || s.labelSeconds > 999) s.labelSeconds = 8;
    s.upscaleSharp = GetPrivateProfileIntW(L"Upscale", L"Sharpness", 25, path.c_str());
    if (s.upscaleSharp < 0 || s.upscaleSharp > 200) s.upscaleSharp = 25;
    s.presentDelay = GetPrivateProfileIntW(L"Advanced", L"PresentDelayMs", 3, path.c_str());
    if (s.presentDelay < 0 || s.presentDelay > 12) s.presentDelay = 3;
    s.limitTarget = GetPrivateProfileIntW(L"Limiter", L"Target", 0, path.c_str());
    if (s.limitTarget < 0 || s.limitTarget > 3) s.limitTarget = 0;
    s.limitFps = GetPrivateProfileIntW(L"Limiter", L"Fps", 0, path.c_str());
    s.profileAuto = GetPrivateProfileIntW(L"Profiles", L"Auto", 0, path.c_str()) != 0 ? 1 : 0;
    s.hideOsd = GetPrivateProfileIntW(L"Limiter", L"HideOsd", 1, path.c_str()) != 0 ? 1 : 0;
    s.optiFps = GetPrivateProfileIntW(L"Limiter", L"OptiFps", 1, path.c_str()) != 0 ? 1 : 0;
    s.limitAuto = GetPrivateProfileIntW(L"Limiter", L"Auto", 0, path.c_str()) != 0 ? 1 : 0;
    s.limitMode = GetPrivateProfileIntW(L"Limiter", L"Mode", 1, path.c_str());
    if (s.limitMode < 0 || s.limitMode > 2) s.limitMode = 1;

    s.boost = GetPrivateProfileIntW(L"Advanced", L"Boost", 3, path.c_str());
    if (s.boost < 0 || s.boost > 3) s.boost = 3;

    const UINT menuKey = GetPrivateProfileIntW(L"General", L"MenuKey", VK_INSERT, path.c_str());
    if (menuKey >= 1 && menuKey <= 254)
        s.menuKey = menuKey;

    auto readKey = [&](const wchar_t* key, UINT def) -> UINT
    {
        const UINT v = GetPrivateProfileIntW(L"General", key, def, path.c_str());
        return (v >= 1 && v <= 254) ? v : def;
    };
    s.fpsKey = readKey(L"FpsKey", s.fpsKey);
    s.fpsCycleKey = readKey(L"FpsCycleKey", s.fpsCycleKey);
    s.fgKey = readKey(L"FgKey", s.fgKey);

    auto read = [&](const wchar_t* key, const Hotkey& def) -> Hotkey
    {
        wchar_t buf[64] = {};
        GetPrivateProfileStringW(L"Shortcuts", key, L"", buf, 64, path.c_str());
        return buf[0] ? ParseHotkey(buf, def) : def;
    };

    s.stop = read(L"Stop", s.stop);
    s.focus = read(L"Focus", s.focus);
    s.menu = read(L"Menu", s.menu);
    s.launcher = read(L"Launcher", s.launcher);
}

inline void SaveSettings(const Settings& s)
{
    const std::wstring path = SettingsPath();

    WritePrivateProfileStringW(L"General", L"VSync", s.vsync ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"Settings", L"FrameGenMode", std::to_wstring(s.frameGenMode).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Settings", L"MfgCount", std::to_wstring(s.mfgCount).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Settings", L"UpscalerMode", std::to_wstring(s.upscalerMode).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Advanced", L"PresentMode", std::to_wstring(s.presentMode).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Upscale", L"Method", std::to_wstring(s.upscale).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Upscale", L"Level", std::to_wstring(s.upscaleLevel).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Upscale", L"DlssPreset", std::to_wstring(s.dlssPreset).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Upscale", L"NisSharpness", std::to_wstring(s.nisSharp).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Upscale", L"LabelSeconds", std::to_wstring(s.labelSeconds).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Upscale", L"Sharpness", std::to_wstring(s.upscaleSharp).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Advanced", L"PresentDelayMs", std::to_wstring(s.presentDelay).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Limiter", L"Target", std::to_wstring(s.limitTarget).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Profiles", L"Auto", std::to_wstring(s.profileAuto).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Limiter", L"HideOsd", std::to_wstring(s.hideOsd).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Limiter", L"OptiFps", std::to_wstring(s.optiFps).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Limiter", L"Auto", std::to_wstring(s.limitAuto).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Limiter", L"Mode", std::to_wstring(s.limitMode).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Limiter", L"Fps", std::to_wstring(s.limitFps).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Advanced", L"Boost", std::to_wstring(s.boost).c_str(), path.c_str());
    WritePrivateProfileStringW(L"General", L"MenuKey", std::to_wstring(s.menuKey).c_str(), path.c_str());
    WritePrivateProfileStringW(L"General", L"FpsKey", std::to_wstring(s.fpsKey).c_str(), path.c_str());
    WritePrivateProfileStringW(L"General", L"FpsCycleKey", std::to_wstring(s.fpsCycleKey).c_str(), path.c_str());
    WritePrivateProfileStringW(L"General", L"FgKey", std::to_wstring(s.fgKey).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Shortcuts", L"Stop", HotkeyToString(s.stop).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Shortcuts", L"Focus", HotkeyToString(s.focus).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Shortcuts", L"Menu", HotkeyToString(s.menu).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Shortcuts", L"Launcher", HotkeyToString(s.launcher).c_str(), path.c_str());
}

// ---------------------------------------------------------------------------------------------------------------
// Live status: the host writes OptiCaptureHost.status about once a second, the launcher reads it and shows it.
// ---------------------------------------------------------------------------------------------------------------

struct LiveStats
{
    unsigned long long stamp = 0; // GetTickCount64() when written (the launcher ignores old files)
    unsigned pid = 0;
    double refreshHz = 0.0;  // refresh rate of the primary display
    double captureFps = 0.0; // frames Windows Graphics Capture delivered per second (what the game draws, up to the refresh rate)
    double presentFps = 0.0; // frames the host presented per second
    double dxgiFps = 0.0;    // presents DXGI counted per second (includes frames generated inside Present)
    double presentMs = 0.0;  // average time inside Present
    double waitMs = 0.0;     // average time waiting for the back buffer
    int interval = 0;        // sync interval the host used
    int focus = 2;           // 0 game, 1 overlay, 2 other
    int capHint = 0;         // 1: the game's frame rate is very steady and clearly below the refresh rate
    double capFps = 0.0;     // that frame rate
};

inline std::wstring StatusPath()
{
    return ExeDir() + L"OptiCaptureHost.status";
}

inline void WriteLiveStats(const LiveStats& s)
{
    const std::wstring path = StatusPath();
    const std::wstring tmp = path + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f)
        return;
    std::fprintf(f,
                 "stamp=%llu\npid=%u\nrefresh=%.3f\ncapture=%.3f\npresent=%.3f\ndxgi=%.3f\npresentms=%.3f\nwaitms=%.3f\n"
                 "interval=%d\nfocus=%d\ncaphint=%d\ncapfps=%.3f\n",
                 s.stamp, s.pid, s.refreshHz, s.captureFps, s.presentFps, s.dxgiFps, s.presentMs, s.waitMs, s.interval, s.focus,
                 s.capHint, s.capFps);
    std::fclose(f);
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

inline bool ReadLiveStats(LiveStats& s)
{
    s = LiveStats{};
    FILE* f = nullptr;
    if (_wfopen_s(&f, StatusPath().c_str(), L"rb") != 0 || !f)
        return false;

    bool any = false;
    char line[160];
    while (std::fgets(line, sizeof(line), f))
    {
        char* eq = std::strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        const char* k = line;
        const char* v = eq + 1;
        const double d = std::atof(v);
        if (!std::strcmp(k, "stamp")) s.stamp = std::strtoull(v, nullptr, 10);
        else if (!std::strcmp(k, "pid")) s.pid = static_cast<unsigned>(d);
        else if (!std::strcmp(k, "refresh")) s.refreshHz = d;
        else if (!std::strcmp(k, "capture")) s.captureFps = d;
        else if (!std::strcmp(k, "present")) s.presentFps = d;
        else if (!std::strcmp(k, "dxgi")) s.dxgiFps = d;
        else if (!std::strcmp(k, "presentms")) s.presentMs = d;
        else if (!std::strcmp(k, "waitms")) s.waitMs = d;
        else if (!std::strcmp(k, "interval")) s.interval = static_cast<int>(d);
        else if (!std::strcmp(k, "focus")) s.focus = static_cast<int>(d);
        else if (!std::strcmp(k, "caphint")) s.capHint = static_cast<int>(d);
        else if (!std::strcmp(k, "capfps")) s.capFps = d;
        else
            continue;
        any = true;
    }
    std::fclose(f);
    return any && s.stamp != 0;
}

// Last-write time of a file as one number (0 when it does not exist).
inline unsigned long long FileStamp(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d))
        return 0;
    return (static_cast<unsigned long long>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
}

} // namespace oc
