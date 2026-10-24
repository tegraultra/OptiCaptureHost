// OptiCaptureLauncher - the front end of OptiCaptureHost.
//
// Pick the running game, switch VSync, change the shortcuts, press Start. The launcher starts OptiCaptureHost.exe
// (with OptiScaler next to it) for the chosen window and keeps writing OptiCaptureHost.ini, which the host re-reads
// while it runs, so VSync and the shortcuts change live.
//
// It deliberately uses no Direct3D / DXGI at all (plain GDI+): OptiScaler loads as dxgi.dll next to the exes, and the
// launcher must not pull it in.
//
// by MK-JH - https://t.me/MKJH_APPS

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

// gdiplus.h uses unqualified min / max; with NOMINMAX they have to come from here.
using std::max;
using std::min;
#include <gdiplus.h>

#include "settings.h"
#include "version.h"

#pragma comment(lib, "advapi32.lib")

namespace gp = Gdiplus;

namespace
{

constexpr const wchar_t* kClassName = L"OptiCaptureLauncher";
constexpr const wchar_t* kTelegramUrl = L"https://t.me/MKJH_APPS";
constexpr const wchar_t* kTelegramJHUrl = L"https://t.me/PersianPatchDeveloper";
constexpr const wchar_t* kTelegramAppsUrl = L"https://t.me/MKJH_APPS";
constexpr const wchar_t* kTelegramArchiveUrl = L"https://t.me/MKJH_ARCHIVE";
constexpr const wchar_t* kTelegramText = L"t.me/MKJH_APPS";
constexpr const wchar_t* kHostExe = L"OptiCaptureHost.exe";
constexpr const wchar_t* kOverlayClass = L"OptiCaptureHostOverlay"; // the host's overlay window class

constexpr const wchar_t* kVersion = L"" OC_VER_STR;

constexpr float kW = 796.0f; // client size in logical pixels (scaled by the DPI)
constexpr float kH = 770.0f;

constexpr UINT_PTR kTimerList = 1;
constexpr UINT_PTR kTimerHost = 2;
constexpr UINT_PTR kTimerFront = 3;
constexpr int kHotkeyShow = 1;
constexpr int kIdFrameGenSelect = 6;
constexpr int kIdFrameGenApply = 7;
constexpr int kIdUpscalerSelect = 11;
constexpr int kIdUpscalerApply = 12;
constexpr int kIdLinkJH = 8;
constexpr int kIdLinkApps = 9;
constexpr int kIdLinkArchive = 10;
constexpr int kFgMenuBase = 5000;
constexpr int kUpscalerMenuBase = 5100;
constexpr int kIdBoostSelect = 13;
constexpr int kBoostMenuBase = 5200;
constexpr int kIdDlgCheck = 20;  // warning dialog: "don't show again"
constexpr int kIdDlgOk = 21;
constexpr int kIdDlgCancel = 22;
constexpr int kIdWarnReset = 23; // show the hidden warnings again
constexpr int kIdPresentSelect = 14;
constexpr int kPresentMenuBase = 5300;
constexpr int kIdLimitTarget = 15;
constexpr int kIdLimitFps = 16;
constexpr int kIdLimitApply = 17;
constexpr int kIdDelaySelect = 18;
constexpr int kIdLimitMode = 19;
constexpr int kIdLimitAuto = 24;
constexpr int kIdLimitOff = 25;
constexpr int kIdBorderless = 26;
constexpr int kIdPictureOn = 27;
constexpr int kIdPictureOff = 28;
constexpr int kLimitModeMenuBase = 5800;
constexpr int kLimitTargetMenuBase = 5500;
constexpr int kLimitFpsMenuBase = 5600;
constexpr int kDelayMenuBase = 5700;
constexpr int kVsyncMenuBase = 5400;

// ---------------------------------------------------------------------------------------------------------------
// Look
// ---------------------------------------------------------------------------------------------------------------

namespace col
{
inline gp::Color Bg() { return gp::Color(255, 13, 15, 20); }
inline gp::Color Card() { return gp::Color(255, 21, 24, 32); }
inline gp::Color Row() { return gp::Color(255, 27, 31, 42); }
inline gp::Color RowHot() { return gp::Color(255, 34, 39, 53); }
inline gp::Color RowSel() { return gp::Color(255, 38, 45, 66); }
inline gp::Color Line() { return gp::Color(255, 40, 45, 58); }
inline gp::Color Text() { return gp::Color(255, 233, 236, 243); }
inline gp::Color Muted() { return gp::Color(255, 135, 143, 163); }
inline gp::Color Accent() { return gp::Color(255, 108, 140, 255); }
inline gp::Color Accent2() { return gp::Color(255, 56, 214, 220); }
inline gp::Color Green() { return gp::Color(255, 61, 220, 151); }
inline gp::Color Red() { return gp::Color(255, 255, 92, 112); }
inline gp::Color Amber() { return gp::Color(255, 255, 191, 71); }
inline gp::Color White() { return gp::Color(255, 255, 255, 255); }
inline gp::Color Alpha(const gp::Color& c, BYTE a) { return gp::Color(a, c.GetR(), c.GetG(), c.GetB()); }
} // namespace col

void RoundPath(gp::GraphicsPath& p, const gp::RectF& r, float rad)
{
    rad = min(rad, min(r.Width, r.Height) / 2.0f);
    const float d = rad * 2.0f;
    p.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    p.CloseFigure();
}

void FillRound(gp::Graphics& g, const gp::RectF& r, float rad, const gp::Color& c)
{
    gp::GraphicsPath p;
    RoundPath(p, r, rad);
    gp::SolidBrush b(c);
    g.FillPath(&b, &p);
}

void StrokeRound(gp::Graphics& g, const gp::RectF& r, float rad, const gp::Color& c, float w)
{
    const gp::RectF in(r.X + w / 2.0f, r.Y + w / 2.0f, r.Width - w, r.Height - w);
    gp::GraphicsPath p;
    RoundPath(p, in, max(0.0f, rad - w / 2.0f));
    gp::Pen pen(c, w);
    g.DrawPath(&pen, &p);
}

void GradRound(gp::Graphics& g, const gp::RectF& r, float rad, const gp::Color& a, const gp::Color& b)
{
    gp::GraphicsPath p;
    RoundPath(p, r, rad);
    gp::LinearGradientBrush br(r, a, b, gp::LinearGradientModeForwardDiagonal);
    g.FillPath(&br, &p);
}

// The app's mark: a gradient tile with four capture-frame corners and a dot.
void DrawLogo(gp::Graphics& g, const gp::RectF& r)
{
    GradRound(g, r, r.Width * 0.28f, col::Accent(), col::Accent2());

    const float m = r.Width * 0.25f;
    const float l = r.Width * 0.17f;
    const float x0 = r.X + m;
    const float y0 = r.Y + m;
    const float x1 = r.X + r.Width - m;
    const float y1 = r.Y + r.Height - m;

    gp::Pen pen(col::White(), max(1.5f, r.Width * 0.065f));
    pen.SetStartCap(gp::LineCapRound);
    pen.SetEndCap(gp::LineCapRound);
    pen.SetLineJoin(gp::LineJoinRound);

    gp::PointF a[3];
    a[0] = gp::PointF(x0, y0 + l);
    a[1] = gp::PointF(x0, y0);
    a[2] = gp::PointF(x0 + l, y0);
    g.DrawLines(&pen, a, 3);

    a[0] = gp::PointF(x1 - l, y0);
    a[1] = gp::PointF(x1, y0);
    a[2] = gp::PointF(x1, y0 + l);
    g.DrawLines(&pen, a, 3);

    a[0] = gp::PointF(x1, y1 - l);
    a[1] = gp::PointF(x1, y1);
    a[2] = gp::PointF(x1 - l, y1);
    g.DrawLines(&pen, a, 3);

    a[0] = gp::PointF(x0 + l, y1);
    a[1] = gp::PointF(x0, y1);
    a[2] = gp::PointF(x0, y1 - l);
    g.DrawLines(&pen, a, 3);

    gp::SolidBrush w(col::White());
    const float d = r.Width * 0.16f;
    g.FillEllipse(&w, r.X + (r.Width - d) / 2.0f, r.Y + (r.Height - d) / 2.0f, d, d);
}

// A small paper plane in a round badge (the link to the Telegram channel).
void DrawPlane(gp::Graphics& g, const gp::RectF& r)
{
    GradRound(g, r, r.Width / 2.0f, col::Accent(), col::Accent2());

    auto P = [&](float x, float y) { return gp::PointF(r.X + r.Width * x, r.Y + r.Height * y); };
    gp::PointF pts[4] = { P(0.20f, 0.50f), P(0.80f, 0.24f), P(0.62f, 0.78f), P(0.47f, 0.60f) };
    gp::SolidBrush w(col::White());
    g.FillPolygon(&w, pts, 4);

    gp::Pen fold(gp::Color(255, 60, 120, 200), max(1.0f, r.Width * 0.05f));
    g.DrawLine(&fold, P(0.47f, 0.60f), P(0.80f, 0.24f));
}

// ---------------------------------------------------------------------------------------------------------------
// Key names
// ---------------------------------------------------------------------------------------------------------------

std::wstring KeyName(UINT vk)
{
    if (vk >= VK_F1 && vk <= VK_F24)
        return L"F" + std::to_wstring(vk - VK_F1 + 1);
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
        return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return L"Num " + std::to_wstring(vk - VK_NUMPAD0);

    switch (vk)
    {
    case VK_INSERT: return L"Insert";
    case VK_DELETE: return L"Delete";
    case VK_HOME: return L"Home";
    case VK_END: return L"End";
    case VK_PRIOR: return L"Page Up";
    case VK_NEXT: return L"Page Down";
    case VK_LEFT: return L"Left";
    case VK_RIGHT: return L"Right";
    case VK_UP: return L"Up";
    case VK_DOWN: return L"Down";
    case VK_PAUSE: return L"Pause";
    case VK_SNAPSHOT: return L"Print Screen";
    case VK_SCROLL: return L"Scroll Lock";
    case VK_BACK: return L"Backspace";
    case VK_TAB: return L"Tab";
    case VK_RETURN: return L"Enter";
    case VK_SPACE: return L"Space";
    case VK_MULTIPLY: return L"Num *";
    case VK_ADD: return L"Num +";
    case VK_SUBTRACT: return L"Num -";
    case VK_DIVIDE: return L"Num /";
    case VK_DECIMAL: return L"Num .";
    default: break;
    }

    LONG lp = static_cast<LONG>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16);
    wchar_t buf[64] = {};
    if (GetKeyNameTextW(lp, buf, 64) > 0)
        return buf;
    return L"Key " + std::to_wstring(vk);
}

std::wstring HotkeyText(const oc::Hotkey& h)
{
    std::wstring s;
    if (h.mods & MOD_CONTROL)
        s += L"Ctrl + ";
    if (h.mods & MOD_ALT)
        s += L"Alt + ";
    if (h.mods & MOD_SHIFT)
        s += L"Shift + ";
    if (h.mods & MOD_WIN)
        s += L"Win + ";
    return s + KeyName(h.vk);
}

bool IsModifierVk(UINT vk)
{
    return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL ||
           vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN;
}

// ---------------------------------------------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------------------------------------------

struct GameItem
{
    HWND hwnd = nullptr;
    std::wstring title;
    std::wstring exe;
    int w = 0;
    int h = 0;
    bool minimized = false;
    bool fills = false; // covers a whole monitor: borderless or fullscreen-sized, the usual look of a game
    HICON icon = nullptr; // owned by the window, never destroyed here
};

struct Hit
{
    gp::RectF r;
    int id = 0;
};

enum class Note
{
    Info,
    Ok,
    Error
};

// Ids of the clickable things.
constexpr int kIdNone = 0;
constexpr int kIdRefresh = 1;
constexpr int kIdVsync = 2;
constexpr int kIdStart = 3;
constexpr int kIdReset = 4;
constexpr int kIdLink = 5;
constexpr int kIdShortcut0 = 100; // + shortcut number
constexpr int kIdRow0 = 1000;     // + row number

enum Shortcut
{
    kScMenuKey,
    kScFpsKey,
    kScFpsCycleKey,
    kScFgKey,
    kScStop,     // Ctrl/Shift/Alt + key (handled by the host)
    kScLauncher, // Ctrl/Shift/Alt + key (handled by this window)
    kScCount
};

struct ShortcutDef
{
    const wchar_t* label;
    const wchar_t* hint;
};

const ShortcutDef kShortcutDefs[kScCount] = {
    { L"OptiScaler menu", L"Open / close the menu" },
    { L"FPS overlay", L"Show / hide it" },
    { L"Overlay type", L"Change what it shows" },
    { L"Frame generation", L"Turn it on / off" },
    { L"Stop capture", L"Close the capture host" },
    { L"This window", L"Bring the launcher on top" },
};

struct EnumCtx
{
    std::vector<GameItem>* out;
    const std::vector<GameItem>* old;
    DWORD selfPid;
    DWORD hostPid;
};

// OptiScaler reads its own hotkeys from OptiScaler.ini ([Menu]); keep that file in step with the launcher.
void WriteOptiScalerKeys(const oc::Settings& s)
{
    const std::wstring ini = oc::ExeDir() + L"OptiScaler.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;

    auto put = [&](const wchar_t* key, UINT vk)
    {
        wchar_t buf[16] = {};
        swprintf_s(buf, L"0x%02X", vk);
        WritePrivateProfileStringW(L"Menu", key, buf, ini.c_str());
    };
    put(L"ShortcutKey", s.menuKey);
    put(L"FpsShortcutKey", s.fpsKey);
    put(L"FpsCycleShortcutKey", s.fpsCycleKey);
    put(L"FGShortcutKey", s.fgKey);
}


// ---- OptiScaler.ini: ForceVsync (auto / true / false) ----
// The key is edited in place (whatever section it lives in); the rest of the file is left byte for byte as it was.
bool IsIniKeyLine(const std::string& line, const char* key, size_t& valueStart, size_t& valueEnd)
{
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= line.size() || line[i] == ';' || line[i] == '#') return false;
    const size_t kl = strlen(key);
    if (line.size() < i + kl || _strnicmp(line.c_str() + i, key, kl) != 0) return false;
    size_t j = i + kl;
    while (j < line.size() && (line[j] == ' ' || line[j] == '\t')) ++j;
    if (j >= line.size() || line[j] != '=') return false;
    ++j;
    valueStart = j;
    valueEnd = line.size();
    while (valueEnd > valueStart && (line[valueEnd - 1] == '\r' || line[valueEnd - 1] == '\n')) --valueEnd;
    return true;
}

bool ReadFileBytes(const std::wstring& path, std::string& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    out.assign(static_cast<size_t>(sz.QuadPart), '\0');
    DWORD got = 0;
    const bool ok = out.empty() || ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
    CloseHandle(h);
    return ok;
}

// Returns 1 for true, 0 for false, -1 for auto / missing.
int ReadForceVsync()
{
    std::string data;
    if (!ReadFileBytes(oc::ExeDir() + L"OptiScaler.ini", data)) return -1;
    size_t pos = 0;
    while (pos < data.size())
    {
        size_t eol = data.find('\n', pos);
        if (eol == std::string::npos) eol = data.size();
        const std::string line = data.substr(pos, eol - pos);
        size_t vs = 0, ve = 0;
        if (IsIniKeyLine(line, "ForceVsync", vs, ve))
        {
            std::string v = line.substr(vs, ve - vs);
            const size_t semi = v.find(';');
            if (semi != std::string::npos) v.resize(semi);
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
            size_t a = 0;
            while (a < v.size() && (v[a] == ' ' || v[a] == '\t')) ++a;
            v = v.substr(a);
            if (_stricmp(v.c_str(), "true") == 0) return 1;
            if (_stricmp(v.c_str(), "false") == 0) return 0;
            return -1;
        }
        pos = eol + 1;
    }
    return -1;
}

// Sets "key = value" in OptiScaler.ini in place (the file is otherwise left as it was). A missing key is added to the
// given section through the INI API.
bool SetIniValue(const char* key, const char* value, const wchar_t* section)
{
    const std::wstring ini = oc::ExeDir() + L"OptiScaler.ini";
    std::string data;
    if (!ReadFileBytes(ini, data)) return false;

    std::string out;
    bool done = false;
    size_t pos = 0;
    while (pos < data.size())
    {
        size_t eol = data.find('\n', pos);
        const bool hasEol = eol != std::string::npos;
        if (!hasEol) eol = data.size();
        std::string line = data.substr(pos, eol - pos);
        size_t vs = 0, ve = 0;
        if (!done && IsIniKeyLine(line, key, vs, ve))
        {
            std::string tail;
            const size_t semi = line.find(';', vs);
            if (semi != std::string::npos) tail = " " + line.substr(semi);
            else if (line.size() > ve) tail = line.substr(ve);
            line = line.substr(0, vs) + " " + value + tail;
            done = true;
        }
        out += line;
        if (hasEol) out += '\n';
        pos = eol + 1;
    }
    if (!done)
    {
        const std::wstring wk(key, key + strlen(key));
        const std::wstring wv(value, value + strlen(value));
        return WritePrivateProfileStringW(section, wk.c_str(), wv.c_str(), ini.c_str()) != FALSE;
    }

    HANDLE h = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &wrote, nullptr) && wrote == out.size();
    CloseHandle(h);
    return ok;
}

// OptiScaler only looks at ForceVsync when [V-Sync] OverrideVsync is on, so both are written together.
bool WriteForceVsync(int mode) // -1 auto, 0 off (ForceVsync=false), 1 on / SyncInterval 1, 2 on / SyncInterval 0
{
    const char* force = mode < 0 ? "auto" : (mode == 0 ? "false" : "true");
    const char* sync = mode < 0 ? "auto" : ((mode == 1) ? "1" : "0");
    bool ok = SetIniValue("OverrideVsync", mode < 0 ? "auto" : "true", L"V-Sync");
    ok = SetIniValue("ForceVsync", force, L"V-Sync") && ok;
    ok = SetIniValue("SyncInterval", sync, L"V-Sync") && ok;
    return ok;
}

HICON IconOf(HWND h)
{
    DWORD_PTR res = 0;
    if (SendMessageTimeoutW(h, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 40, &res) && res)
        return reinterpret_cast<HICON>(res);
    if (SendMessageTimeoutW(h, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 40, &res) && res)
        return reinterpret_cast<HICON>(res);

    HICON ic = reinterpret_cast<HICON>(GetClassLongPtrW(h, GCLP_HICON));
    if (!ic)
        ic = reinterpret_cast<HICON>(GetClassLongPtrW(h, GCLP_HICONSM));
    return ic;
}

BOOL CALLBACK EnumWindowsProc(HWND h, LPARAM lp)
{
    auto* c = reinterpret_cast<EnumCtx*>(lp);

    if (!IsWindowVisible(h))
        return TRUE;

    wchar_t title[256] = {};
    if (GetWindowTextW(h, title, 256) <= 0)
        return TRUE;

    const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW)
        return TRUE;
    if (GetWindow(h, GW_OWNER) != nullptr && !(ex & WS_EX_APPWINDOW))
        return TRUE;

    DWORD cloaked = 0; // DWMWA_CLOAKED: windows of suspended store apps and other virtual desktops
    if (SUCCEEDED(DwmGetWindowAttribute(h, 14, &cloaked, sizeof(cloaked))) && cloaked != 0)
        return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == c->selfPid || (c->hostPid != 0 && pid == c->hostPid))
        return TRUE;

    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") ||
        !wcscmp(cls, kOverlayClass))
        return TRUE;

    GameItem it;
    it.hwnd = h;
    it.title = title;
    it.minimized = IsIconic(h) != FALSE;

    RECT r{};
    if (it.minimized)
    {
        WINDOWPLACEMENT wp{};
        wp.length = sizeof(wp);
        GetWindowPlacement(h, &wp);
        r = wp.rcNormalPosition;
    }
    else
    {
        GetWindowRect(h, &r);
    }
    it.w = r.right - r.left;
    it.h = r.bottom - r.top;
    if (it.w < 200 || it.h < 120)
        return TRUE;

    if (!it.minimized)
    {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
        {
            const RECT& m = mi.rcMonitor;
            it.fills = r.left <= m.left + 2 && r.top <= m.top + 2 && r.right >= m.right - 2 && r.bottom >= m.bottom - 2;
        }
    }

    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (p)
    {
        wchar_t path[MAX_PATH] = {};
        DWORD len = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, path, &len))
        {
            std::wstring s = path;
            const size_t cut = s.find_last_of(L"\\/");
            it.exe = cut == std::wstring::npos ? s : s.substr(cut + 1);
        }
        CloseHandle(p);
    }

    // Reuse the icon found last time (asking a window for it can take a moment).
    if (c->old)
    {
        for (const auto& o : *c->old)
        {
            if (o.hwnd == h)
            {
                it.icon = o.icon;
                break;
            }
        }
    }
    if (!it.icon)
        it.icon = IconOf(h);

    c->out->push_back(std::move(it));
    return TRUE;
}

BOOL CALLBACK CloseOverlayProc(HWND h, LPARAM lp)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != static_cast<DWORD>(lp))
        return TRUE;

    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, kOverlayClass))
        PostMessageW(h, WM_CLOSE, 0, 0);
    return TRUE;
}

// Windows only lets the foreground process give the foreground away; attaching to its input thread gets around that
// for a window the user just asked for.
void ForceForeground(HWND h)
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

    if (attached)
        AttachThreadInput(me, fgThread, FALSE);
}

HICON MakeAppIcon(int px)
{
    gp::Bitmap bmp(px, px, PixelFormat32bppARGB);
    gp::Graphics g(&bmp);
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    DrawLogo(g, gp::RectF(1.0f, 1.0f, static_cast<float>(px) - 2.0f, static_cast<float>(px) - 2.0f));

    HICON ic = nullptr;
    bmp.GetHICON(&ic);
    return ic;
}

// ---------------------------------------------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------------------------------------------

// ---------------------------------------------------------------------------------------------------------------
// Warnings (things outside this program that limit the frame rate)
// ---------------------------------------------------------------------------------------------------------------

enum Warn
{
    kWarnRtssMissing, // the frame limiter needs RivaTuner Statistics Server (RTSS) to be installed
    kWarnCount
};

const wchar_t* const kWarnKeys[kWarnCount] = { L"HideRtssMissing" }; // OptiCaptureHost.ini [Warnings]
const wchar_t* const kWarnTitles[kWarnCount] = { L"RivaTuner (RTSS) is not installed" };

// Folder of the running RivaTuner Statistics Server, with a trailing backslash (empty when it is not running).
std::wstring RtssDir()
{
    std::wstring dir;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return dir;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, L"RTSS.exe") != 0)
                continue;
            if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID))
            {
                wchar_t path[MAX_PATH * 2] = {};
                DWORD n = MAX_PATH * 2;
                if (QueryFullProcessImageNameW(p, 0, path, &n))
                {
                    dir = path;
                    const size_t cut = dir.find_last_of(L"\\/");
                    dir = cut == std::wstring::npos ? std::wstring() : dir.substr(0, cut + 1);
                }
                CloseHandle(p);
            }
            break;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return dir;
}

bool FileExists(const std::wstring& f)
{
    return GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Install folder of RTSS (trailing backslash) when RTSSHooks64.dll is there, else empty. Not installed = empty.
std::wstring RtssInstallDir()
{
    std::vector<std::wstring> cands;
    cands.push_back(RtssDir());

    const DWORD flags[] = { RRF_RT_REG_SZ | RRF_SUBKEY_WOW6432KEY, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY };
    for (DWORD f : flags)
    {
        wchar_t buf[MAX_PATH * 2] = {};
        DWORD cb = sizeof(buf) - sizeof(wchar_t);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Unwinder\\RTSS", L"InstallDir", f, nullptr, buf, &cb) == ERROR_SUCCESS)
            cands.push_back(buf);
    }
    for (const wchar_t* var : { L"ProgramFiles(x86)", L"ProgramFiles" })
    {
        wchar_t pf[MAX_PATH] = {};
        if (GetEnvironmentVariableW(var, pf, MAX_PATH) > 0)
            cands.push_back(std::wstring(pf) + L"\\RivaTuner Statistics Server");
    }

    for (std::wstring d : cands)
    {
        if (d.empty())
            continue;
        if (d.back() != L'\\' && d.back() != L'/')
            d += L'\\';
        if (FileExists(d + L"RTSSHooks64.dll"))
            return d;
    }
    return std::wstring();
}

std::string Narrow(const std::wstring& w)
{
    if (w.empty())
        return std::string();
    const int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string r(static_cast<size_t>(n > 0 ? n : 1), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, r.data(), n, nullptr, nullptr);
    r.resize(strlen(r.c_str()));
    return r;
}

// Small text log next to the program (rtss_log.txt) so a limiter that does nothing can be diagnosed.
void RtssLog(const std::wstring& line)
{
    const std::wstring path = oc::ExeDir() + L"rtss_log.txt";
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return;
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t head[40];
    swprintf(head, 40, L"%02d:%02d:%02d  ", t.wHour, t.wMinute, t.wSecond);
    const std::wstring w = std::wstring(head) + line + L"\r\n";
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string u(static_cast<size_t>(n > 0 ? n : 1), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, u.data(), n, nullptr, nullptr);
    DWORD wr = 0;
    WriteFile(f, u.c_str(), static_cast<DWORD>(strlen(u.c_str())), &wr, nullptr);
    CloseHandle(f);
}

// The profile functions exported by RTSSHooks64.dll (the same ones RTSS and MSI Afterburner use for their settings).
struct RtssApi
{
    typedef void (*LoadFn)(LPCSTR);
    typedef void (*SaveFn)(LPCSTR);
    typedef BOOL (*GetFn)(LPCSTR, LPBYTE, DWORD);
    typedef BOOL (*SetFn)(LPCSTR, LPBYTE, DWORD);
    typedef void (*UpdateFn)();

    HMODULE mod = nullptr;
    LoadFn load = nullptr;
    SaveFn save = nullptr;
    GetFn get = nullptr;
    SetFn set = nullptr;
    UpdateFn update = nullptr;

    bool Open(const std::wstring& dir)
    {
        if (mod)
            return true;
        mod = LoadLibraryExW((dir + L"RTSSHooks64.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!mod)
            return false;
        load = reinterpret_cast<LoadFn>(GetProcAddress(mod, "LoadProfile"));
        save = reinterpret_cast<SaveFn>(GetProcAddress(mod, "SaveProfile"));
        get = reinterpret_cast<GetFn>(GetProcAddress(mod, "GetProfileProperty"));
        set = reinterpret_cast<SetFn>(GetProcAddress(mod, "SetProfileProperty"));
        update = reinterpret_cast<UpdateFn>(GetProcAddress(mod, "UpdateProfiles"));
        if (!load || !save || !get || !set || !update)
        {
            FreeLibrary(mod);
            mod = nullptr;
            return false;
        }
        return true;
    }

    // value of a DWORD property of a profile (profile "" = Global), or def when it does not exist
    int Read(const std::string& profile, const char* prop, int def)
    {
        load(profile.c_str());
        DWORD v = 0;
        if (!get(prop, reinterpret_cast<LPBYTE>(&v), sizeof(v)))
            return def;
        return static_cast<int>(v);
    }

    // sets limit (fps), denominator and limiter mode (0 = async, 1 = front edge sync) of a profile and checks the result
    bool Write(const std::string& profile, int limit, int denominator, int syncLimiter)
    {
        load(profile.c_str());
        DWORD d = static_cast<DWORD>(denominator), l = static_cast<DWORD>(limit), m = static_cast<DWORD>(syncLimiter);
        const BOOL r1 = set("FramerateLimitDenominator", reinterpret_cast<LPBYTE>(&d), sizeof(d));
        const BOOL r2 = set("FramerateLimit", reinterpret_cast<LPBYTE>(&l), sizeof(l));
        const BOOL r3 = set("SyncLimiter", reinterpret_cast<LPBYTE>(&m), sizeof(m));
        save(profile.c_str());
        update();
        const int back = Read(profile, "FramerateLimit", -1);
        RtssLog(L"write profile \"" + std::wstring(profile.begin(), profile.end()) + L"\": limit " + std::to_wstring(limit) + L" den " +
                std::to_wstring(denominator) + L" mode " + std::to_wstring(syncLimiter) + L" -> set results " + std::to_wstring(r1) +
                std::to_wstring(r2) + std::to_wstring(r3) + L", read back limit " + std::to_wstring(back) + L", mode " +
                std::to_wstring(Read(profile, "SyncLimiter", -1)));
        return back == limit;
    }
};

// Refresh rate of the monitor the window is on (the primary monitor when there is no window).
int RefreshRateOf(HWND wnd)
{
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromWindow(wnd ? wnd : GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        return static_cast<int>(dm.dmDisplayFrequency);
    return 60;
}

// Result of one limiter run.
struct LimiterOut
{
    int code = 0;       // 0 done, 1 RTSS did not take it (needs administrator rights / RTSS not ready), 2 RTSS not installed, 3 bad input
    std::wstring info;  // short text under the Apply button
    std::wstring note;  // status line
};

bool IsElevated()
{
    HANDLE tok = nullptr;
    bool r = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
    {
        TOKEN_ELEVATION te{};
        DWORD n = 0;
        if (GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &n))
            r = te.TokenIsElevated != 0;
        CloseHandle(tok);
    }
    return r;
}

// Base name of this program (RTSS profile name of the launcher itself).
std::string OwnExeName()
{
    wchar_t path[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    std::wstring w = path;
    const size_t cut = w.find_last_of(L"\\/");
    return Narrow(cut == std::wstring::npos ? w : w.substr(cut + 1));
}

// The RTSS frame limiter. Only one profile is changed at a time; its old values are kept in the .ini and put back when
// the limiter is switched off or moved to another target. hz = refresh rate of the game's monitor.
LimiterOut DoLimiter(RtssApi& api, const oc::Settings& st, const std::wstring& gameExe, int hz, bool off)
{
    LimiterOut out;
    const std::wstring ini = oc::SettingsPath();
    const std::wstring dir = RtssInstallDir();
    RtssLog(L"apply: RTSS folder \"" + dir + L"\", running folder \"" + RtssDir() + L"\", elevated " + std::to_wstring(IsElevated()));

    if (dir.empty() || !api.Open(dir))
    {
        RtssLog(L"RTSS missing or RTSSHooks64.dll could not be loaded (error " + std::to_wstring(GetLastError()) + L")");
        if (st.limitTarget != 0)
        {
            out.code = 2;
            out.note = L"RivaTuner (RTSS) is not installed.";
            return out;
        }
        out.info = L"Limiter off.";
        out.note = L"Frame limiter is off.";
        return out;
    }

    // the launcher itself must not get the RTSS on-screen display (it would draw over the launcher)
    {
        const std::string me = OwnExeName();
        api.load(me.c_str());
        DWORD zero = 0;
        api.set("EnableOSD", reinterpret_cast<LPBYTE>(&zero), sizeof(zero));
        api.save(me.c_str());
        api.update();
        RtssLog(L"launcher profile \"" + std::wstring(me.begin(), me.end()) + L"\": overlay off");
    }

    // Off: set the limit of the profile that was limited, and of the profile the current choice points at, to 0.
    if (off || st.limitTarget == 0)
    {
        std::vector<std::string> names;
        if (GetPrivateProfileIntW(L"Rtss", L"Has", 0, ini.c_str()) != 0)
        {
            wchar_t prof[260] = {};
            GetPrivateProfileStringW(L"Rtss", L"Profile", L"", prof, 260, ini.c_str());
            names.push_back(Narrow(prof));
        }
        if (st.limitTarget == 1 && !gameExe.empty())
            names.push_back(Narrow(gameExe));
        else if (st.limitTarget == 2)
            names.push_back(Narrow(kHostExe));
        else if (st.limitTarget == 3)
            names.push_back(std::string());
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());

        bool allOk = true;
        for (const std::string& n : names)
            allOk = api.Write(n, 0, 1, api.Read(n, "SyncLimiter", 0)) && allOk;
        WritePrivateProfileStringW(L"Rtss", L"Has", L"0", ini.c_str());
        out.info = L"Limiter off.";
        if (allOk)
        {
            out.note = L"Frame limiter is off (limit set to 0).";
        }
        else
        {
            out.code = 1;
            out.note = L"RTSS did not take the change.";
        }
        return out;
    }

    // 1) put back whatever an earlier Apply changed
    if (GetPrivateProfileIntW(L"Rtss", L"Has", 0, ini.c_str()) != 0)
    {
        wchar_t prof[260] = {};
        GetPrivateProfileStringW(L"Rtss", L"Profile", L"", prof, 260, ini.c_str());
        const int oLimit = GetPrivateProfileIntW(L"Rtss", L"OrigLimit", 0, ini.c_str());
        const int oDen = GetPrivateProfileIntW(L"Rtss", L"OrigDen", 1, ini.c_str());
        const int oSync = GetPrivateProfileIntW(L"Rtss", L"OrigSync", 0, ini.c_str());
        api.Write(Narrow(prof), oLimit, oDen < 1 ? 1 : oDen, oSync);
        WritePrivateProfileStringW(L"Rtss", L"Has", L"0", ini.c_str());
    }

    if (st.limitTarget == 0)
    {
        out.info = L"Limiter off.";
        out.note = L"Frame limiter is off.";
        return out;
    }

    // 2) which profile
    std::wstring profile; // empty = Global
    if (st.limitTarget == 1)
    {
        if (gameExe.empty())
        {
            out.code = 3;
            out.note = L"Pick a game in the list first (the limiter is set for its .exe).";
            return out;
        }
        profile = gameExe;
    }
    else if (st.limitTarget == 2)
    {
        profile = kHostExe;
    }

    // 3) which number: limit = num / den (half of 75 Hz is exactly 37.5 = 75 / 2)
    int num = st.limitFps, den = 1;
    if (st.limitFps == 0)
        num = hz;
    else if (st.limitFps <= -2 && st.limitFps >= -4)
    {
        // refresh / 2, / 3 or / 4 as an exact fraction (75 Hz / 2 = 75/2 = 37.5, 75 Hz / 3 = 25)
        num = hz;
        den = -st.limitFps;
        int a = num, b = den;
        while (b != 0)
        {
            const int t = a % b;
            a = b;
            b = t;
        }
        if (a > 1)
        {
            num /= a;
            den /= a;
        }
    }
    if (num < 1)
        num = 30;

    const std::string name = Narrow(profile);
    const int oLimit = api.Read(name, "FramerateLimit", 0);
    const int oDen = api.Read(name, "FramerateLimitDenominator", 1);
    const int oSync = api.Read(name, "SyncLimiter", 0);
    WritePrivateProfileStringW(L"Rtss", L"Profile", profile.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Rtss", L"OrigLimit", std::to_wstring(oLimit).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Rtss", L"OrigDen", std::to_wstring(oDen).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Rtss", L"OrigSync", std::to_wstring(oSync).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Rtss", L"Has", L"1", ini.c_str());

    static const wchar_t* const modeNames[] = { L"async", L"front edge sync", L"back edge sync" };
    const int mode = (st.limitMode >= 0 && st.limitMode <= 2) ? st.limitMode : 1;
    const std::wstring who = profile.empty() ? std::wstring(L"Global") : profile;
    if (!api.Write(name, num, den, mode))
    {
        WritePrivateProfileStringW(L"Rtss", L"Has", L"0", ini.c_str());
        out.code = 1;
        out.info = L"RTSS did not accept the change.";
        out.note = L"RTSS did not take the limit.";
        return out;
    }

    std::wstring fpsText;
    {
        wchar_t tb[32];
        swprintf(tb, 32, L"%.2f", static_cast<double>(num) / static_cast<double>(den));
        fpsText = tb;
        while (!fpsText.empty() && fpsText.back() == L'0')
            fpsText.pop_back();
        if (!fpsText.empty() && fpsText.back() == L'.')
            fpsText.pop_back();
    }
    out.info = who + L" - " + fpsText + L" fps - " + modeNames[mode];
    out.note = L"RTSS limit set: " + who + L" at " + fpsText + L" fps (" + modeNames[mode] + L")." +
               (RtssDir().empty() ? L" RTSS is not running - start it before the game." : L"");
    return out;
}

// Runs this program again, elevated, to write the RTSS profile (RTSS often needs administrator rights for that), so the
// launcher itself can stay a normal program (an elevated launcher breaks the shortcuts of the games it starts).
LimiterOut RunLimiterElevated(const std::wstring& gameExe, int hz, bool off)
{
    LimiterOut out;
    wchar_t self[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    const std::wstring args = L"--rtss-apply \"" + (gameExe.empty() ? std::wstring(L"-") : gameExe) + L"\" " + std::to_wstring(hz) + (off ? L" off" : L"");

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = self;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess)
    {
        out.code = 1;
        out.info = L"Administrator permission was not given.";
        out.note = L"RTSS needs administrator rights to take the limit. Press Apply again and allow it.";
        return out;
    }
    WaitForSingleObject(sei.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);

    const std::wstring ini = oc::SettingsPath();
    wchar_t buf[512] = {};
    GetPrivateProfileStringW(L"Rtss", L"LastInfo", L"", buf, 512, ini.c_str());
    out.info = buf;
    GetPrivateProfileStringW(L"Rtss", L"LastNote", L"", buf, 512, ini.c_str());
    out.note = buf;
    out.code = static_cast<int>(code);
    if (out.note.empty())
        out.note = L"RTSS did not take the limit. Is RTSS running?";
    return out;
}

// Copies a folder with everything in it over another one (files are replaced).
void CopyTree(const std::wstring& from, const std::wstring& to, int& copied, int& failed)
{
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((from + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do
    {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..")
            continue;
        const std::wstring src = from + L"\\" + name;
        const std::wstring dst = to + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            CreateDirectoryW(dst.c_str(), nullptr);
            CopyTree(src, dst, copied, failed);
        }
        else if (CopyFileW(src.c_str(), dst.c_str(), FALSE))
            ++copied;
        else
            ++failed;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ---- Force Borderless ----
struct BorderState
{
    HWND wnd = nullptr;
    LONG_PTR style = 0;
    LONG_PTR exStyle = 0;
    RECT rect{};
    bool wasMaximized = false;
};

// Takes the frame off a window and stretches it over its monitor. False when Windows refused the change.
bool BorderApplyOn(HWND w, const BorderState& b)
{
    if (IsZoomed(w) || IsIconic(w))
        ShowWindow(w, SW_RESTORE);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi))
        return false;
    LONG_PTR style = b.style & ~static_cast<LONG_PTR>(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZE | WS_MAXIMIZE | WS_SYSMENU);
    style |= WS_POPUP;
    const LONG_PTR ex = b.exStyle & ~static_cast<LONG_PTR>(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
    SetLastError(0);
    SetWindowLongPtrW(w, GWL_STYLE, style);
    SetWindowLongPtrW(w, GWL_EXSTYLE, ex);
    if (GetWindowLongPtrW(w, GWL_STYLE) != style)
        return false;
    SetWindowPos(w, nullptr, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    return true;
}

// Puts the frame, the size and the position back.
bool BorderApplyOff(HWND w, const BorderState& b)
{
    SetWindowLongPtrW(w, GWL_STYLE, b.style);
    SetWindowLongPtrW(w, GWL_EXSTYLE, b.exStyle);
    if (GetWindowLongPtrW(w, GWL_STYLE) != b.style)
        return false;
    SetWindowPos(w, nullptr, b.rect.left, b.rect.top, b.rect.right - b.rect.left, b.rect.bottom - b.rect.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (b.wasMaximized)
        ShowWindow(w, SW_MAXIMIZE);
    return true;
}

// Same change from an elevated copy of this program (needed when the game runs as administrator and the launcher does not).
bool RunBorderElevated(bool on, const BorderState& b)
{
    wchar_t self[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    const std::wstring args = std::wstring(L"--borderless ") + (on ? L"on " : L"off ") +
                              std::to_wstring(reinterpret_cast<ULONG_PTR>(b.wnd)) + L" " + std::to_wstring(static_cast<long long>(b.style)) +
                              L" " + std::to_wstring(static_cast<long long>(b.exStyle)) + L" " + std::to_wstring(b.rect.left) + L" " +
                              std::to_wstring(b.rect.top) + L" " + std::to_wstring(b.rect.right) + L" " + std::to_wstring(b.rect.bottom) +
                              L" " + (b.wasMaximized ? L"1" : L"0");
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = self;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess)
        return false;
    WaitForSingleObject(sei.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code == 0;
}

class App
{
  public:
    bool Create(HINSTANCE inst);
    int Loop();

  private:
    struct Fonts
    {
        std::unique_ptr<gp::Font> title, h2, body, bodyB, sm, tiny, link, linkN, btn, chip;
    };

    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT OnMessage(UINT m, WPARAM w, LPARAM l);

    float S(float v) const { return v * scale_ * 0.85f; }
    // DPI scale, made smaller when the window would not fit the height of the screen (taskbar excluded)
    float ScaleFor(UINT dpi, HWND wnd) const
    {
        const float base = static_cast<float>(dpi) / 96.0f;
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        HMONITOR mon = MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY);
        if (!GetMonitorInfoW(mon, &mi))
            return base;
        RECT rc{ 0, 0, static_cast<LONG>(kW * base), static_cast<LONG>(kH * base) };
        AdjustWindowRectExForDpi(&rc, WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0, dpi);
        const float nonClient = static_cast<float>((rc.bottom - rc.top) - static_cast<LONG>(kH * base));
        const float availH = static_cast<float>(mi.rcWork.bottom - mi.rcWork.top) * 0.97f - nonClient;
        const float availW = static_cast<float>(mi.rcWork.right - mi.rcWork.left) * 0.97f;
        float fit = min(availH / (kH * base), availW / (kW * base));
        if (fit > 1.0f) fit = 1.0f;
        if (fit < 0.5f) fit = 0.5f;
        return base * fit;
    }
    gp::RectF R(float x, float y, float w, float h) const { return gp::RectF(S(x), S(y), S(w), S(h)); }

    void MakeFonts();
    void ApplyDpi(UINT dpi, const RECT* suggested);
    void Invalidate() { if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }

    void Paint(HDC dc, int w, int h);
    void Text(gp::Graphics& g, const std::wstring& t, const gp::Font& f, const gp::RectF& r, const gp::Color& c,
              int align = 0, bool wrap = false);
    void AddHit(const gp::RectF& r, int id) { hits_.push_back(Hit{ r, id }); }
    int HitAt(int x, int y) const;
    void Card(gp::Graphics& g, const gp::RectF& r);
    void Chip(gp::Graphics& g, const gp::RectF& r, const std::wstring& text, int id, bool capturing);
    void Button(gp::Graphics& g, const gp::RectF& r, const std::wstring& text, bool disabled, bool danger);

    void Refresh();
    void ClampScroll();
    gp::RectF ListView() const { return R(36, 164, 476, 478); }

    void OnClick(int id);
    LRESULT OnCaptureKey(UINT vk);
    void BeginCapture(int sc);
    UINT* KeyRef(int sc);
    oc::Hotkey* HotkeyRef(int sc);
    std::wstring ChipText(int sc);
    bool RegisterShowHotkey();
    void Save();
    void SetNote(const std::wstring& t, Note k = Note::Info);

    bool Running() const { return host_ != nullptr; }
    void Start();
    void Stop(bool wait);
    void PollHost();
    void ShowOnTop();

    // live status + warnings
    void LoadWarnFlags();
    void ApplyLimiter(bool off = false);
    std::vector<BorderState> border_; // windows made borderless by this launcher (to bring the frame back)
    int BorderIndex(HWND w) const
    {
        for (size_t i = 0; i < border_.size(); ++i)
            if (border_[i].wnd == w)
                return static_cast<int>(i);
        return -1;
    }
    void ToggleBorderless();
    bool pictureOn_ = false; // [CAS] Enabled in OptiScaler.ini
    void ReadPictureState()
    {
        wchar_t v[16] = {};
        GetPrivateProfileStringW(L"CAS", L"Enabled", L"false", v, 16, (oc::ExeDir() + L"OptiScaler.ini").c_str());
        pictureOn_ = _wcsicmp(v, L"true") == 0;
    }
    RtssApi rtss_;
    std::wstring limitInfo_; // what the limiter has applied (shown under the Apply button)
    void RaiseWarning(int kind, const std::wstring& text);
    void OpenNextDialog();
    void CloseDialog(bool accepted);
    void CheckLive();
    void PaintLive(gp::Graphics& g, const gp::RectF& view);
    void PaintDialog(gp::Graphics& g, int W, int H);

    HWND hwnd_ = nullptr;
    HINSTANCE inst_ = nullptr;
    float scale_ = 1.0f;
    Fonts f_;

    oc::Settings st_;
    int vsyncMode_ = -1; // the user's VSync choice: -1 auto (default), 2 on / SyncInterval 0, 1 on / SyncInterval 1, 0 off
    // With Arthur frame generation OptiScaler's forced VSync blocks every internal present (heavy frame drops), so
    // there the host paces its frames itself and OptiScaler is left on auto.
    bool ApplyVsyncToOptiScaler()
    {
        // Arthur: both "On" modes leave OptiScaler on auto and let the host pace its frames instead.
        // Classic (2.2.0) mode: VSync is only the host's own setting and OptiScaler is left on auto for every FG.
        const bool classic = st_.presentMode == 3 || st_.presentMode == 5;
        const bool ls = st_.presentMode == 4; // Lossless-style: tearing unless VSync is explicitly On, host-side only
        const bool on = vsyncMode_ == 1 || vsyncMode_ == 2;
        const bool hostOnly = on && (classic || ls || st_.frameGenMode == 1);
        const int eff = (classic || hostOnly) ? -1 : vsyncMode_;
        // Classic = 2.2.0: the host's own VSync toggle, on by default (Auto and both On modes), off only for Off.
        st_.vsync = classic ? vsyncMode_ != 0 : (ls ? on : (vsyncMode_ == 1 || hostOnly)); // host-side VSync (pacing / classic Present)
        Save();
        WritePrivateProfileStringW(L"General", L"VSyncMode", vsyncMode_ < 0 ? L"auto" : (vsyncMode_ == 2 ? L"on0" : (vsyncMode_ == 1 ? L"on1" : L"off")),
                                   oc::SettingsPath().c_str());
        return WriteForceVsync(eff);
    }
    std::vector<GameItem> games_;
    int selected_ = -1;
    float scroll_ = 0.0f;

    std::vector<Hit> hits_;
    int hover_ = kIdNone;
    int pressed_ = kIdNone;
    bool tracking_ = false;
    int capturing_ = -1;

    std::wstring note_;
    Note noteKind_ = Note::Info;

    HANDLE host_ = nullptr;
    DWORD hostPid_ = 0;
    HWND target_ = nullptr;
    std::wstring targetTitle_;
    bool stopping_ = false;
    ULONGLONG stopAt_ = 0;

    oc::LiveStats live_;
    bool liveValid_ = false;
    unsigned long long lastLiveStamp_ = 0;
    int blockCount_ = 0;
    bool capActive_ = false;   // the game currently looks capped
    bool blockActive_ = false; // Present currently waits although nobody asked for VSync
    bool hideWarn_[kWarnCount] = {};  // "don't show again", saved in OptiCaptureHost.ini
    bool shownWarn_[kWarnCount] = {}; // already raised during this capture
    std::wstring warnText_[kWarnCount];
    std::vector<int> warnQueue_;      // waiting for the launcher window to be visible
    int dlgKind_ = -1;                // the warning dialog that is open (-1 none)
    bool dlgDontShow_ = false;
    bool dlgStart_ = false;           // the dialog asks "start anyway?" before a capture starts
    bool startConfirmed_ = false;
};

bool App::Create(HINSTANCE inst)
{
    inst_ = inst;
    oc::LoadSettings(st_);
    {
        // The user's VSync choice lives in OptiCaptureHost.ini ([General] VSyncMode = auto / on / off). Without it
        // (first run of this version) it is taken from OptiScaler's ForceVsync.
        wchar_t vm[16] = {};
        GetPrivateProfileStringW(L"General", L"VSyncMode", L"", vm, 16, oc::SettingsPath().c_str());
        if (_wcsicmp(vm, L"on0") == 0) vsyncMode_ = 2;
        else if (_wcsicmp(vm, L"on1") == 0 || _wcsicmp(vm, L"on") == 0) vsyncMode_ = 1;
        else if (_wcsicmp(vm, L"off") == 0) vsyncMode_ = 0;
        else if (_wcsicmp(vm, L"auto") == 0) vsyncMode_ = -1;
        else vsyncMode_ = ReadForceVsync();
        st_.vsync = (st_.presentMode == 3 || st_.presentMode == 5) ? vsyncMode_ != 0
                    : (st_.presentMode == 4) ? (vsyncMode_ == 1 || vsyncMode_ == 2)
                                             : (vsyncMode_ == 1 || (vsyncMode_ == 2 && st_.frameGenMode == 1));
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc))
        return false;

    const DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const UINT dpi = GetDpiForSystem();
    scale_ = ScaleFor(dpi, nullptr);

    RECT rc{ 0, 0, static_cast<LONG>(kW * scale_), static_cast<LONG>(kH * scale_) };
    AdjustWindowRectExForDpi(&rc, style, FALSE, 0, dpi);
    const int ww = rc.right - rc.left;
    const int wh = rc.bottom - rc.top;

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.left + ((work.right - work.left) - ww) / 2;
    const int y = work.top + ((work.bottom - work.top) - wh) / 2;

    hwnd_ = CreateWindowExW(0, kClassName, L"OptiCaptureHost  -  by MK-JH", style, x, y, ww, wh, nullptr, nullptr, inst,
                            this);
    if (!hwnd_)
        return false;

    const UINT actual = GetDpiForWindow(hwnd_);
    if (actual != 0 && actual != dpi)
        ApplyDpi(actual, nullptr);
    else
        MakeFonts();

    // Dark title bar, matching caption colour, rounded corners (the last two are Windows 11 only and ignored before).
    BOOL dark = TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark))))
        DwmSetWindowAttribute(hwnd_, 19, &dark, sizeof(dark));
    const COLORREF caption = RGB(13, 15, 20);
    DwmSetWindowAttribute(hwnd_, 35, &caption, sizeof(caption));
    const DWORD round = 2;
    DwmSetWindowAttribute(hwnd_, 33, &round, sizeof(round));

    if (HICON bigIcon = reinterpret_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 64, 64, LR_DEFAULTCOLOR)))
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(bigIcon));
    if (HICON smallIcon = reinterpret_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR)))
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));

    LoadWarnFlags();
    ReadPictureState();
    DeleteFileW(oc::StatusPath().c_str()); // a leftover from a crashed host
    Refresh();
    RegisterShowHotkey();
    SetTimer(hwnd_, kTimerList, 3000, nullptr);
    SetTimer(hwnd_, kTimerHost, 400, nullptr);

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    return true;
}

int App::Loop()
{
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK App::Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    App* self = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        self = static_cast<App*>(cs->lpCreateParams);
        self->hwnd_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->OnMessage(m, w, l) : DefWindowProcW(h, m, w, l);
}

void App::MakeFonts()
{
    auto mk = [&](const wchar_t* family, float px, int style)
    { return std::make_unique<gp::Font>(family, S(px), style, gp::UnitPixel); };

    const int regular = gp::FontStyleRegular;
    f_.title = mk(L"Segoe UI Semibold", 23.0f, regular);
    f_.h2 = mk(L"Segoe UI Semibold", 15.0f, regular);
    f_.body = mk(L"Segoe UI", 13.5f, regular);
    f_.bodyB = mk(L"Segoe UI Semibold", 14.0f, regular);
    f_.sm = mk(L"Segoe UI", 12.0f, regular);
    f_.tiny = mk(L"Segoe UI", 11.0f, regular);
    f_.link = mk(L"Segoe UI Semibold", 13.0f, gp::FontStyleUnderline);
    f_.linkN = mk(L"Segoe UI Semibold", 13.0f, regular);
    f_.btn = mk(L"Segoe UI Semibold", 15.0f, regular);
    f_.chip = mk(L"Segoe UI Semibold", 12.5f, regular);
}

void App::ApplyDpi(UINT dpi, const RECT* suggested)
{
    scale_ = ScaleFor(dpi, hwnd_);
    MakeFonts();

    if (suggested)
    {
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    else
    {
        RECT rc{ 0, 0, static_cast<LONG>(kW * scale_), static_cast<LONG>(kH * scale_) };
        AdjustWindowRectExForDpi(&rc, static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE)), FALSE, 0, dpi);
        SetWindowPos(hwnd_, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ClampScroll();
    Invalidate();
}

// ---------------------------------------------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------------------------------------------

void App::Refresh()
{
    const HWND keep = (selected_ >= 0 && selected_ < static_cast<int>(games_.size())) ? games_[selected_].hwnd : nullptr;

    std::vector<GameItem> list;
    EnumCtx ctx{ &list, &games_, GetCurrentProcessId(), hostPid_ };
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&ctx));

    // Borderless / fullscreen-sized windows first (that is what a game usually looks like), the rest in z-order.
    std::stable_sort(list.begin(), list.end(), [](const GameItem& a, const GameItem& b) { return a.fills && !b.fills; });

    games_ = std::move(list);

    selected_ = -1;
    if (keep)
    {
        for (size_t i = 0; i < games_.size(); ++i)
        {
            if (games_[i].hwnd == keep)
            {
                selected_ = static_cast<int>(i);
                break;
            }
        }
    }

    ClampScroll();
    Invalidate();
}

void App::ClampScroll()
{
    const float content = static_cast<float>(games_.size()) * S(62.0f) - S(6.0f);
    const float maxScroll = max(0.0f, content - ListView().Height);
    scroll_ = min(max(scroll_, 0.0f), maxScroll);
}

void App::SetNote(const std::wstring& t, Note k)
{
    note_ = t;
    noteKind_ = k;
    Invalidate();
}

void App::Save()
{
    oc::SaveSettings(st_);
}

UINT* App::KeyRef(int sc)
{
    switch (sc)
    {
    case kScMenuKey: return &st_.menuKey;
    case kScFpsKey: return &st_.fpsKey;
    case kScFpsCycleKey: return &st_.fpsCycleKey;
    case kScFgKey: return &st_.fgKey;
    default: return nullptr;
    }
}

oc::Hotkey* App::HotkeyRef(int sc)
{
    switch (sc)
    {
    case kScStop: return &st_.stop;
    case kScLauncher: return &st_.launcher;
    default: return nullptr;
    }
}

std::wstring App::ChipText(int sc)
{
    if (const oc::Hotkey* h = HotkeyRef(sc))
        return HotkeyText(*h);
    const UINT* k = KeyRef(sc);
    return k ? KeyName(*k) : std::wstring();
}

bool App::RegisterShowHotkey()
{
    UnregisterHotKey(hwnd_, kHotkeyShow);
    if (!RegisterHotKey(hwnd_, kHotkeyShow, st_.launcher.mods | MOD_NOREPEAT, st_.launcher.vk))
    {
        SetNote(L"The \"Show this window\" shortcut is already used by another program. Pick a different one.", Note::Error);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Start / stop
// ---------------------------------------------------------------------------------------------------------------

void App::Start()
{
    if (Running())
    {
        Stop(false);
        return;
    }

    if (selected_ < 0 || selected_ >= static_cast<int>(games_.size()))
    {
        SetNote(L"Pick a game from the list first.", Note::Error);
        return;
    }

    const GameItem game = games_[selected_];
    if (!IsWindow(game.hwnd))
    {
        Refresh();
        SetNote(L"That window is gone. Pick the game again.", Note::Error);
        return;
    }

    const std::wstring dir = oc::ExeDir();
    const std::wstring host = dir + kHostExe;
    if (GetFileAttributesW(host.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        SetNote(L"OptiCaptureHost.exe was not found next to the launcher.", Note::Error);
        return;
    }

    startConfirmed_ = false;

    Save();
    if (st_.limitAuto)
        ApplyLimiter(); // RTSS profile first, so the limit is already in place when the game runs
    WriteOptiScalerKeys(st_);

    if (IsIconic(game.hwnd))
        ShowWindow(game.hwnd, SW_RESTORE);

    std::wstring cmd = L"\"" + host + L"\" --hwnd " + std::to_wstring(reinterpret_cast<ULONG_PTR>(game.hwnd)) +
                       L" --parent " + std::to_wstring(GetCurrentProcessId());
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(host.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi))
    {
        SetNote(L"Could not start OptiCaptureHost.exe (error " + std::to_wstring(GetLastError()) + L").", Note::Error);
        return;
    }
    CloseHandle(pi.hThread);

    host_ = pi.hProcess;
    hostPid_ = pi.dwProcessId;

    // new capture: forget the live data and let every warning be raised once more
    DeleteFileW(oc::StatusPath().c_str());
    liveValid_ = false;
    lastLiveStamp_ = 0;
    blockCount_ = 0;
    capActive_ = false;
    blockActive_ = false;
    for (bool& b : shownWarn_)
        b = false;
    warnQueue_.clear();

    target_ = game.hwnd;
    targetTitle_ = game.title;
    stopping_ = false;
    SetNote(L"");

    // Get out of the way, then hand the foreground back to the game once the overlay is up.
    ShowWindow(hwnd_, SW_MINIMIZE);
    SetTimer(hwnd_, kTimerFront, 1800, nullptr);
    Invalidate();
}

void App::Stop(bool wait)
{
    if (!host_)
        return;

    EnumWindows(CloseOverlayProc, static_cast<LPARAM>(hostPid_));
    stopping_ = true;
    stopAt_ = GetTickCount64() + 4000;
    Invalidate();

    if (wait)
    {
        if (WaitForSingleObject(host_, 2000) != WAIT_OBJECT_0)
            TerminateProcess(host_, 1);
    }
}

void App::PollHost()
{
    if (!host_)
        return;

    const DWORD w = WaitForSingleObject(host_, 0);
    if (w == WAIT_OBJECT_0)
    {
        DWORD code = 0;
        GetExitCodeProcess(host_, &code);
        CloseHandle(host_);
        host_ = nullptr;
        hostPid_ = 0;
        liveValid_ = false;
        capActive_ = false;
        blockActive_ = false;

        const bool asked = stopping_;
        stopping_ = false;

        if (asked || code == 0)
            SetNote(L"Capture ended.", Note::Ok);
        else
            SetNote(L"The capture host stopped (code " + std::to_wstring(code) + L"). See OptiCaptureHost.log.", Note::Error);

        Refresh();
        ShowOnTop();
        SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return;
    }

    if (stopping_ && GetTickCount64() > stopAt_)
    {
        TerminateProcess(host_, 1);
        stopAt_ = GetTickCount64() + 4000;
    }

    CheckLive();
}

void App::ShowOnTop()
{
    if (IsIconic(hwnd_))
        ShowWindow(hwnd_, SW_RESTORE);
    else
        ShowWindow(hwnd_, SW_SHOW);

    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    ForceForeground(hwnd_);
}

// ---------------------------------------------------------------------------------------------------------------
// Live status and warnings
// ---------------------------------------------------------------------------------------------------------------

namespace
{
std::wstring Num(double v, int decimals)
{
    wchar_t b[48] = {};
    swprintf_s(b, 48, decimals <= 0 ? L"%.0f" : (decimals == 1 ? L"%.1f" : L"%.2f"), v);
    return b;
}
} // namespace

void App::LoadWarnFlags()
{
    for (int i = 0; i < kWarnCount; ++i)
        hideWarn_[i] = GetPrivateProfileIntW(L"Warnings", kWarnKeys[i], 0, oc::SettingsPath().c_str()) != 0;
}

// Queues a warning dialog. It opens at once when the launcher window is visible; otherwise (the launcher is minimized
// while a game runs) it waits and the taskbar button flashes. Each kind is raised at most once per capture.
void App::RaiseWarning(int kind, const std::wstring& text)
{
    warnText_[kind] = text;
    if (hideWarn_[kind] || shownWarn_[kind])
        return;
    shownWarn_[kind] = true;
    warnQueue_.push_back(kind);

    if (IsIconic(hwnd_) || !IsWindowVisible(hwnd_))
    {
        FLASHWINFO fi{};
        fi.cbSize = sizeof(fi);
        fi.hwnd = hwnd_;
        fi.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
        fi.uCount = 3;
        FlashWindowEx(&fi);
    }
    else
    {
        OpenNextDialog();
    }
}

void App::OpenNextDialog()
{
    if (dlgKind_ >= 0 || warnQueue_.empty())
        return;
    dlgKind_ = warnQueue_.front();
    warnQueue_.erase(warnQueue_.begin());
    dlgDontShow_ = false;
    dlgStart_ = false;
    Invalidate();
}

void App::ApplyLimiter(bool off)
{
    std::wstring exe;
    HWND wnd = nullptr;
    if (selected_ >= 0 && selected_ < static_cast<int>(games_.size()))
    {
        exe = games_[selected_].exe;
        wnd = games_[selected_].hwnd;
    }
    const int hz = RefreshRateOf(wnd);

    LimiterOut r = DoLimiter(rtss_, st_, exe, hz, off);
    if (r.code == 1 && !IsElevated())
    {
        SetNote(L"RTSS needs administrator rights to write the limit - allow the Windows prompt.", Note::Info);
        r = RunLimiterElevated(exe, hz, off);
    }

    if (r.code == 2)
    {
        shownWarn_[kWarnRtssMissing] = false;
        RaiseWarning(kWarnRtssMissing,
                     L"The frame limiter uses RivaTuner Statistics Server (RTSS), and it was not found on this PC.\n\n"
                     L"Install RTSS (it comes with MSI Afterburner, or from guru3d.com), start it, then press Apply again.\n\n"
                     L"RTSS must be running while you play: it does the actual limiting.");
    }
    if (!r.info.empty())
        limitInfo_ = r.info;
    SetNote(r.note, r.code == 0 ? Note::Ok : Note::Error);
}

// Force Borderless: takes the frame off the selected game's window and stretches it over its monitor; pressing it again
// brings the frame and the old size back. (Exclusive fullscreen cannot be changed this way.)
void App::ToggleBorderless()
{
    if (selected_ < 0 || selected_ >= static_cast<int>(games_.size()))
    {
        SetNote(L"Pick a game in the list first.", Note::Error);
        return;
    }
    const HWND w = games_[selected_].hwnd;
    if (!IsWindow(w))
    {
        SetNote(L"That window is gone. Press Refresh.", Note::Error);
        return;
    }

    const int idx = BorderIndex(w);
    if (idx >= 0)
    {
        const BorderState b = border_[idx];
        bool ok = BorderApplyOff(w, b);
        if (!ok && !IsElevated())
        {
            SetNote(L"The game window needs administrator rights - allow the Windows prompt.", Note::Info);
            ok = RunBorderElevated(false, b);
        }
        if (!ok)
        {
            SetNote(L"Could not bring the window frame back.", Note::Error);
            return;
        }
        border_.erase(border_.begin() + idx);
        SetNote(L"Borderless is off: the window frame is back.", Note::Ok);
        Refresh();
        return;
    }

    BorderState b;
    b.wnd = w;
    b.style = GetWindowLongPtrW(w, GWL_STYLE);
    b.exStyle = GetWindowLongPtrW(w, GWL_EXSTYLE);
    b.wasMaximized = IsZoomed(w) != FALSE;
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (GetWindowPlacement(w, &wp) && (b.wasMaximized || IsIconic(w)))
        b.rect = wp.rcNormalPosition;
    else
        GetWindowRect(w, &b.rect);

    bool ok = BorderApplyOn(w, b);
    if (!ok && !IsElevated())
    {
        SetNote(L"The game window needs administrator rights - allow the Windows prompt.", Note::Info);
        ok = RunBorderElevated(true, b);
    }
    if (!ok)
    {
        SetNote(L"Windows did not let the launcher change that window.", Note::Error);
        return;
    }
    border_.push_back(b);
    SetNote(L"Borderless is on. Press the button again to bring the frame back.", Note::Ok);
    Refresh();
}

void App::CloseDialog(bool accepted)
{
    const int kind = dlgKind_;
    const bool wasStart = dlgStart_;
    if (kind >= 0 && dlgDontShow_)
    {
        hideWarn_[kind] = true;
        WritePrivateProfileStringW(L"Warnings", kWarnKeys[kind], L"1", oc::SettingsPath().c_str());
    }
    dlgKind_ = -1;
    dlgDontShow_ = false;
    dlgStart_ = false;
    Invalidate();

    if (wasStart)
    {
        if (accepted)
        {
            startConfirmed_ = true;
            Start();
        }
    }
    else
    {
        OpenNextDialog();
    }
}

// Reads the host's status file (about once a second) and raises the warnings that follow from it.
void App::CheckLive()
{
    liveValid_ = oc::ReadLiveStats(live_) && live_.pid == hostPid_ && GetTickCount64() - live_.stamp < 4000;
    if (!liveValid_)
    {
        capActive_ = false;
        blockActive_ = false;
        blockCount_ = 0;
        if (!IsIconic(hwnd_))
            Invalidate();
        return;
    }

    capActive_ = false;
    blockActive_ = false;
    lastLiveStamp_ = live_.stamp;

    if (!IsIconic(hwnd_))
        Invalidate();
}

// The live panel that replaces the (read-only) game list while a capture runs.
void App::PaintLive(gp::Graphics& g, const gp::RectF& view)
{
    FillRound(g, gp::RectF(view.X - S(2), view.Y - S(2), view.Width + S(4), view.Height + S(4)), S(10), col::Card());

    {
        gp::SolidBrush dot(liveValid_ ? col::Green() : col::Amber());
        g.FillEllipse(&dot, view.X + S(4), view.Y + S(8), S(9), S(9));
    }
    Text(g, liveValid_ ? L"Live status" : L"Live status – waiting for the host…", *f_.bodyB,
         gp::RectF(view.X + S(20), view.Y + S(2), view.Width - S(24), S(22)), col::Text());

    float y = view.Y + S(30);
    const float rowH = S(24);
    auto row = [&](const wchar_t* label, const std::wstring& value, const gp::Color& tone)
    {
        Text(g, label, *f_.body, gp::RectF(view.X + S(6), y, S(170), rowH), col::Muted());
        Text(g, value, *f_.bodyB, gp::RectF(view.X + S(180), y, view.Width - S(186), rowH), tone);
        y += rowH;
    };

    static const wchar_t* fgShort[] = { L"Intel Multi Frame Generation", L"Arthur Multi Frame Generation", L"No Frame Generation",
                                        L"FSR Frame Generation", L"Intel Frame Generation",
                                        L"Combo MFG", L"Nukem FG", L"FSR3-4 FG", L"Nvidia MFG" };
    static const wchar_t* vsShort[] = { L"Auto", L"On · 0", L"On · 1", L"Off" };
    static const wchar_t* focusNames[] = { L"Game", L"Overlay", L"Other window" };

    const std::wstring dash = L"–";
    row(L"Game", targetTitle_.empty() ? dash : targetTitle_, col::Text());
    row(L"Display refresh", liveValid_ && live_.refreshHz > 0 ? Num(live_.refreshHz, 0) + L" Hz" : dash, col::Text());
    row(L"Game frames (capture)", liveValid_ ? Num(live_.captureFps, 1) + L" fps" + (capActive_ ? L"   · looks capped" : L"") : dash,
        capActive_ ? col::Amber() : col::Text());
    row(L"Presented by the host", liveValid_ ? Num(live_.presentFps, 1) + L" /s" : dash, col::Text());
    row(L"Presents counted by DXGI", liveValid_ ? Num(live_.dxgiFps, 1) + L" /s" : dash, col::Text());
    row(L"Time inside Present",
        liveValid_ ? Num(live_.presentMs, 2) + L" ms   · wait " + Num(live_.waitMs, 2) + L" ms" : dash,
        blockActive_ ? col::Amber() : col::Text());
    row(L"Sync interval",
        liveValid_ ? std::to_wstring(live_.interval) + L"   · VSync " + vsShort[vsyncMode_ < 0 ? 0 : (vsyncMode_ == 2 ? 1 : (vsyncMode_ == 1 ? 2 : 3))] : dash,
        col::Text());
    row(L"Focus", liveValid_ ? focusNames[(live_.focus >= 0 && live_.focus <= 2) ? live_.focus : 2] : dash, col::Text());
    row(L"Frame generation", fgShort[(st_.frameGenMode >= 0 && st_.frameGenMode <= 8) ? st_.frameGenMode : 0], col::Text());

    if (capActive_ || blockActive_)
    {
        std::wstring w = capActive_ ? L"Game looks capped" : L"";
        if (blockActive_)
            w += (w.empty() ? L"" : L"  ·  ") + std::wstring(L"Present is waiting");
        Text(g, L"Check: " + w, *f_.sm, gp::RectF(view.X + S(6), y + S(4), view.Width - S(12), S(20)), col::Amber());
    }
}

// The warning dialog, drawn over the whole window (no separate window: the launcher is plain GDI+).
void App::PaintDialog(gp::Graphics& g, int W, int H)
{
    hits_.clear(); // nothing below the dialog can be clicked

    gp::SolidBrush dim(gp::Color(175, 6, 8, 12));
    g.FillRectangle(&dim, 0, 0, W, H);

    const int k = (dlgKind_ >= 0 && dlgKind_ < kWarnCount) ? dlgKind_ : 0;
    const gp::RectF card = R(130, 224, 676, 330);
    FillRound(g, card, S(16), col::Card());
    StrokeRound(g, card, S(16), col::Alpha(col::Amber(), 150), max(1.0f, S(1.5f)));

    {
        gp::SolidBrush dot(col::Amber());
        const gp::RectF d = R(160, 250, 12, 12);
        g.FillEllipse(&dot, d.X, d.Y, d.Width, d.Height);
    }
    Text(g, kWarnTitles[k], *f_.h2, R(182, 242, 590, 28), col::Amber());
    Text(g, warnText_[k], *f_.body, R(160, 282, 616, 190), col::Text(), 0, true);

    // "Don't show this warning again"
    {
        const gp::RectF box = R(160, 486, 20, 20);
        const bool hot = hover_ == kIdDlgCheck;
        FillRound(g, box, S(5), dlgDontShow_ ? col::Accent() : col::Row());
        StrokeRound(g, box, S(5), hot ? col::Accent2() : col::Line(), max(1.0f, S(1)));
        if (dlgDontShow_)
            Text(g, L"✓", *f_.chip, box, col::White(), 1);
        Text(g, L"Don't show this warning again", *f_.body, R(190, 484, 320, 24), col::Text());
        AddHit(R(160, 480, 360, 32), kIdDlgCheck);
    }

    auto button = [&](const gp::RectF& r, const wchar_t* text, int id, bool primary)
    {
        const bool hot = hover_ == id;
        if (primary)
            FillRound(g, r, S(10), hot ? col::Accent2() : col::Accent());
        else
            FillRound(g, r, S(10), hot ? col::RowHot() : col::Row());
        StrokeRound(g, r, S(10), hot ? col::Accent() : col::Line(), max(1.0f, S(1)));
        Text(g, text, *f_.chip, r, primary ? col::White() : col::Text(), 1);
        AddHit(r, id);
    };

    if (dlgStart_)
    {
        button(R(494, 512, 130, 34), L"Cancel", kIdDlgCancel, false);
        button(R(636, 512, 150, 34), L"Start anyway", kIdDlgOk, true);
    }
    else
    {
        button(R(656, 512, 130, 34), L"OK", kIdDlgOk, true);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------------------------

int App::HitAt(int x, int y) const
{
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it)
    {
        if (it->r.Contains(static_cast<float>(x), static_cast<float>(y)))
            return it->id;
    }
    return kIdNone;
}

void App::BeginCapture(int sc)
{
    capturing_ = sc;
    SetNote(L"");
    UnregisterHotKey(hwnd_, kHotkeyShow); // so that its own combination can be assigned too
    Invalidate();
}

LRESULT App::OnCaptureKey(UINT vk)
{
    const int sc = capturing_;

    if (vk == VK_ESCAPE)
    {
        capturing_ = -1;
        RegisterShowHotkey();
        Invalidate();
        return 0;
    }
    if (IsModifierVk(vk))
        return 0; // wait for the real key

    if (oc::Hotkey* target = HotkeyRef(sc))
    {
        // Stop / launcher: a global shortcut, so it needs Ctrl, Shift or Alt (otherwise it would clash with the game).
        UINT mods = 0;
        if (GetKeyState(VK_CONTROL) & 0x8000)
            mods |= MOD_CONTROL;
        if (GetKeyState(VK_SHIFT) & 0x8000)
            mods |= MOD_SHIFT;
        if (GetKeyState(VK_MENU) & 0x8000)
            mods |= MOD_ALT;
        if (mods == 0)
        {
            SetNote(L"Hold Ctrl, Shift or Alt together with the key (so it does not clash with the game).", Note::Error);
            return 0;
        }

        oc::Hotkey candidate;
        candidate.mods = mods;
        candidate.vk = vk;
        for (int other = 0; other < kScCount; ++other)
        {
            const oc::Hotkey* h = HotkeyRef(other);
            if (other != sc && h && *h == candidate)
            {
                SetNote(L"Already used by \"" + std::wstring(kShortcutDefs[other].label) + L"\".", Note::Error);
                return 0;
            }
        }
        *target = candidate;
    }
    else
    {
        // OptiScaler's own keys: a single key, no modifiers.
        for (int other = 0; other < kScCount; ++other)
        {
            const UINT* k = KeyRef(other);
            if (other != sc && k && *k == vk)
            {
                SetNote(L"Already used by \"" + std::wstring(kShortcutDefs[other].label) + L"\".", Note::Error);
                return 0;
            }
        }
        if (UINT* k = KeyRef(sc))
            *k = vk;
    }

    capturing_ = -1;
    Save();
    WriteOptiScalerKeys(st_);
    SetNote(L"Saved to OptiScaler.ini.", Note::Ok);
    RegisterShowHotkey();
    Invalidate();
    return 0;
}

void App::OnClick(int id)
{
    if (dlgKind_ >= 0)
    {
        if (id == kIdDlgCheck)
            dlgDontShow_ = !dlgDontShow_;
        else if (id == kIdDlgOk)
            CloseDialog(true);
        else if (id == kIdDlgCancel)
            CloseDialog(false);
        return;
    }

    if (id == kIdWarnReset)
    {
        for (int i = 0; i < kWarnCount; ++i)
        {
            hideWarn_[i] = false;
            WritePrivateProfileStringW(L"Warnings", kWarnKeys[i], L"0", oc::SettingsPath().c_str());
        }
        SetNote(L"Hidden warnings will be shown again.", Note::Ok);
        return;
    }

    if (id == kIdRefresh)
    {
        Refresh();
    }
    else if (id == kIdVsync)
    {
        // Order in the menu: Auto, On with SyncInterval 0, On with SyncInterval 1, Off.
        static const wchar_t* names[] = {
            L"Auto  (default, nothing forced)", L"On  (ForceVsync = true, SyncInterval = 0)",
            L"On  (ForceVsync = true, SyncInterval = 1)", L"Off  (ForceVsync = false)"
        };
        static const int modes[] = { -1, 2, 1, 0 };
        HMENU menu = CreatePopupMenu();
        if (menu)
        {
            int cur = 0;
            for (int v = 0; v < 4; ++v)
                if (modes[v] == vsyncMode_) cur = v;
            for (int v = 0; v < 4; ++v)
                AppendMenuW(menu, MF_STRING | (cur == v ? MF_CHECKED : 0), kVsyncMenuBase + v, names[v]);
            POINT pt{};
            GetCursorPos(&pt);
            const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
            DestroyMenu(menu);
            if (cmd >= kVsyncMenuBase && cmd <= kVsyncMenuBase + 3)
            {
                const int v = static_cast<int>(cmd - kVsyncMenuBase);
                vsyncMode_ = modes[v];
                if (ApplyVsyncToOptiScaler())
                {
                    SetNote(std::wstring(L"VSync: ") + names[v] + L". Restart the game to apply.", Note::Ok);
                }
                else
                {
                    SetNote(L"Could not update OptiScaler.ini. Check the file and permissions.", Note::Error);
                }
            }
        }
    }
    else if (id == kIdStart)
    {
        Start();
    }
    else if (id == kIdReset)
    {
        const oc::Settings d;
        st_.stop = d.stop;
        st_.launcher = d.launcher;
        st_.menuKey = d.menuKey;
        st_.fpsKey = d.fpsKey;
        st_.fpsCycleKey = d.fpsCycleKey;
        st_.fgKey = d.fgKey;
        Save();
        WriteOptiScalerKeys(st_);
        if (RegisterShowHotkey())
            SetNote(L"Shortcuts reset.", Note::Ok);
    }
    else if (id == kIdLink)
    {
        ShellExecuteW(hwnd_, L"open", kTelegramUrl, nullptr, nullptr, SW_SHOWNORMAL);
    }
    else if (id == kIdLinkJH)
    {
        ShellExecuteW(hwnd_, L"open", kTelegramJHUrl, nullptr, nullptr, SW_SHOWNORMAL);
    }
    else if (id == kIdLinkApps)
    {
        ShellExecuteW(hwnd_, L"open", kTelegramAppsUrl, nullptr, nullptr, SW_SHOWNORMAL);
    }
    else if (id == kIdLinkArchive)
    {
        ShellExecuteW(hwnd_, L"open", kTelegramArchiveUrl, nullptr, nullptr, SW_SHOWNORMAL);
    }
    else if (id == kIdFrameGenSelect)
    {
        HMENU menu = CreatePopupMenu();
        if (menu)
        {
            AppendMenuW(menu, MF_STRING | (st_.frameGenMode == 0 ? MF_CHECKED : 0), kFgMenuBase + 0, L"Intel Multi Frame Generation");
            AppendMenuW(menu, MF_STRING | (st_.frameGenMode == 1 ? MF_CHECKED : 0), kFgMenuBase + 1, L"Arthur Multi Frame Generation");
            AppendMenuW(menu, MF_STRING | (st_.frameGenMode == 2 ? MF_CHECKED : 0), kFgMenuBase + 2, L"No Frame Generation");
            AppendMenuW(menu, MF_STRING | (st_.frameGenMode == 3 ? MF_CHECKED : 0), kFgMenuBase + 3, L"FSR Frame Generation");
            AppendMenuW(menu, MF_STRING | (st_.frameGenMode == 4 ? MF_CHECKED : 0), kFgMenuBase + 4, L"Intel Frame Generation");
            AppendMenuW(menu, MF_STRING | (st_.frameGenMode == 5 ? MF_CHECKED : 0), kFgMenuBase + 5, L"Combo MFG");
            AppendMenuW(menu, MF_STRING | (st_.frameGenMode == 6 ? MF_CHECKED : 0), kFgMenuBase + 6, L"Nukem FG");
            AppendMenuW(menu, MF_STRING | (st_.frameGenMode == 7 ? MF_CHECKED : 0), kFgMenuBase + 7, L"FSR3-4 FG");
            AppendMenuW(menu, MF_STRING | (st_.frameGenMode == 8 ? MF_CHECKED : 0), kFgMenuBase + 8, L"Nvidia MFG");

            POINT pt{};
            GetCursorPos(&pt);
            const UINT cmd = TrackPopupMenu(
                menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                pt.x, pt.y, 0, hwnd_, nullptr);
            DestroyMenu(menu);

            if (cmd >= kFgMenuBase && cmd <= kFgMenuBase + 8)
            {
                st_.frameGenMode = static_cast<int>(cmd - kFgMenuBase);
                Save();

                static const wchar_t* names[] = {
                    L"Intel Multi Frame Generation",
                    L"Arthur Multi Frame Generation",
                    L"No Frame Generation",
                    L"FSR Frame Generation",
                    L"Intel Frame Generation",
                    L"Combo MFG",
                    L"Nukem FG",
                    L"FSR3-4 FG",
                    L"Nvidia MFG"
                };
                SetNote(std::wstring(L"Selected: ") + names[st_.frameGenMode] + L". Press Apply to write OptiScaler.ini.", Note::Ok);
            }
        }
    }
    else if (id == kIdFrameGenApply)
    {
        if (st_.frameGenMode < 0 || st_.frameGenMode > 8)
        {
            SetNote(L"Invalid Frame Generation selection.", Note::Error);
        }
        else if (st_.frameGenMode >= 5)
        {
            // Combo MFG ("Combo"), Nukem FG ("Nukem FG"), FSR3-4 FG ("FSR3-4 FG"), Nvidia MFG ("DlssG"): everything inside
            // that folder: only its OptiScaler.ini is copied next to the program, replacing the one there.
            static const wchar_t* const shown[] = { L"Combo", L"Nukem FG", L"FSR3-4 FG", L"DlssG" };
            static const wchar_t* const labels[] = { L"Combo MFG", L"Nukem FG", L"FSR3-4 FG", L"Nvidia MFG" };
            const int k = st_.frameGenMode - 5;
            const std::wstring base = shown[k];
            std::vector<std::wstring> folders = { base };
            for (const wchar_t* sep : { L"_", L"-", L"" })
            {
                std::wstring v = base;
                size_t p;
                while ((p = v.find(L' ')) != std::wstring::npos)
                    v.replace(p, 1, sep);
                if (v != base)
                    folders.push_back(v);
            }
            const std::wstring root = oc::ExeDir();
            std::wstring from;
            for (const std::wstring& f : folders)
            {
                const DWORD at = GetFileAttributesW((root + f).c_str());
                if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY))
                {
                    from = root + f;
                    break;
                }
            }
            if (from.empty())
            {
                SetNote(L"The \"" + base + L"\" folder was not found next to the launcher.", Note::Error);
            }
            else
            {
                const std::wstring src = from + L"\\OptiScaler.ini";
                if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES)
                {
                    SetNote(L"There is no OptiScaler.ini inside the \"" + base + L"\" folder.", Note::Error);
                }
                else if (!CopyFileW(src.c_str(), (root + L"OptiScaler.ini").c_str(), FALSE))
                {
                    SetNote(L"Could not replace OptiScaler.ini. Check the file and permissions.", Note::Error);
                }
                else
                {
                    WriteOptiScalerKeys(st_);
                    ApplyVsyncToOptiScaler();
                    Save();
                    SetNote(std::wstring(L"OptiScaler.ini replaced with the ") + labels[k] + L" one. Restart the game to use the change.", Note::Ok);
                }
            }
        }
        else if (st_.frameGenMode == 0 || st_.frameGenMode == 1 || st_.frameGenMode == 4)
        {
            // Intel MFG (folder "intel MFG"), Arthur (folder "artur") and Intel Frame Generation (folder "intel FG"): use
            // the ready-made OptiScaler.ini from that folder next to the launcher.
            const bool arthur = st_.frameGenMode == 1;
            const wchar_t* shownName = st_.frameGenMode == 0 ? L"intel MFG" : (arthur ? L"artur" : L"intel FG");
            std::vector<std::wstring> folders;
            if (st_.frameGenMode == 0)
                folders = { L"intel MFG", L"intel_MFG", L"intelMFG", L"intel-MFG" };
            else if (arthur)
                folders = { L"artur" };
            else
                folders = { L"intel FG", L"intel_FG", L"intelFG", L"intel-FG" };
            const std::wstring shown = shownName;
            std::wstring src = oc::ExeDir() + folders[0] + L"\\OptiScaler.ini";
            for (const std::wstring& folder : folders)
            {
                const std::wstring cand = oc::ExeDir() + folder + L"\\OptiScaler.ini";
                if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES)
                {
                    src = cand;
                    break;
                }
            }
            const std::wstring dst = oc::ExeDir() + L"OptiScaler.ini";
            if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                SetNote(L"The " + shown + L"\\OptiScaler.ini file was not found next to the launcher.", Note::Error);
            }
            else if (!CopyFileW(src.c_str(), dst.c_str(), FALSE))
            {
                SetNote(L"Could not replace OptiScaler.ini. Check the file and permissions.", Note::Error);
            }
            else
            {
                WriteOptiScalerKeys(st_);
                ApplyVsyncToOptiScaler();
                Save();
                SetNote(std::wstring(L"OptiScaler.ini replaced with the ") + (arthur ? L"Arthur" : (st_.frameGenMode == 0 ? L"Intel MFG" : L"Intel FG")) + L" one. Restart the game to use the change.", Note::Ok);
            }
        }
        else
        {
            static const wchar_t* values[] = {
                L"XeFG", L"DLSSG", L"nofg", L"fsrfg"
            };
            const std::wstring iniPath = oc::ExeDir() + L"OptiScaler.ini";

            if (WritePrivateProfileStringW(
                    L"FrameGen", L"FGOutput",
                    values[st_.frameGenMode], iniPath.c_str()))
            {
                ApplyVsyncToOptiScaler();
                SetNote(L"OptiScaler.ini updated. Restart the game to use the change.", Note::Ok);
            }
            else
            {
                SetNote(L"Could not update OptiScaler.ini. Check the file and permissions.", Note::Error);
            }
        }
    }
    else if (id == kIdPresentSelect)
    {
        static const wchar_t* names[] = {
            L"Auto  (FG on: no wait, FG off: VSync)", L"Always no wait  (interval 0)", L"Always wait for vblank  (interval 1)",
            L"Classic  (behaves like version 2.2.0)", L"Lossless-style  (low latency, tearing allowed)",
            L"Classic + half refresh  (exactly refresh / 2 frames)"
        };
        HMENU menu = CreatePopupMenu();
        if (menu)
        {
            for (int v = 0; v <= 5; ++v)
                AppendMenuW(menu, MF_STRING | (st_.presentMode == v ? MF_CHECKED : 0), kPresentMenuBase + v, names[v]);
            POINT pt{};
            GetCursorPos(&pt);
            const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
            DestroyMenu(menu);
            if (cmd >= kPresentMenuBase && cmd <= kPresentMenuBase + 5)
            {
                st_.presentMode = static_cast<int>(cmd - kPresentMenuBase);
                Save();
                ApplyVsyncToOptiScaler(); // classic mode keeps VSync host-side only
                SetNote(L"Present mode: " + std::wstring(names[st_.presentMode]) + (Running() ? L". Restart the game to apply." : L"."), Note::Ok);
            }
        }
    }
    else if (id == kIdDelaySelect)
    {
        HMENU menu = CreatePopupMenu();
        if (menu)
        {
            for (int v = 0; v <= 12; ++v)
            {
                std::wstring t = std::to_wstring(v) + L" ms";
                if (v == 0) t += L"  (off)";
                if (v == 3) t += L"  (recommended)";
                AppendMenuW(menu, MF_STRING | (st_.presentDelay == v ? MF_CHECKED : 0), kDelayMenuBase + v, t.c_str());
            }
            POINT pt{};
            GetCursorPos(&pt);
            const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
            DestroyMenu(menu);
            if (cmd >= static_cast<UINT>(kDelayMenuBase) && cmd <= static_cast<UINT>(kDelayMenuBase + 12))
            {
                st_.presentDelay = static_cast<int>(cmd - kDelayMenuBase);
                Save();
                SetNote(L"Present delay: " + std::to_wstring(st_.presentDelay) + L" ms" + (Running() ? L" (applied live)." : L"."), Note::Ok);
            }
        }
    }
    else if (id == kIdLimitTarget)
    {
        static const wchar_t* names[] = { L"Off", L"Game  (the selected game's .exe)", L"Host  (OptiCaptureHost.exe)", L"Global  (every program)" };
        HMENU menu = CreatePopupMenu();
        if (menu)
        {
            for (int v = 0; v <= 3; ++v)
                AppendMenuW(menu, MF_STRING | (st_.limitTarget == v ? MF_CHECKED : 0), kLimitTargetMenuBase + v, names[v]);
            POINT pt{};
            GetCursorPos(&pt);
            const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
            DestroyMenu(menu);
            if (cmd >= static_cast<UINT>(kLimitTargetMenuBase) && cmd <= static_cast<UINT>(kLimitTargetMenuBase + 3))
            {
                st_.limitTarget = static_cast<int>(cmd - kLimitTargetMenuBase);
                Save();
                SetNote(L"Press Apply to use the limiter setting.", Note::Info);
            }
        }
    }
    else if (id == kIdLimitMode)
    {
        static const wchar_t* names[] = { L"Async", L"Front edge sync  (default)", L"Back edge sync" };
        HMENU menu = CreatePopupMenu();
        if (menu)
        {
            for (int v = 0; v <= 2; ++v)
                AppendMenuW(menu, MF_STRING | (st_.limitMode == v ? MF_CHECKED : 0), kLimitModeMenuBase + v, names[v]);
            POINT pt{};
            GetCursorPos(&pt);
            const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
            DestroyMenu(menu);
            if (cmd >= static_cast<UINT>(kLimitModeMenuBase) && cmd <= static_cast<UINT>(kLimitModeMenuBase + 2))
            {
                st_.limitMode = static_cast<int>(cmd - kLimitModeMenuBase);
                Save();
                SetNote(L"Press Apply to use the limiter setting.", Note::Info);
            }
        }
    }
    else if (id == kIdLimitFps)
    {
        static const int fpsList[] = { 30, 40, 45, 48, 50, 60, 72, 75, 90, 100, 120, 144, 165, 240 };
        HMENU menu = CreatePopupMenu();
        if (menu)
        {
            AppendMenuW(menu, MF_STRING | (st_.limitFps == 0 ? MF_CHECKED : 0), kLimitFpsMenuBase + 0, L"Refresh rate  (auto)");
            AppendMenuW(menu, MF_STRING | (st_.limitFps == -2 ? MF_CHECKED : 0), kLimitFpsMenuBase + 1, L"1/2 refresh rate  (auto)");
            AppendMenuW(menu, MF_STRING | (st_.limitFps == -3 ? MF_CHECKED : 0), kLimitFpsMenuBase + 2, L"1/3 refresh rate  (auto)");
            AppendMenuW(menu, MF_STRING | (st_.limitFps == -4 ? MF_CHECKED : 0), kLimitFpsMenuBase + 3, L"1/4 refresh rate  (auto)");
            for (int i = 0; i < static_cast<int>(sizeof(fpsList) / sizeof(fpsList[0])); ++i)
                AppendMenuW(menu, MF_STRING | (st_.limitFps == fpsList[i] ? MF_CHECKED : 0), kLimitFpsMenuBase + 4 + i,
                            (std::to_wstring(fpsList[i]) + L" fps").c_str());
            POINT pt{};
            GetCursorPos(&pt);
            const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
            DestroyMenu(menu);
            if (cmd >= static_cast<UINT>(kLimitFpsMenuBase) && cmd < static_cast<UINT>(kLimitFpsMenuBase + 4 + 14))
            {
                const int k = static_cast<int>(cmd - kLimitFpsMenuBase);
                st_.limitFps = k == 0 ? 0 : (k <= 3 ? -(k + 1) : fpsList[k - 4]);
                Save();
                SetNote(L"Press Apply to use the limiter setting.", Note::Info);
            }
        }
    }
    else if (id == kIdLimitAuto)
    {
        st_.limitAuto = st_.limitAuto ? 0 : 1;
        Save();
        SetNote(st_.limitAuto ? L"The limiter will be applied every time you press START." : L"The limiter is applied only with the Apply button.", Note::Ok);
    }
    else if (id == kIdPictureOn || id == kIdPictureOff)
    {
        const bool on = id == kIdPictureOn;
        const std::wstring iniPath = oc::ExeDir() + L"OptiScaler.ini";
        const wchar_t* v = on ? L"true" : L"false";
        bool ok = WritePrivateProfileStringW(L"Sharpness", L"OverrideSharpness", v, iniPath.c_str()) != 0;
        if (on)
            ok = WritePrivateProfileStringW(L"Sharpness", L"Sharpness", L"0.600000", iniPath.c_str()) != 0 && ok;
        ok = WritePrivateProfileStringW(L"CAS", L"Enabled", v, iniPath.c_str()) != 0 && ok;
        ok = WritePrivateProfileStringW(L"DlssNr", L"NativeUpscalerPass", v, iniPath.c_str()) != 0 && ok;
        if (ok)
            SetNote(on ? L"Upscaler on the picture is on (sharpness 0.6, CAS, native upscaler pass). Restart the game to use the change."
                       : L"Upscaler on the picture is off. Restart the game to use the change.", Note::Ok);
        else
            SetNote(L"Could not update OptiScaler.ini. Check the file and permissions.", Note::Error);
    }
    else if (id == kIdBorderless)
    {
        ToggleBorderless();
    }
    else if (id == kIdLimitOff)
    {
        st_.limitAuto = 0; // otherwise START would switch it on again
        Save();
        ApplyLimiter(true);
    }
    else if (id == kIdLimitApply)
    {
        ApplyLimiter();
    }
    else if (id == kIdBoostSelect)
    {
        static const wchar_t* names[] = {
            L"Off  (weak GPUs)", L"Light  (CPU priority only)", L"Balanced  (CPU + GPU high)", L"Max  (best for strong GPUs)"
        };

        HMENU menu = CreatePopupMenu();
        if (menu)
        {
            for (int v = 3; v >= 0; --v)
                AppendMenuW(menu, MF_STRING | (st_.boost == v ? MF_CHECKED : 0), kBoostMenuBase + v, names[v]);

            POINT pt{};
            GetCursorPos(&pt);
            const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
            DestroyMenu(menu);

            if (cmd >= kBoostMenuBase && cmd <= kBoostMenuBase + 3)
            {
                st_.boost = static_cast<int>(cmd - kBoostMenuBase);
                Save();
                SetNote(L"Performance: " + std::wstring(names[st_.boost]) + (Running() ? L". Applies the next time you press START." : L"."),
                        Note::Ok);
            }
        }
    }
    else if (id == kIdUpscalerSelect)
    {
        HMENU menu = CreatePopupMenu();
        if (menu)
        {
            AppendMenuW(menu, MF_STRING | (st_.upscalerMode == 0 ? MF_CHECKED : 0),
                kUpscalerMenuBase + 0, L"Intel XeSS");
            AppendMenuW(menu, MF_STRING | (st_.upscalerMode == 1 ? MF_CHECKED : 0),
                kUpscalerMenuBase + 1, L"AMD FSR 3 & 4");
            AppendMenuW(menu, MF_STRING | (st_.upscalerMode == 2 ? MF_CHECKED : 0),
                kUpscalerMenuBase + 2, L"NVIDIA DLSS");
            AppendMenuW(menu, MF_STRING | (st_.upscalerMode == 3 ? MF_CHECKED : 0),
                kUpscalerMenuBase + 3, L"DLSS 3.7  (AMD and GTX 16 cards)");

            POINT pt{};
            GetCursorPos(&pt);
            const UINT cmd = TrackPopupMenu(
                menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                pt.x, pt.y, 0, hwnd_, nullptr);
            DestroyMenu(menu);

            if (cmd >= kUpscalerMenuBase && cmd <= kUpscalerMenuBase + 3)
            {
                st_.upscalerMode = static_cast<int>(cmd - kUpscalerMenuBase);
                Save();

                static const wchar_t* names[] = {
                    L"Intel XeSS", L"AMD FSR 3 & 4", L"NVIDIA DLSS", L"DLSS 3.7 (AMD / GTX 16)"
                };
                SetNote(std::wstring(L"Selected: ") + names[st_.upscalerMode] +
                    L". Press Apply to write OptiScaler.ini.", Note::Ok);
            }
        }
    }
    else if (id == kIdUpscalerApply)
    {
        if (st_.upscalerMode < 0 || st_.upscalerMode > 3)
        {
            SetNote(L"Invalid Upscaler selection.", Note::Error);
        }
        else
        {
            static const wchar_t* values[] = {
                L"xess", L"ffx", L"dlss", L"ffx" /* DLSS 3.7 for AMD / GTX 16 runs on the FSR upscaler */
            };
            const std::wstring iniPath = oc::ExeDir() + L"OptiScaler.ini";

            if (WritePrivateProfileStringW(
                    L"Upscalers", L"Dx12Upscaler",
                    values[st_.upscalerMode], iniPath.c_str()))
            {
                // FSR 3 & 4 and DLSS 3.7: also put the ready-made DLL set of that folder into the MK-JH folder.
                if (st_.upscalerMode == 1 || st_.upscalerMode == 3)
                {
                    const bool fsr = st_.upscalerMode == 1;
                    const std::vector<std::wstring> names = fsr
                        ? std::vector<std::wstring>{ L"FSR 4", L"FSR4", L"FSR_4", L"FSR-4" }
                        : std::vector<std::wstring>{ L"Dlss 3.7", L"Dlss3.7", L"Dlss_3.7", L"Dlss-3.7" };
                    const std::wstring root = oc::ExeDir();
                    std::wstring from;
                    for (const std::wstring& n : names)
                    {
                        for (const std::wstring& base : { root + L"MK-JH\\", root })
                        {
                            const std::wstring cand = base + n;
                            const DWORD at = GetFileAttributesW(cand.c_str());
                            if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY))
                            {
                                from = cand;
                                break;
                            }
                        }
                        if (!from.empty())
                            break;
                    }
                    const std::wstring label = fsr ? L"FSR 4" : L"Dlss 3.7";
                    if (from.empty())
                    {
                        SetNote(L"OptiScaler.ini updated, but the \"" + label + L"\" folder was not found (next to the launcher or inside MK-JH).", Note::Error);
                    }
                    else
                    {
                        const std::wstring to = root + L"MK-JH";
                        CreateDirectoryW(to.c_str(), nullptr);
                        int copied = 0, failed = 0;
                        CopyTree(from, to, copied, failed);
                        if (failed > 0)
                            SetNote(L"Copied " + std::to_wstring(copied) + L" files from " + label + L" to MK-JH, but " + std::to_wstring(failed) +
                                        L" could not be replaced (is the game or the host still running?).", Note::Error);
                        else
                            SetNote(L"OptiScaler.ini updated and the " + label + L" files (" + std::to_wstring(copied) +
                                        L") were copied into MK-JH. Restart the game to use the change.", Note::Ok);
                    }
                }
                else
                {
                    SetNote(L"OptiScaler.ini updated. Restart the game to use the change.", Note::Ok);
                }
            }
            else
            {
                SetNote(L"Could not update OptiScaler.ini. Check the file and permissions.", Note::Error);
            }
        }
    }
    else if (id >= kIdShortcut0 && id < kIdShortcut0 + kScCount)
    {
        BeginCapture(id - kIdShortcut0);
    }
    else if (id >= kIdRow0 && id < kIdRow0 + static_cast<int>(games_.size()))
    {
        selected_ = id - kIdRow0;
        SetNote(L"");
    }
    ReadPictureState();
    Invalidate();
}

LRESULT App::OnMessage(UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd_, &ps);
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        const int cw = rc.right - rc.left;
        const int ch = rc.bottom - rc.top;
        if (cw > 0 && ch > 0)
        {
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, cw, ch);
            HGDIOBJ old = SelectObject(mem, bmp);
            Paint(mem, cw, ch);
            BitBlt(dc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(hwnd_, &ps);
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        if (!tracking_)
        {
            TRACKMOUSEEVENT t{};
            t.cbSize = sizeof(t);
            t.dwFlags = TME_LEAVE;
            t.hwndTrack = hwnd_;
            TrackMouseEvent(&t);
            tracking_ = true;
        }
        const int h = HitAt(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (h != hover_)
        {
            hover_ = h;
            Invalidate();
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        tracking_ = false;
        if (hover_ != kIdNone)
        {
            hover_ = kIdNone;
            Invalidate();
        }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT)
        {
            SetCursor(LoadCursorW(nullptr, hover_ != kIdNone ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_LBUTTONDOWN:
    {
        const int h = HitAt(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (capturing_ >= 0 && h != kIdShortcut0 + capturing_)
        {
            capturing_ = -1;
            RegisterShowHotkey();
        }
        pressed_ = h;
        SetCapture(hwnd_);
        Invalidate();
        return 0;
    }

    case WM_LBUTTONUP:
    {
        ReleaseCapture();
        const int h = HitAt(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        const int was = pressed_;
        pressed_ = kIdNone;
        if (h != kIdNone && h == was)
            OnClick(h);
        Invalidate();
        return 0;
    }

    case WM_MOUSEWHEEL:
    {
        POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        ScreenToClient(hwnd_, &p);
        const gp::RectF view = ListView();
        if (view.Contains(static_cast<float>(p.x), static_cast<float>(p.y)))
        {
            scroll_ -= static_cast<float>(GET_WHEEL_DELTA_WPARAM(w)) / 120.0f * S(48.0f);
            ClampScroll();
            Invalidate();
        }
        return 0;
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (dlgKind_ >= 0)
        {
            if (m == WM_KEYDOWN && w == VK_RETURN)
                CloseDialog(true);
            else if (m == WM_KEYDOWN && w == VK_ESCAPE)
                CloseDialog(false);
            return 0;
        }
        if (capturing_ >= 0)
            return OnCaptureKey(static_cast<UINT>(w));
        if (m == WM_KEYDOWN && w == VK_F5)
        {
            Refresh();
            return 0;
        }
        break;

    case WM_SYSCHAR:
        if (capturing_ >= 0)
            return 0; // no menu beep while a shortcut is being typed
        break;

    case WM_TIMER:
        if (w == kTimerList)
        {
            if (!Running() && capturing_ < 0)
                Refresh();
        }
        else if (w == kTimerHost)
        {
            PollHost();
            // a warning that came up while the launcher was minimized: show it as soon as the window is visible again
            if (dlgKind_ < 0 && !warnQueue_.empty() && !IsIconic(hwnd_) && IsWindowVisible(hwnd_))
                OpenNextDialog();
        }
        else if (w == kTimerFront)
        {
            KillTimer(hwnd_, kTimerFront);
            if (Running() && IsWindow(target_))
                ForceForeground(target_);
        }
        return 0;

    case WM_HOTKEY:
        if (w == kHotkeyShow)
        {
            ShowOnTop();
            Refresh();
        }
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE)
        {
            if (capturing_ >= 0)
            {
                capturing_ = -1;
                RegisterShowHotkey();
                Invalidate();
            }
            // Shown on top of the game by the shortcut: go back below the overlay once the user clicks elsewhere.
            SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return 0;

    case WM_DPICHANGED:
        ApplyDpi(HIWORD(w), reinterpret_cast<const RECT*>(l));
        return 0;

    case WM_CLOSE:
        Stop(true);
        DestroyWindow(hwnd_);
        return 0;

    case WM_DESTROY:
        UnregisterHotKey(hwnd_, kHotkeyShow);
        KillTimer(hwnd_, kTimerList);
        KillTimer(hwnd_, kTimerHost);
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

// ---------------------------------------------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------------------------------------------

void App::Text(gp::Graphics& g, const std::wstring& t, const gp::Font& f, const gp::RectF& r, const gp::Color& c, int align,
               bool wrap)
{
    gp::StringFormat fmt;
    fmt.SetAlignment(align == 0 ? gp::StringAlignmentNear : (align == 1 ? gp::StringAlignmentCenter : gp::StringAlignmentFar));
    fmt.SetLineAlignment(wrap ? gp::StringAlignmentNear : gp::StringAlignmentCenter);
    fmt.SetTrimming(gp::StringTrimmingEllipsisCharacter);
    if (!wrap)
        fmt.SetFormatFlags(gp::StringFormatFlagsNoWrap);

    gp::SolidBrush b(c);
    g.DrawString(t.c_str(), -1, &f, r, &fmt, &b);
}

void App::Card(gp::Graphics& g, const gp::RectF& r)
{
    FillRound(g, r, S(14), col::Card());
    StrokeRound(g, r, S(14), col::Line(), max(1.0f, S(1)));
}

void App::Chip(gp::Graphics& g, const gp::RectF& r, const std::wstring& text, int id, bool capturing)
{
    const bool hot = hover_ == id;
    if (capturing)
    {
        FillRound(g, r, S(8), col::Alpha(col::Accent(), 48));
        StrokeRound(g, r, S(8), col::Accent(), max(1.5f, S(1.5f)));
        Text(g, L"Press keys\u2026", *f_.chip, r, col::Accent(), 1);
    }
    else
    {
        FillRound(g, r, S(8), hot ? col::RowHot() : col::Row());
        StrokeRound(g, r, S(8), hot ? col::Accent() : col::Line(), max(1.0f, S(1)));
        Text(g, text, *f_.chip, r, col::Text(), 1);
    }
    AddHit(r, id);
}

void App::Button(gp::Graphics& g, const gp::RectF& r, const std::wstring& text, bool disabled, bool danger)
{
    const bool hot = hover_ == kIdStart;
    const bool down = pressed_ == kIdStart && hot;

    if (disabled)
    {
        FillRound(g, r, S(12), col::Row());
        StrokeRound(g, r, S(12), col::Line(), max(1.0f, S(1)));
        Text(g, text, *f_.btn, r, col::Muted(), 1);
    }
    else
    {
        if (danger)
            GradRound(g, r, S(12), col::Red(), gp::Color(255, 255, 140, 92));
        else
            GradRound(g, r, S(12), col::Accent(), col::Accent2());

        if (hot)
            FillRound(g, r, S(12), gp::Color(down ? 40 : 26, down ? 0 : 255, down ? 0 : 255, down ? 0 : 255));
        Text(g, text, *f_.btn, r, col::White(), 1);
    }
    AddHit(r, kIdStart);
}

void App::Paint(HDC dc, int W, int H)
{
    gp::Graphics g(dc);
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
    g.SetTextRenderingHint(gp::TextRenderingHintClearTypeGridFit);

    hits_.clear();
    const bool running = Running();
    const bool locked = running; // the list is read-only while capturing

    {
        gp::SolidBrush bg(col::Bg());
        g.FillRectangle(&bg, 0, 0, W, H);

        // A soft glow behind the header.
        const gp::RectF glow(0.0f, 0.0f, static_cast<float>(W), S(130));
        gp::LinearGradientBrush gb(glow, gp::Color(70, 108, 140, 255), gp::Color(0, 13, 15, 20), gp::LinearGradientModeVertical);
        g.FillRectangle(&gb, glow);
    }

    // ---- header ----
    if (HICON logo = reinterpret_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 64, 64, LR_DEFAULTCOLOR))) { HDC dc = g.GetHDC(); DrawIconEx(dc, static_cast<int>(S(24)), static_cast<int>(S(22)), logo, static_cast<int>(S(46)), static_cast<int>(S(46)), 0, nullptr, DI_NORMAL); g.ReleaseHDC(dc); DestroyIcon(logo); }
    Text(g, L"OptiCaptureHost", *f_.title, R(84, 18, 420, 32), col::Text());
    {
        // version badge, right after the title
        const gp::RectF badge = R(288, 25, 58, 20);
        FillRound(g, badge, S(10), col::Alpha(col::Accent(), 40));
        StrokeRound(g, badge, S(10), col::Alpha(col::Accent(), 110), max(1.0f, S(1)));
        Text(g, std::wstring(L"v") + kVersion, *f_.tiny, badge, col::Accent(), 1);
    }
    Text(g, L"Capture any game \u00B7 FSR 4 and XeSS multi frame generation through OptiScaler", *f_.sm, R(84, 50, 620, 20),
         col::Muted());

    {
        const gp::RectF pill = R(762, 30, 150, 30);
        const gp::Color tone = stopping_ ? col::Amber() : (running ? col::Green() : col::Muted());
        FillRound(g, pill, S(15), col::Alpha(tone, 36));
        StrokeRound(g, pill, S(15), col::Alpha(tone, 120), max(1.0f, S(1)));
        gp::SolidBrush dot(tone);
        g.FillEllipse(&dot, pill.X + S(14), pill.Y + S(10), S(10), S(10));
        Text(g, stopping_ ? L"Stopping" : (running ? L"Capturing" : L"Ready"), *f_.bodyB,
             gp::RectF(pill.X + S(32), pill.Y, S(110), pill.Height), tone);
    }

    // ---- games card ----
    const gp::RectF left = R(24, 100, 500, 698);
    Card(g, left);
    Text(g, L"Choose a game", *f_.h2, R(44, 112, 300, 24), col::Text());
    Text(g, L"Pick the window of the game you want to enhance", *f_.sm, R(44, 136, 340, 18), col::Muted());

    {
        // Force Borderless on / off for the selected game
        const bool pick = selected_ >= 0 && selected_ < static_cast<int>(games_.size());
        const bool on = pick && BorderIndex(games_[selected_].hwnd) >= 0;
        const gp::RectF bb = R(310, 108, 94, 26);
        const bool hotB = hover_ == kIdBorderless && pick;
        FillRound(g, bb, S(8), on ? col::Alpha(col::Green(), 40) : (hotB ? col::RowHot() : col::Row()));
        StrokeRound(g, bb, S(8), on ? col::Green() : (hotB ? col::Accent() : col::Line()), max(1.0f, S(1)));
        Text(g, on ? L"Borderless: On" : L"Borderless: Off", *f_.chip, bb, pick ? (on ? col::Green() : col::Text()) : col::Muted(), 1);
        if (pick)
            AddHit(bb, kIdBorderless);
    }

    {
        const gp::RectF btn = R(414, 114, 90, 30);
        const bool hot = hover_ == kIdRefresh && !locked;
        FillRound(g, btn, S(8), hot ? col::RowHot() : col::Row());
        StrokeRound(g, btn, S(8), hot ? col::Accent() : col::Line(), max(1.0f, S(1)));
        Text(g, L"Refresh", *f_.chip, btn, locked ? col::Muted() : col::Text(), 1);
        if (!locked)
            AddHit(btn, kIdRefresh);
    }

    {
        const gp::RectF view = ListView();
        const float pitch = S(62);
        const float rowH = S(56);
        const float rowW = S(462);

        g.SetClip(view);
        float y = view.Y - scroll_;
        for (size_t i = 0; i < games_.size(); ++i, y += pitch)
        {
            if (y + rowH < view.Y || y > view.Y + view.Height)
                continue;

            const GameItem& it = games_[i];
            const int id = kIdRow0 + static_cast<int>(i);
            const bool sel = static_cast<int>(i) == selected_;
            const bool hot = hover_ == id && !locked;
            const gp::RectF row(view.X, y, rowW, rowH);

            FillRound(g, row, S(10), sel ? col::RowSel() : (hot ? col::RowHot() : col::Row()));
            if (sel)
                StrokeRound(g, row, S(10), col::Accent(), max(1.5f, S(1.5f)));

            const float ic = S(30);
            const float ix = row.X + S(14);
            const float iy = row.Y + (rowH - ic) / 2.0f;
            if (iy >= view.Y && iy + ic <= view.Y + view.Height)
            {
                HDC hdc = g.GetHDC();
                DrawIconEx(hdc, static_cast<int>(ix), static_cast<int>(iy), it.icon ? it.icon : LoadIconW(nullptr, IDI_APPLICATION),
                           static_cast<int>(ic), static_cast<int>(ic), 0, nullptr, DI_NORMAL);
                g.ReleaseHDC(hdc);
            }

            Text(g, it.title, *f_.bodyB, gp::RectF(row.X + S(56), row.Y + S(8), rowW - S(56) - S(34), S(22)), col::Text());

            std::wstring sub = it.exe.empty() ? L"protected process" : it.exe;
            sub += L"  \u00B7  " + std::to_wstring(it.w) + L"\u00D7" + std::to_wstring(it.h);
            if (it.fills)
                sub += L"  \u00B7  borderless";
            if (it.minimized)
                sub += L"  \u00B7  minimized";
            Text(g, sub, *f_.tiny, gp::RectF(row.X + S(56), row.Y + S(30), rowW - S(56) - S(34), S(18)), col::Muted());

            if (sel)
            {
                gp::SolidBrush dot(col::Accent());
                g.FillEllipse(&dot, row.X + rowW - S(26), row.Y + (rowH - S(10)) / 2.0f, S(10), S(10));
            }

            if (!locked)
            {
                const float top = max(row.Y, view.Y);
                const float bot = min(row.Y + row.Height, view.Y + view.Height);
                if (bot > top)
                    AddHit(gp::RectF(row.X, top, row.Width, bot - top), id);
            }
        }

        if (games_.empty())
            Text(g, L"No windows found. Start the game, then press Refresh.", *f_.body, view, col::Muted(), 1);

        // thin scrollbar
        const float content = static_cast<float>(games_.size()) * pitch - S(6);
        if (content > view.Height)
        {
            const float track = view.Height;
            const float thumb = max(S(30), track * view.Height / content);
            const float maxScroll = content - view.Height;
            const float ty = view.Y + (track - thumb) * (maxScroll > 0 ? scroll_ / maxScroll : 0.0f);
            FillRound(g, gp::RectF(view.X + view.Width - S(5), ty, S(4), thumb), S(2), col::Alpha(col::Muted(), 120));
        }
        g.ResetClip();

        if (locked)
            FillRound(g, gp::RectF(view.X - S(2), view.Y - S(2), view.Width + S(4), view.Height + S(4)), S(10),
                      col::Alpha(col::Card(), 150));
    }

    // While a capture runs the game list is read-only anyway: show the live status in its place.
    if (running)
        PaintLive(g, ListView());

    // ---- performance selector (under the game list) ----
    {
        static const wchar_t* boostNames[] = { L"Off  (weak GPUs)", L"Light  (CPU priority)", L"Balanced  (CPU + GPU high)", L"Max  (strong GPUs)" };

        Text(g, L"Performance", *f_.bodyB, R(44, 652, 130, 24), col::Text());

        const gp::RectF selector = R(176, 648, 328, 32);
        const bool hot = hover_ == kIdBoostSelect;
        FillRound(g, selector, S(8), hot ? col::RowHot() : col::Row());
        StrokeRound(g, selector, S(8), hot ? col::Accent() : col::Line(), max(1.0f, S(1)));
        Text(g, boostNames[(st_.boost >= 0 && st_.boost <= 3) ? st_.boost : 3], *f_.chip, R(186, 654, 308, 22), col::Text());
        AddHit(selector, kIdBoostSelect);
    }

    // ---- present mode selector ----
    {
        static const wchar_t* presentNames[] = { L"Auto  (recommended)", L"Always no wait  (interval 0)", L"Always wait for vblank  (interval 1)", L"Classic  (like 2.2.0)", L"Lossless-style  (low latency)", L"Classic + half refresh" };

        Text(g, L"Present mode", *f_.bodyB, R(44, 694, 130, 24), col::Text());

        const gp::RectF selector = R(176, 690, 328, 32);
        const bool hot = hover_ == kIdPresentSelect;
        FillRound(g, selector, S(8), hot ? col::RowHot() : col::Row());
        StrokeRound(g, selector, S(8), hot ? col::Accent() : col::Line(), max(1.0f, S(1)));
        Text(g, presentNames[(st_.presentMode >= 0 && st_.presentMode <= 5) ? st_.presentMode : 0], *f_.chip, R(186, 696, 308, 22), col::Text());
        AddHit(selector, kIdPresentSelect);
    }

    // ---- present delay selector ----
    {
        Text(g, L"Present delay", *f_.bodyB, R(44, 736, 130, 24), col::Text());

        const gp::RectF selector = R(176, 732, 328, 32);
        const bool hot = hover_ == kIdDelaySelect;
        FillRound(g, selector, S(8), hot ? col::RowHot() : col::Row());
        StrokeRound(g, selector, S(8), hot ? col::Accent() : col::Line(), max(1.0f, S(1)));
        std::wstring t = std::to_wstring(st_.presentDelay) + L" ms";
        if (st_.presentDelay == 0) t += L"  (off)";
        else if (st_.presentDelay == 3) t += L"  (recommended)";
        Text(g, t, *f_.chip, R(186, 738, 308, 22), col::Text());
        AddHit(selector, kIdDelaySelect);
    }

    Text(g, L"Tip: run the game in Borderless or Windowed mode, not Exclusive Fullscreen.", *f_.tiny, R(44, 772, 460, 22),
         col::Muted(), 0, true);

    // ---- VSync card ----
    {
        const gp::RectF c = R(540, 100, 372, 92);
        Card(g, c);
        if (hover_ == kIdVsync)
            StrokeRound(g, c, S(14), col::Alpha(col::Accent(), 150), max(1.0f, S(1)));

        Text(g, L"VSync", *f_.h2, R(560, 114, 200, 24), col::Text());
        Text(g, L"Sets OptiScaler ForceVsync. Auto leaves it to OptiScaler and gives the lowest latency.", *f_.sm,
             R(560, 140, 200, 40), col::Muted(), 0, true);

        static const wchar_t* vsNames[] = { L"Auto", L"On \u00B7 0", L"On \u00B7 1", L"Off" };
        const gp::RectF chip = R(790, 131, 104, 30);
        const bool hotChip = hover_ == kIdVsync;
        FillRound(g, chip, S(8), hotChip ? col::RowHot() : col::Row());
        StrokeRound(g, chip, S(8), hotChip ? col::Accent() : col::Line(), max(1.0f, S(1)));
        Text(g, vsNames[vsyncMode_ < 0 ? 0 : (vsyncMode_ == 2 ? 1 : (vsyncMode_ == 1 ? 2 : 3))], *f_.chip, R(802, 136, 84, 22), col::Text());
        AddHit(c, kIdVsync);
    }

    // ---- frame limiter card (RivaTuner, async) ----
    {
        static const wchar_t* targetNames[] = { L"Off", L"Game", L"Host", L"Global" };
        static const wchar_t* modeShort[] = { L"Async", L"Front edge sync", L"Back edge sync" };
        const gp::RectF c = R(540, 204, 372, 164);
        Card(g, c);
        Text(g, L"Frame limiter  ·  RivaTuner", *f_.h2, R(560, 214, 340, 24), col::Text());

        const gp::RectF t = R(560, 246, 150, 30);
        const bool hotT = hover_ == kIdLimitTarget;
        FillRound(g, t, S(8), hotT ? col::RowHot() : col::Row());
        StrokeRound(g, t, S(8), hotT ? col::Accent() : col::Line(), max(1.0f, S(1)));
        Text(g, std::wstring(L"Lock:  ") + targetNames[(st_.limitTarget >= 0 && st_.limitTarget <= 3) ? st_.limitTarget : 0], *f_.chip,
             R(570, 252, 134, 22), col::Text());
        AddHit(t, kIdLimitTarget);

        const gp::RectF mo = R(720, 246, 172, 30);
        const bool hotM = hover_ == kIdLimitMode;
        FillRound(g, mo, S(8), hotM ? col::RowHot() : col::Row());
        StrokeRound(g, mo, S(8), hotM ? col::Accent() : col::Line(), max(1.0f, S(1)));
        Text(g, modeShort[(st_.limitMode >= 0 && st_.limitMode <= 2) ? st_.limitMode : 1], *f_.chip, R(730, 252, 154, 22), col::Text());
        AddHit(mo, kIdLimitMode);

        std::wstring fpsText;
        if (st_.limitFps == 0) fpsText = L"Refresh rate  (auto)";
        else if (st_.limitFps == -2) fpsText = L"1/2 refresh  (auto)";
        else if (st_.limitFps == -3) fpsText = L"1/3 refresh  (auto)";
        else if (st_.limitFps == -4) fpsText = L"1/4 refresh  (auto)";
        else fpsText = std::to_wstring(st_.limitFps) + L" fps";
        const gp::RectF f = R(560, 284, 230, 30);
        const bool hotF = hover_ == kIdLimitFps;
        FillRound(g, f, S(8), hotF ? col::RowHot() : col::Row());
        StrokeRound(g, f, S(8), hotF ? col::Accent() : col::Line(), max(1.0f, S(1)));
        Text(g, fpsText, *f_.chip, R(568, 290, 214, 22), col::Text());
        AddHit(f, kIdLimitFps);

        const gp::RectF apply = R(800, 284, 48, 30);
        const bool hotA = hover_ == kIdLimitApply;
        FillRound(g, apply, S(8), hotA ? col::Accent2() : col::Accent());
        StrokeRound(g, apply, S(8), col::Line(), max(1.0f, S(1)));
        Text(g, L"Apply", *f_.chip, apply, col::White(), 1);
        AddHit(apply, kIdLimitApply);

        const gp::RectF offb = R(852, 284, 40, 30);
        const bool hotO = hover_ == kIdLimitOff;
        FillRound(g, offb, S(8), hotO ? col::RowHot() : col::Row());
        StrokeRound(g, offb, S(8), hotO ? col::Accent() : col::Line(), max(1.0f, S(1)));
        Text(g, L"Off", *f_.chip, offb, col::Text(), 1);
        AddHit(offb, kIdLimitOff);

        {
            const gp::RectF box = R(560, 324, 18, 18);
            const bool hot = hover_ == kIdLimitAuto;
            FillRound(g, box, S(5), st_.limitAuto ? col::Accent() : col::Row());
            StrokeRound(g, box, S(5), hot ? col::Accent2() : col::Line(), max(1.0f, S(1)));
            if (st_.limitAuto)
                Text(g, L"\u2713", *f_.chip, box, col::White(), 1);
            Text(g, L"Apply automatically when the game starts", *f_.body, R(586, 322, 306, 22), col::Text());
            AddHit(R(560, 320, 332, 26), kIdLimitAuto);
        }
        Text(g, limitInfo_.empty() ? std::wstring(L"Needs RivaTuner Statistics Server (async / front / back edge sync).") : limitInfo_,
             *f_.tiny, R(560, 348, 332, 14), col::Muted());
    }

    // ---- shortcuts card ----
    {
        const gp::RectF c = R(540, 380, 372, 432);
        Card(g, c);
        Text(g, L"Shortcuts", *f_.h2, R(560, 576, 200, 24), col::Text());

        const gp::RectF reset = R(822, 576, 70, 24);
        Text(g, L"Reset", hover_ == kIdReset ? *f_.link : *f_.linkN, reset, hover_ == kIdReset ? col::Accent2() : col::Muted(), 2);
        AddHit(reset, kIdReset);

        // brings back the warnings that were hidden with "Don't show this warning again"
        const gp::RectF warnReset = R(640, 576, 170, 24);
        Text(g, L"Show warnings again", hover_ == kIdWarnReset ? *f_.link : *f_.linkN, warnReset,
             hover_ == kIdWarnReset ? col::Accent2() : col::Muted(), 2);
        AddHit(warnReset, kIdWarnReset);

        // ---- Frame Generation controls ----
        {
            static const wchar_t* fgNames[] = {
                L"Intel Multi Frame Generation",
                L"Arthur Multi Frame Generation",
                L"No Frame Generation",
                L"FSR Frame Generation",
                L"Intel Frame Generation",
                L"Combo MFG",
                L"Nukem FG",
                L"FSR3-4 FG",
                L"Nvidia MFG"
            };

            Text(g, L"Frame Generation", *f_.bodyB,
                R(560, 386, 230, 24), col::Text());

            const gp::RectF selector = R(560, 412, 230, 30);
            const bool selectorHot = hover_ == kIdFrameGenSelect && !locked;
            FillRound(g, selector, S(8), selectorHot ? col::RowHot() : col::Row());
            StrokeRound(g, selector, S(8),
                selectorHot ? col::Accent() : col::Line(), max(1.0f, S(1)));
            Text(g, fgNames[(st_.frameGenMode >= 0 && st_.frameGenMode <= 8)
                ? st_.frameGenMode : 0], *f_.chip,
                R(568, 418, 214, 22), locked ? col::Muted() : col::Text());
            if (!locked)
                AddHit(selector, kIdFrameGenSelect);

            const gp::RectF apply = R(800, 412, 92, 30);
            const bool applyHot = hover_ == kIdFrameGenApply && !locked;
            FillRound(g, apply, S(8), applyHot ? col::Accent2() : col::Accent());
            StrokeRound(g, apply, S(8), col::Line(), max(1.0f, S(1)));
            Text(g, L"Apply", *f_.chip, apply, col::White(), 1);
            if (!locked)
                AddHit(apply, kIdFrameGenApply);
        }

        // ---- Upscaler controls ----
        {
            static const wchar_t* upscalerNames[] = {
                L"Intel XeSS",
                L"AMD FSR 3 & 4",
                L"NVIDIA DLSS",
                L"DLSS 3.7  (AMD / GTX 16)"
            };

            Text(g, L"Upscaler", *f_.bodyB,
                R(560, 448, 230, 24), col::Text());

            const gp::RectF selector = R(560, 472, 230, 30);
            const bool selectorHot = hover_ == kIdUpscalerSelect && !locked;
            FillRound(g, selector, S(8), selectorHot ? col::RowHot() : col::Row());
            StrokeRound(g, selector, S(8),
                selectorHot ? col::Accent() : col::Line(), max(1.0f, S(1)));
            Text(g, upscalerNames[(st_.upscalerMode >= 0 && st_.upscalerMode <= 3)
                ? st_.upscalerMode : 0], *f_.chip,
                R(568, 478, 214, 22), locked ? col::Muted() : col::Text());
            if (!locked)
                AddHit(selector, kIdUpscalerSelect);

            const gp::RectF apply = R(800, 472, 92, 30);
            const bool applyHot = hover_ == kIdUpscalerApply && !locked;
            FillRound(g, apply, S(8), applyHot ? col::Accent2() : col::Accent());
            StrokeRound(g, apply, S(8), col::Line(), max(1.0f, S(1)));
            Text(g, L"Apply", *f_.chip, apply, col::White(), 1);
            if (!locked)
                AddHit(apply, kIdUpscalerApply);
        }

        // ---- "Run the upscaler on the picture" (sharpening + CAS + native upscaler pass) ----
        {
            Text(g, L"Run the upscaler on the picture", *f_.bodyB, R(560, 508, 330, 24), col::Text());

            const gp::RectF onB = R(560, 534, 110, 30);
            const bool hotOn = hover_ == kIdPictureOn && !locked;
            FillRound(g, onB, S(8), hotOn ? col::Accent2() : col::Accent());
            StrokeRound(g, onB, S(8), col::Line(), max(1.0f, S(1)));
            Text(g, L"Apply", *f_.chip, onB, col::White(), 1);
            if (!locked)
                AddHit(onB, kIdPictureOn);

            const gp::RectF offB = R(678, 534, 110, 30);
            const bool hotOff = hover_ == kIdPictureOff && !locked;
            FillRound(g, offB, S(8), hotOff ? col::RowHot() : col::Row());
            StrokeRound(g, offB, S(8), hotOff ? col::Accent() : col::Line(), max(1.0f, S(1)));
            Text(g, L"Off", *f_.chip, offB, col::Text(), 1);
            if (!locked)
                AddHit(offB, kIdPictureOff);

            Text(g, pictureOn_ ? L"On" : L"Off", *f_.bodyB, R(800, 538, 92, 22), pictureOn_ ? col::Green() : col::Muted(), 2);
        }

        float y = 602.0f;
        for (int i = 0; i < kScCount; ++i, y += 30.0f)
        {
            if (i > 0)
            {
                gp::Pen sep(col::Alpha(col::Line(), 150), max(1.0f, S(1)));
                g.DrawLine(&sep, S(560), S(y - 1), S(892), S(y - 1));
            }

            Text(g, kShortcutDefs[i].label, *f_.body, R(560, y + 1, 150, 18), col::Text());
            Text(g, kShortcutDefs[i].hint, *f_.tiny, R(560, y + 17, 176, 14), col::Muted());

            if (i == kScFgKey)
            {
                // ON / OFF badge: this key switches frame generation on and off
                const gp::RectF badge = R(680, y + 3, 52, 18);
                FillRound(g, badge, S(9), col::Alpha(col::Green(), 36));
                StrokeRound(g, badge, S(9), col::Alpha(col::Green(), 130), max(1.0f, S(1)));
                Text(g, L"ON/OFF", *f_.tiny, badge, col::Green(), 1);
            }

            Chip(g, R(742, y + 3, 150, 25), ChipText(i), kIdShortcut0 + i, capturing_ == i);
        }

        // Two more shortcuts that always work while the capture host runs (fixed, not changeable here).
        {
            gp::Pen sep(col::Alpha(col::Line(), 150), max(1.0f, S(1)));
            g.DrawLine(&sep, S(560), S(y + 1), S(892), S(y + 1));
            Text(g, L"Ctrl+Shift+F10", *f_.tiny, R(560, y + 3, 100, 13), col::Accent());
            Text(g, L"Focus the overlay and open the OptiScaler menu", *f_.tiny, R(660, y + 3, 240, 13), col::Muted());
            Text(g, L"Ctrl+Shift+F11", *f_.tiny, R(560, y + 16, 100, 13), col::Accent());
            Text(g, L"Switch mouse and keyboard: game / overlay", *f_.tiny, R(660, y + 16, 240, 13), col::Muted());
        }
    }

    // ---- footer ----
    {
        gp::Pen line(col::Line(), max(1.0f, S(1)));
        g.DrawLine(&line, S(24), S(822), S(912), S(822));

        Text(g, L"by", *f_.body, R(24, 838, 24, 22), col::Muted());
        Text(g, L"MK-JH", *f_.bodyB, R(44, 838, 120, 22), col::Accent());

        // Three clickable Telegram links.
        {
            const bool hotJH = hover_ == kIdLinkJH;
            DrawPlane(g, R(24, 864, 22, 22));
            Text(g, L"MK-JH", hotJH ? *f_.link : *f_.linkN,
                R(54, 862, 150, 26), hotJH ? col::Accent2() : col::Accent());
            AddHit(R(24, 862, 200, 26), kIdLinkJH);

            const bool hotApps = hover_ == kIdLinkApps;
            Text(g, L"MKJH_APPS", hotApps ? *f_.link : *f_.linkN,
                R(145, 862, 135, 26), hotApps ? col::Accent2() : col::Accent());
            AddHit(R(140, 862, 140, 26), kIdLinkApps);

            const bool hotArchive = hover_ == kIdLinkArchive;
            Text(g, L"MK-JH_ARCHIVE", hotArchive ? *f_.link : *f_.linkN,
                R(290, 862, 180, 26), hotArchive ? col::Accent2() : col::Accent());
            AddHit(R(285, 862, 190, 26), kIdLinkArchive);
        }

        // status line
        std::wstring status;
        gp::Color tone = col::Muted();
        if (!note_.empty())
        {
            status = note_;
            tone = noteKind_ == Note::Error ? col::Red() : (noteKind_ == Note::Ok ? col::Green() : col::Muted());
        }
        else if (running)
        {
            status = L"Capturing \"" + targetTitle_ + L"\". " + KeyName(st_.menuKey) + L" opens the OptiScaler menu, " +
                     HotkeyText(st_.launcher) + L" shows this window.";
        }
        else if (selected_ >= 0 && selected_ < static_cast<int>(games_.size()))
        {
            status = L"Ready: " + games_[selected_].title;
        }
        else
        {
            status = L"Pick a game from the list to begin.";
        }
        Text(g, status, *f_.sm, R(300, 836, 340, 50), tone, 0, true);

        const bool noPick = !running && (selected_ < 0 || selected_ >= static_cast<int>(games_.size()));
        Button(g, R(672, 839, 220, 44), running ? (stopping_ ? L"STOPPING\u2026" : L"STOP") : L"START", noPick, running);
    }

    // ---- warning dialog (on top of everything) ----
    if (dlgKind_ >= 0)
        PaintDialog(g, W, H);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Helper mode: the launcher started itself elevated only to write the RTSS profile.
    {
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv && argc >= 11 && wcscmp(argv[1], L"--borderless") == 0)
        {
            BorderState b;
            const bool on = wcscmp(argv[2], L"on") == 0;
            b.wnd = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(_wcstoui64(argv[3], nullptr, 10)));
            b.style = static_cast<LONG_PTR>(_wcstoi64(argv[4], nullptr, 10));
            b.exStyle = static_cast<LONG_PTR>(_wcstoi64(argv[5], nullptr, 10));
            b.rect.left = _wtoi(argv[6]);
            b.rect.top = _wtoi(argv[7]);
            b.rect.right = _wtoi(argv[8]);
            b.rect.bottom = _wtoi(argv[9]);
            b.wasMaximized = _wtoi(argv[10]) != 0;
            LocalFree(argv);
            if (!IsWindow(b.wnd))
                return 1;
            return (on ? BorderApplyOn(b.wnd, b) : BorderApplyOff(b.wnd, b)) ? 0 : 1;
        }
        if (argv && argc >= 4 && wcscmp(argv[1], L"--rtss-apply") == 0)
        {
            const std::wstring exe = wcscmp(argv[2], L"-") == 0 ? std::wstring() : std::wstring(argv[2]);
            const int hz = _wtoi(argv[3]);
            const bool offArg = argc >= 5 && wcscmp(argv[4], L"off") == 0;
            LocalFree(argv);

            oc::Settings st;
            oc::LoadSettings(st);
            RtssApi api;
            const LimiterOut r = DoLimiter(api, st, exe, hz > 1 ? hz : 60, offArg);
            WritePrivateProfileStringW(L"Rtss", L"LastInfo", r.info.c_str(), oc::SettingsPath().c_str());
            WritePrivateProfileStringW(L"Rtss", L"LastNote", r.note.c_str(), oc::SettingsPath().c_str());
            return r.code;
        }
        if (argv)
            LocalFree(argv);
    }

    // One launcher at a time: a second start brings the first one forward.
    HANDLE once = CreateMutexW(nullptr, TRUE, L"Local\\OptiCaptureLauncher.SingleInstance");
    if (once && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (HWND other = FindWindowW(kClassName, nullptr))
        {
            if (IsIconic(other))
                ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        CloseHandle(once);
        return 0;
    }

    gp::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (gp::GdiplusStartup(&token, &input, nullptr) != gp::Ok)
    {
        MessageBoxW(nullptr, L"Could not start GDI+.", L"OptiCaptureHost", MB_OK | MB_ICONERROR);
        return 1;
    }

    int code = 0;
    {
        App app;
        if (app.Create(inst))
            code = app.Loop();
        else
            MessageBoxW(nullptr, L"Could not create the window.", L"OptiCaptureHost", MB_OK | MB_ICONERROR);
    }

    gp::GdiplusShutdown(token);
    if (once)
        CloseHandle(once);
    return code;
}
