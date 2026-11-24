// ==WindhawkMod==
// @id              genie-minimize-animation
// @name            Genie Minimize Animation
// @description     Windows pour into their taskbar button like a genie into a lamp when you minimize them
// @version         1.0.1
// @author          penicius-code
// @github          https://github.com/penicius-code
// @include         *
// @exclude         TextInputHost.exe
// @exclude         ShellExperienceHost.exe
// @exclude         StartMenuExperienceHost.exe
// @exclude         SearchHost.exe
// @exclude         dwm.exe
// @exclude         mmc.exe
// @exclude         git.exe
// @exclude         git-remote-https.exe
// @exclude         nvidia-smi.exe
// @exclude         reg.exe
// @exclude         AudioCaptureService.exe
// @exclude         NVIDIA Web Helper.exe
// @compilerOptions -ldwmapi -lgdi32 -luser32 -lshell32 -lole32 -loleaut32 -luuid
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Genie Minimize Animation

When you minimize a window it bends and pours into its own taskbar button,
the classic "genie" effect. Extracted from the Genie effect of the
*Windows Animations* mod by ReDrag (MIT), minimize only. The genie maths there
goes back to *MacOS Minimize Animation* by Abdullah Masood (MIT).

* Works with the minimize button, the taskbar button, the window menu and
  apps that minimize themselves (browsers, Electron apps...).
* Finds the real taskbar button of the app (UI Automation). The first
  minimize of an app can take a fraction of a second longer while it looks
  the button up; after that it is remembered.
* Taskbar on any edge (bottom, top, left, right), auto-hide, multiple
  monitors, rounded Windows 11 corners.
* Restoring a window is left to Windows. Restoring while the genie is still
  running cancels it instantly.
* Win+D / Win+M are left to Windows.
* Fullscreen windows (games, F11 browsers, fullscreen video) are never
  animated.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- duration_ms: 450
  $name: Animation duration (ms)
  $description: How long the genie takes, 150 to 1500
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <windhawk_utils.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using Microsoft::WRL::ComPtr;

////////////////////////////////////////////////////////////////////////////////
// Settings and global state

std::atomic<int> g_durationMs{450};
std::atomic<bool> g_unloading{false};

// Private message: probe (proves the window procedure forwards unknown
// messages to DefWindowProc) and minimize request (lets explorer.exe hand a
// taskbar minimize to the window's own process, which owns the animation).
UINT g_msgGenie = 0;
constexpr LPARAM kProbe = 0x47454E31;
constexpr LPARAM kMinimizeRequest = 0x47454E32;

constexpr PCWSTR kPropProtocol = L"GenieMinimize.ProtocolV1";
constexpr PCWSTR kPropProbePending = L"GenieMinimize.ProbePendingV1";

// Genie constants (from Windows Animations).
constexpr float kSpread = 0.65f;
constexpr DWORD kFirstFrameWaitMs = 500;
constexpr DWORD kUiaWaitMs = 400;
constexpr DWORD kDockCacheFreshMs = 3000;

// Windows with a genie in flight (process-local).
std::mutex g_stateMutex;
std::unordered_set<HWND> g_active;

// Every worker is tracked so unloading can join them before the DLL goes away.
std::mutex g_workerMutex;
std::vector<HANDLE> g_workers;

using ShowWindow_t = decltype(&ShowWindow);
using ShowWindowAsync_t = decltype(&ShowWindowAsync);
using CloseWindow_t = decltype(&CloseWindow);
using DefWindowProcW_t = decltype(&DefWindowProcW);

ShowWindow_t ShowWindow_Original;
ShowWindowAsync_t ShowWindowAsync_Original;
CloseWindow_t CloseWindow_Original;
DefWindowProcW_t DefWindowProcW_Original;

////////////////////////////////////////////////////////////////////////////////
// Small helpers

class PerMonitorDpiScope {
    DPI_AWARENESS_CONTEXT previous_ = nullptr;

public:
    PerMonitorDpiScope() {
        previous_ = SetThreadDpiAwarenessContext(
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    ~PerMonitorDpiScope() {
        if (previous_) SetThreadDpiAwarenessContext(previous_);
    }
};

static void SetCloak(HWND hWnd, BOOL cloak) {
    DwmSetWindowAttribute(hWnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
}

static void SetTransitions(HWND hWnd, BOOL enable) {
    BOOL disable = !enable;
    DwmSetWindowAttribute(hWnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disable,
                          sizeof(disable));
}

static bool IsCloaked(HWND hWnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hWnd, DWMWA_CLOAKED, &cloaked,
                                           sizeof(cloaked))) &&
           cloaked;
}

static bool IsOurWindow(HWND hWnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);
    return pid == GetCurrentProcessId();
}

static bool IsMinimizeCmd(int cmd) {
    return cmd == SW_MINIMIZE || cmd == SW_SHOWMINIMIZED ||
           cmd == SW_SHOWMINNOACTIVE;
}

// Waits for an event while still answering messages other threads SEND to
// this (UI) thread, so nothing that talks to the app deadlocks. Posted input
// stays queued.
static bool WaitPumpingSent(HANDLE handle, DWORD timeoutMs) {
    if (!handle) return false;
    const DWORD start = GetTickCount();
    for (;;) {
        const DWORD elapsed = GetTickCount() - start;
        if (elapsed >= timeoutMs) return false;
        const DWORD result = MsgWaitForMultipleObjectsEx(
            1, &handle, timeoutMs - elapsed, QS_SENDMESSAGE,
            MWMO_INPUTAVAILABLE);
        if (result == WAIT_OBJECT_0) return true;
        if (result != WAIT_OBJECT_0 + 1) return false;
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
    }
}

static bool SpawnWorker(LPTHREAD_START_ROUTINE proc, void* param) {
    std::lock_guard<std::mutex> lock(g_workerMutex);
    if (g_unloading.load()) return false;
    for (size_t i = g_workers.size(); i-- > 0;) {
        if (WaitForSingleObject(g_workers[i], 0) == WAIT_OBJECT_0) {
            CloseHandle(g_workers[i]);
            g_workers.erase(g_workers.begin() + i);
        }
    }
    HANDLE thread = CreateThread(nullptr, 0, proc, param, 0, nullptr);
    if (!thread) return false;
    g_workers.push_back(thread);
    return true;
}

static bool IsWinKeyDown() {
    return (GetAsyncKeyState(VK_LWIN) & 0x8000) ||
           (GetAsyncKeyState(VK_RWIN) & 0x8000);
}

////////////////////////////////////////////////////////////////////////////////
// Which windows get the effect

static bool IsExplorerProcess() {
    static const bool isExplorer = [] {
        WCHAR path[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, path, ARRAYSIZE(path))) return false;
        const WCHAR* name = wcsrchr(path, L'\\');
        return _wcsicmp(name ? name + 1 : path, L"explorer.exe") == 0;
    }();
    return isExplorer;
}

// Lowercase executable name without extension ("chrome", "explorer"...).
static const std::wstring& OwnProcessName() {
    static const std::wstring name = [] {
        WCHAR path[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, path, ARRAYSIZE(path))) {
            return std::wstring();
        }
        const WCHAR* base = wcsrchr(path, L'\\');
        std::wstring n = base ? base + 1 : path;
        const size_t dot = n.rfind(L'.');
        if (dot != std::wstring::npos) n.resize(dot);
        for (auto& c : n) c = static_cast<wchar_t>(towlower(c));
        return n;
    }();
    return name;
}

static bool IsExcludedClass(const WCHAR* cls) {
    static const WCHAR* const kClasses[] = {
        L"Xaml_WindowedPopupClass",
        L"Popup",
        L"Overlay",
        L"ToolTip",
        L"tooltips_class32",
        L"#32768",
        L"CoreWindow",
        L"ApplicationFrameWindow",
        L"XamlExplorerHostIslandWindow",
        L"Windows.Internal.Shell.TabProxyWindow",
    };
    for (const WCHAR* c : kClasses) {
        if (_wcsicmp(cls, c) == 0) return true;
    }
    // Inside explorer.exe only real File Explorer windows; the desktop,
    // taskbar and shell surfaces must never be animated.
    if (IsExplorerProcess() && _wcsicmp(cls, L"CabinetWClass") != 0 &&
        _wcsicmp(cls, L"ExploreWClass") != 0) {
        return true;
    }
    return false;
}

// Fullscreen games, F11 browsers and fullscreen video are never animated: the
// snapshot would run on the game's window thread (a full-resolution copy) and
// a topmost ghost would sit over its swap chain at alt-tab. Fullscreen = covers
// its whole monitor and is not a maximized normal (captioned) window, or the
// foreground window while Windows reports exclusive D3D fullscreen.
static bool IsFullscreenWindow(HWND hWnd) {
    const LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
    if (!((style & WS_CAPTION) == WS_CAPTION && IsZoomed(hWnd))) {
        RECT r;
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetWindowRect(hWnd, &r) &&
            GetMonitorInfoW(MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST),
                            &mi) &&
            r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
            r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom) {
            return true;
        }
    }
    QUERY_USER_NOTIFICATION_STATE state;
    return hWnd == GetForegroundWindow() &&
           SUCCEEDED(SHQueryUserNotificationState(&state)) &&
           (state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_BUSY ||
            state == QUNS_PRESENTATION_MODE);
}

// A visible, top-level app window of this process, at least 100x100.
static bool IsMinimizeCandidate(HWND hWnd) {
    if (g_unloading.load() || !hWnd || !IsWindow(hWnd) ||
        !IsWindowVisible(hWnd) || IsIconic(hWnd) ||
        GetAncestor(hWnd, GA_ROOT) != hWnd || !IsOurWindow(hWnd)) {
        return false;
    }
    const LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
    const LONG_PTR exStyle = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);
    if ((style & WS_CHILD) || (exStyle & WS_EX_TOOLWINDOW)) return false;
    if (GetWindow(hWnd, GW_OWNER) && !(exStyle & WS_EX_APPWINDOW)) {
        return false;
    }

    WCHAR cls[128];
    if (!GetClassNameW(hWnd, cls, ARRAYSIZE(cls)) || IsExcludedClass(cls)) {
        return false;
    }
    if (IsCloaked(hWnd)) return false;

    RECT r;
    return GetWindowRect(hWnd, &r) && r.right - r.left >= 100 &&
           r.bottom - r.top >= 100 && !IsFullscreenWindow(hWnd);
}

////////////////////////////////////////////////////////////////////////////////
// Snapshot (same capture as the Thanos mod)

struct Snapshot {
    RECT rect{};  // physical screen coordinates (DWM extended frame)
    int w = 0;
    int h = 0;
    std::vector<uint32_t> px;  // premultiplied BGRA, top-down
};

static HBITMAP CreateDib32(HDC dc, int width, int height, void** bits) {
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, bits, nullptr, 0);
}

// The composed screen is only a faithful picture of the window if nothing
// visible sits above it. Must run with per-monitor DPI awareness.
static bool CanCaptureFromScreen(HWND hWnd, const RECT& rc) {
    int budget = 512;
    for (HWND c = GetWindow(hWnd, GW_HWNDPREV); c;
         c = GetWindow(c, GW_HWNDPREV)) {
        if (--budget <= 0) return false;
        if (!IsWindowVisible(c) || IsIconic(c) || IsCloaked(c)) continue;

        const LONG_PTR ex = GetWindowLongPtrW(c, GWL_EXSTYLE);
        if (ex & WS_EX_LAYERED) {
            COLORREF key = 0;
            BYTE alpha = 255;
            DWORD flags = 0;
            if (GetLayeredWindowAttributes(c, &key, &alpha, &flags) &&
                (flags & LWA_ALPHA) && alpha == 0) {
                continue;  // fully transparent
            }
            // Explorer's invisible XAML host island.
            constexpr LONG_PTR kIsland = WS_EX_LAYERED |
                                         WS_EX_NOREDIRECTIONBITMAP |
                                         WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT;
            WCHAR cls[64];
            if ((ex & kIsland) == kIsland &&
                GetClassNameW(c, cls, ARRAYSIZE(cls)) &&
                _wcsicmp(cls, L"XamlExplorerHostIslandWindow") == 0) {
                continue;
            }
        }

        RECT cr;
        if (FAILED(DwmGetWindowAttribute(c, DWMWA_EXTENDED_FRAME_BOUNDS, &cr,
                                         sizeof(cr))) &&
            !GetWindowRect(c, &cr)) {
            continue;
        }
        RECT overlap;
        if (IntersectRect(&overlap, &rc, &cr)) return false;
    }
    return true;
}

// Windows 11 rounds window corners; the snapshot is a rectangle, so clip the
// same corners off.
static int GetCornerRadius(HWND hWnd) {
    if (IsZoomed(hWnd)) return 0;
    int preference = 0;  // DWMWCP_DEFAULT
    if (FAILED(DwmGetWindowAttribute(hWnd, 33 /* WINDOW_CORNER_PREFERENCE */,
                                     &preference, sizeof(preference)))) {
        return 0;  // Windows 10: no rounding
    }
    if (preference == 1) return 0;  // DONOTROUND
    if (preference == 0) {
        const LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
        const LONG_PTR ex = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);
        if (!(style & WS_CAPTION) || (ex & WS_EX_LAYERED)) return 0;
    }
    UINT dpi = GetDpiForWindow(hWnd);
    if (!dpi) dpi = 96;
    return MulDiv(preference == 3 ? 4 : 8, static_cast<int>(dpi), 96);
}

static void ApplyRoundedCorners(Snapshot* s, int radius) {
    radius = std::min(radius, std::min(s->w, s->h) / 2);
    if (radius <= 0) return;

    auto limit = [](uint32_t& p, uint32_t maskAlpha) {
        const uint32_t a = p >> 24;
        if (maskAlpha >= a) return;
        if (!maskAlpha || !a) {
            p = 0;
            return;
        }
        auto scale = [&](uint32_t c) { return (c * maskAlpha + a / 2) / a; };
        p = (maskAlpha << 24) | (scale((p >> 16) & 0xFF) << 16) |
            (scale((p >> 8) & 0xFF) << 8) | scale(p & 0xFF);
    };

    constexpr int kSamples = 4;
    const float r = static_cast<float>(radius);
    for (int y = 0; y < radius; ++y) {
        for (int x = 0; x < radius; ++x) {
            int inside = 0;
            for (int sy = 0; sy < kSamples; ++sy) {
                const float dy = y + (sy + 0.5f) / kSamples - r;
                for (int sx = 0; sx < kSamples; ++sx) {
                    const float dx = x + (sx + 0.5f) / kSamples - r;
                    if (dx * dx + dy * dy <= r * r) ++inside;
                }
            }
            const uint32_t mask =
                (inside * 255 + kSamples * kSamples / 2) / (kSamples * kSamples);
            const int w = s->w, h = s->h;
            limit(s->px[static_cast<size_t>(y) * w + x], mask);
            limit(s->px[static_cast<size_t>(y) * w + (w - 1 - x)], mask);
            limit(s->px[static_cast<size_t>(h - 1 - y) * w + x], mask);
            limit(s->px[static_cast<size_t>(h - 1 - y) * w + (w - 1 - x)], mask);
        }
    }
}

// Prefer the composed screen (exact look, including acrylic/blur). Fall back
// to PrintWindow, but only on the window's own thread - PrintWindow from
// another thread is a synchronous cross-thread call that can deadlock.
static bool CaptureWindow(HWND hWnd, Snapshot* s) {
    PerMonitorDpiScope dpi;

    RECT wr;
    if (!GetWindowRect(hWnd, &wr)) return false;
    RECT er = wr;
    DwmGetWindowAttribute(hWnd, DWMWA_EXTENDED_FRAME_BOUNDS, &er, sizeof(er));

    const int w = er.right - er.left;
    const int h = er.bottom - er.top;
    const int rawW = wr.right - wr.left;
    const int rawH = wr.bottom - wr.top;
    if (w <= 0 || h <= 0 || rawW <= 0 || rawH <= 0) return false;

    s->rect = er;
    s->w = w;
    s->h = h;
    s->px.assign(static_cast<size_t>(w) * h, 0);

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    bool ok = false;

    if (CanCaptureFromScreen(hWnd, er)) {
        void* bits = nullptr;
        HBITMAP bmp = CreateDib32(screen, w, h, &bits);
        if (bmp && bits) {
            HGDIOBJ old = SelectObject(mem, bmp);
            if (BitBlt(mem, 0, 0, w, h, screen, er.left, er.top, SRCCOPY)) {
                GdiFlush();
                // Re-check: something may have moved on top during the grab.
                if (CanCaptureFromScreen(hWnd, er)) {
                    const uint32_t* src = static_cast<const uint32_t*>(bits);
                    for (size_t i = 0; i < s->px.size(); ++i) {
                        s->px[i] = src[i] | 0xFF000000u;
                    }
                    ok = true;
                }
            }
            SelectObject(mem, old);
        }
        if (bmp) DeleteObject(bmp);
    }

    if (!ok && GetWindowThreadProcessId(hWnd, nullptr) == GetCurrentThreadId()) {
        void* bits = nullptr;
        HBITMAP bmp = CreateDib32(screen, rawW, rawH, &bits);
        if (bmp && bits) {
            HGDIOBJ old = SelectObject(mem, bmp);
            if (PrintWindow(hWnd, mem, PW_RENDERFULLCONTENT)) {
                GdiFlush();
                const uint32_t* src = static_cast<const uint32_t*>(bits);
                const int offX = er.left - wr.left;
                const int offY = er.top - wr.top;
                size_t inBounds = 0, zeroAlpha = 0;
                for (int y = 0; y < h; ++y) {
                    const int sy = y + offY;
                    if (sy < 0 || sy >= rawH) continue;
                    for (int x = 0; x < w; ++x) {
                        const int sx = x + offX;
                        if (sx < 0 || sx >= rawW) continue;
                        const uint32_t p =
                            src[static_cast<size_t>(sy) * rawW + sx];
                        ++inBounds;
                        if ((p >> 24) == 0) {
                            ++zeroAlpha;
                            continue;
                        }
                        s->px[static_cast<size_t>(y) * w + x] = p;
                    }
                }
                // Mostly transparent = the alpha channel isn't real (common on
                // Mica/acrylic): use the colors opaque instead of a hole.
                if (inBounds && zeroAlpha * 2 > inBounds) {
                    for (int y = 0; y < h; ++y) {
                        const int sy = y + offY;
                        if (sy < 0 || sy >= rawH) continue;
                        for (int x = 0; x < w; ++x) {
                            const int sx = x + offX;
                            if (sx < 0 || sx >= rawW) continue;
                            const uint32_t p =
                                src[static_cast<size_t>(sy) * rawW + sx];
                            if ((p >> 24) == 0) {
                                s->px[static_cast<size_t>(y) * w + x] =
                                    p | 0xFF000000u;
                            }
                        }
                    }
                }
                ok = true;
            }
            SelectObject(mem, old);
        }
        if (bmp) DeleteObject(bmp);
    }

    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    if (ok) ApplyRoundedCorners(s, GetCornerRadius(hWnd));
    return ok;
}

////////////////////////////////////////////////////////////////////////////////
// Taskbar button location (UI Automation, cached)

enum class TaskbarEdge { Bottom, Top, Left, Right };

static bool IsHorizontal(TaskbarEdge e) {
    return e == TaskbarEdge::Bottom || e == TaskbarEdge::Top;
}

static HWND FindTaskbarForMonitor(HMONITOR mon) {
    HWND mainTray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!mon || !mainTray ||
        MonitorFromWindow(mainTray, MONITOR_DEFAULTTOPRIMARY) == mon) {
        return mainTray;
    }
    HWND sec = nullptr;
    while ((sec = FindWindowExW(nullptr, sec, L"Shell_SecondaryTrayWnd",
                                nullptr)) != nullptr) {
        if (MonitorFromWindow(sec, MONITOR_DEFAULTTONULL) == mon) return sec;
    }
    return mainTray;
}

static TaskbarEdge DetectEdge(const RECT& tr, const RECT& mon) {
    const int w = tr.right - tr.left;
    const int h = tr.bottom - tr.top;
    if (w >= h) {
        return tr.top + h / 2 <= mon.top + (mon.bottom - mon.top) / 2
                   ? TaskbarEdge::Top
                   : TaskbarEdge::Bottom;
    }
    return tr.left + w / 2 <= mon.left + (mon.right - mon.left) / 2
               ? TaskbarEdge::Left
               : TaskbarEdge::Right;
}

struct DockCacheEntry {
    POINT center{};
    HMONITOR mon = nullptr;
    DWORD tick = 0;
};

std::mutex g_dockMutex;
std::unordered_map<HWND, DockCacheEntry> g_dockCache;
// Last button found for this process per monitor: windows of one app usually
// share one (grouped) taskbar button, so it is a good first guess.
std::unordered_map<HMONITOR, DockCacheEntry> g_processDockCache;
std::unordered_set<HWND> g_dockInFlight;

struct UiaRequest {
    HWND hWnd = nullptr;
    HMONITOR mon = nullptr;
    HWND tray = nullptr;
    RECT trayRect{};
    bool horizontal = true;
    std::wstring title;
    std::wstring proc;
    std::wstring hint;
    HANDLE done = nullptr;
    bool found = false;
    POINT center{};
    ~UiaRequest() {
        if (done) CloseHandle(done);
    }
};

static std::wstring ProcHint(const std::wstring& proc) {
    if (proc == L"chrome") return L"google chrome";
    if (proc == L"msedge") return L"microsoft edge";
    if (proc == L"code") return L"visual studio code";
    if (proc == L"devenv") return L"visual studio";
    if (proc == L"windowsterminal") return L"terminal";
    if (proc == L"explorer") return L"file explorer";
    return proc;
}

static int ScoreName(const UiaRequest& r, const std::wstring& name) {
    int score = 0;
    if (!r.title.empty()) {
        if (r.title == name) score += 1000;
        if (r.title.find(name) != std::wstring::npos) score += 500;
        if (name.find(r.title) != std::wstring::npos) score += 500;
    }
    if (!r.proc.empty() && name.find(r.proc) != std::wstring::npos) {
        score += 400;
    }
    if (!r.hint.empty() && r.hint != r.proc &&
        name.find(r.hint) != std::wstring::npos) {
        score += 900;
    }
    std::wstring word;
    auto scoreWord = [&] {
        if (word.length() >= 4 && name.find(word) != std::wstring::npos) {
            score += 50;
        }
        word.clear();
    };
    for (wchar_t c : r.title) {
        if (iswalnum(c)) {
            word += c;
        } else {
            scoreWord();
        }
    }
    scoreWord();
    for (PCWSTR bad : {L"start", L"search", L"task view", L"widgets"}) {
        if (name.find(bad) != std::wstring::npos) score -= 500;
    }
    return score;
}

static bool FindTaskbarButton(const UiaRequest& r, POINT* out) {
    ComPtr<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(automation.ReleaseAndGetAddressOf()))) &&
        FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(automation.ReleaseAndGetAddressOf())))) {
        return false;
    }
    ComPtr<IUIAutomation2> automation2;
    if (SUCCEEDED(automation.As(&automation2))) {
        automation2->put_ConnectionTimeout(1000);
        automation2->put_TransactionTimeout(2000);
    }

    ComPtr<IUIAutomationElement> trayElement;
    if (FAILED(automation->ElementFromHandle(r.tray, trayElement.GetAddressOf())) ||
        !trayElement) {
        return false;
    }

    VARIANT button{};
    button.vt = VT_I4;
    button.lVal = UIA_ButtonControlTypeId;
    VARIANT listItem{};
    listItem.vt = VT_I4;
    listItem.lVal = UIA_ListItemControlTypeId;
    ComPtr<IUIAutomationCondition> buttonCond, listItemCond, orCond;
    if (FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId,
                                                   button, buttonCond.GetAddressOf())) ||
        FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId,
                                                   listItem, listItemCond.GetAddressOf())) ||
        FAILED(automation->CreateOrCondition(buttonCond.Get(),
                                             listItemCond.Get(), orCond.GetAddressOf()))) {
        return false;
    }

    ComPtr<IUIAutomationElementArray> items;
    if (FAILED(trayElement->FindAll(TreeScope_Descendants, orCond.Get(),
                                    items.GetAddressOf())) ||
        !items) {
        return false;
    }

    int length = 0;
    items->get_Length(&length);
    int bestScore = 399;  // minimum accepted score is 400
    bool found = false;
    for (int i = 0; i < length && !g_unloading.load(); ++i) {
        ComPtr<IUIAutomationElement> item;
        if (FAILED(items->GetElement(i, item.GetAddressOf())) || !item) continue;

        BSTR rawName = nullptr;
        if (FAILED(item->get_CurrentName(&rawName)) || !rawName) continue;
        std::wstring name = rawName;
        SysFreeString(rawName);
        if (name.empty()) continue;
        for (auto& c : name) c = static_cast<wchar_t>(towlower(c));

        int score = ScoreName(r, name);
        // Prefer real task-list buttons over a same-named tray icon.
        BSTR rawClass = nullptr;
        if (SUCCEEDED(item->get_CurrentClassName(&rawClass)) && rawClass) {
            if (wcsstr(rawClass, L"TaskList")) score += 300;
            SysFreeString(rawClass);
        }
        if (score <= bestScore) continue;

        RECT br{};
        if (FAILED(item->get_CurrentBoundingRectangle(&br)) ||
            br.right <= br.left || br.bottom <= br.top) {
            continue;
        }
        const POINT c{br.left + (br.right - br.left) / 2,
                      br.top + (br.bottom - br.top) / 2};
        const bool inside =
            r.horizontal
                ? c.x >= r.trayRect.left && c.x < r.trayRect.right - 50
                : c.y >= r.trayRect.top && c.y < r.trayRect.bottom - 50;
        if (!inside) continue;

        bestScore = score;
        *out = c;
        found = true;
    }
    return found;
}

static DWORD WINAPI UiaThread(LPVOID param) {
    std::unique_ptr<std::shared_ptr<UiaRequest>> holder(
        static_cast<std::shared_ptr<UiaRequest>*>(param));
    UiaRequest& r = **holder;
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    POINT center{};
    bool found = false;
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) {
        try {
            found = FindTaskbarButton(r, &center);
        } catch (...) {
            found = false;
        }
    }
    if (SUCCEEDED(hr)) CoUninitialize();

    {
        std::lock_guard<std::mutex> lock(g_dockMutex);
        g_dockInFlight.erase(r.hWnd);
        if (found && !g_unloading.load() && IsWindow(r.hWnd)) {
            const DockCacheEntry entry{center, r.mon, GetTickCount()};
            g_dockCache[r.hWnd] = entry;
            g_processDockCache[r.mon] = entry;
        }
    }
    if (!found) Wh_Log(L"Taskbar button not found hwnd=%p", r.hWnd);

    r.found = found;
    r.center = center;
    SetEvent(r.done);
    return 0;
}

// Center of the window's taskbar button (physical pixels). A remembered
// position is used right away (and refreshed in the background); only the
// very first lookup waits for UI Automation, and only briefly.
static bool LookupTaskbarButton(HWND hWnd, HMONITOR mon, HWND tray,
                                const RECT& trayRect, bool horizontal,
                                POINT* out) {
    const DWORD now = GetTickCount();
    bool haveLearned = false;
    POINT learned{};
    bool inFlight = false;
    {
        std::lock_guard<std::mutex> lock(g_dockMutex);
        auto it = g_dockCache.find(hWnd);
        if (it != g_dockCache.end() && it->second.mon == mon) {
            if (now - it->second.tick < kDockCacheFreshMs) {
                *out = it->second.center;
                return true;
            }
            learned = it->second.center;
            haveLearned = true;
        } else {
            auto pit = g_processDockCache.find(mon);
            if (pit != g_processDockCache.end()) {
                learned = pit->second.center;
                haveLearned = true;
            }
        }
        inFlight = g_dockInFlight.count(hWnd) != 0;
        if (!inFlight) g_dockInFlight.insert(hWnd);
    }

    std::shared_ptr<UiaRequest> request;
    if (!inFlight) {
        auto unmark = [&] {
            std::lock_guard<std::mutex> lock(g_dockMutex);
            g_dockInFlight.erase(hWnd);
        };
        try {
            request = std::make_shared<UiaRequest>();
            request->hWnd = hWnd;
            request->mon = mon;
            request->tray = tray;
            request->trayRect = trayRect;
            request->horizontal = horizontal;
            request->proc = OwnProcessName();
            request->hint = ProcHint(request->proc);
            // GetWindowText from another thread is a synchronous cross-thread
            // call; the title is only a hint, so skip it there.
            if (GetWindowThreadProcessId(hWnd, nullptr) ==
                GetCurrentThreadId()) {
                WCHAR title[256]{};
                GetWindowTextW(hWnd, title, ARRAYSIZE(title));
                request->title = title;
                for (auto& c : request->title) {
                    c = static_cast<wchar_t>(towlower(c));
                }
            }
            request->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        } catch (...) {
            request.reset();
        }
        auto* param = request && request->done
                          ? new (std::nothrow)
                                std::shared_ptr<UiaRequest>(request)
                          : nullptr;
        if (!param || !SpawnWorker(UiaThread, param)) {
            delete param;
            request.reset();
            unmark();
        }
    }

    if (haveLearned) {
        *out = learned;
        return true;
    }
    if (request && WaitPumpingSent(request->done, kUiaWaitMs) &&
        request->found) {
        *out = request->center;
        return true;
    }
    return false;
}

struct DockTarget {
    TaskbarEdge edge = TaskbarEdge::Bottom;
    float x = 0.0f;  // physical screen point on the taskbar's inner edge
    float y = 0.0f;
    HWND tray = nullptr;
};

// Must run with per-monitor DPI awareness.
static DockTarget ComputeDockTarget(HWND hWnd, HMONITOR mon) {
    DockTarget d;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) {
        mi.rcMonitor = {0, 0, GetSystemMetrics(SM_CXSCREEN),
                        GetSystemMetrics(SM_CYSCREEN)};
    }
    const RECT& m = mi.rcMonitor;

    HWND tray = FindTaskbarForMonitor(mon);
    RECT tr{};
    const bool haveTray = tray && GetWindowRect(tray, &tr) &&
                          tr.right > tr.left && tr.bottom > tr.top;
    d.tray = haveTray ? tray : nullptr;
    d.edge = haveTray ? DetectEdge(tr, m) : TaskbarEdge::Bottom;

    // The visible part of the taskbar (a thin strip when auto-hidden).
    RECT vis{};
    const bool visible = haveTray && IntersectRect(&vis, &tr, &m);
    float inner = 0.0f;
    switch (d.edge) {
        case TaskbarEdge::Bottom:
            inner = static_cast<float>(visible ? vis.top : m.bottom);
            break;
        case TaskbarEdge::Top:
            inner = static_cast<float>(visible ? vis.bottom : m.top);
            break;
        case TaskbarEdge::Left:
            inner = static_cast<float>(visible ? vis.right : m.left);
            break;
        case TaskbarEdge::Right:
            inner = static_cast<float>(visible ? vis.left : m.right);
            break;
    }

    const bool horizontal = IsHorizontal(d.edge);
    UINT dpi = haveTray ? GetDpiForWindow(tray) : 96;
    if (!dpi) dpi = 96;
    const int leading = MulDiv(160, static_cast<int>(dpi), 96);

    // Fallback: where buttons usually start (left/centered per TaskbarAl).
    float axis;
    if (horizontal) {
        DWORD align = 1, size = sizeof(align);
        RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer"
                     L"\\Advanced",
                     L"TaskbarAl", RRF_RT_REG_DWORD, nullptr, &align, &size);
        axis = static_cast<float>(align == 0 ? m.left + leading
                                             : m.left + (m.right - m.left) / 2);
    } else {
        axis = static_cast<float>(m.top + leading);
    }

    POINT button{};
    if (haveTray &&
        LookupTaskbarButton(hWnd, mon, tray, tr, horizontal, &button)) {
        axis = static_cast<float>(horizontal ? button.x : button.y);
    }

    d.x = horizontal ? axis : inner;
    d.y = horizontal ? inner : axis;
    return d;
}

////////////////////////////////////////////////////////////////////////////////
// Genie rendering (CPU, from Windows Animations)

struct Canvas {
    uint32_t* px;
    int w;
    int h;
};

struct GenieGeometry {
    int W = 0, H = 0;
    float origLeft = 0, origTop = 0;
    float centerX = 0, centerY = 0;
    float dockX = 0, dockY = 0;
    float neckW = 0;
    TaskbarEdge edge = TaskbarEdge::Bottom;
    int boundLeft = 0, boundTop = 0;
};

static inline float MorphAt(float v, float tt) {
    float m = tt * (1.0f + kSpread) - (1.0f - v) * kSpread;
    m = std::clamp(m, 0.0f, 1.0f);
    return m * m * (3.0f - 2.0f * m);
}

// The edge nearest the taskbar is sucked in first; every row (or column for
// vertical taskbars) slides toward the dock and narrows into the neck.
static void RenderGenie(const Canvas& c, const Snapshot& s,
                        const GenieGeometry& g, float tt,
                        std::vector<float>& bounds) {
    const bool horizontal = IsHorizontal(g.edge);
    const bool leading =
        g.edge == TaskbarEdge::Top || g.edge == TaskbarEdge::Left;
    const int W = g.W, H = g.H;
    const int axisSize = horizontal ? H : W;
    const uint32_t* src = s.px.data();

    for (int k = 0; k <= axisSize; ++k) {
        const float axis = static_cast<float>(k) / axisSize;
        const float e = MorphAt(leading ? 1.0f - axis : axis, tt);
        const float sourceAxis =
            horizontal ? g.origTop + H * axis : g.origLeft + W * axis;
        const float dockAxis = horizontal ? g.dockY : g.dockX;
        bounds[k] = sourceAxis + (dockAxis - sourceAxis) * e;
    }

    int segment = 0;
    const int canvasAxisSize = horizontal ? c.h : c.w;
    const float axisOrigin =
        static_cast<float>(horizontal ? g.boundTop : g.boundLeft);
    const float crossOrigin =
        static_cast<float>(horizontal ? g.boundLeft : g.boundTop);
    const float crossSourceSize = static_cast<float>(horizontal ? W : H);
    const float sourceCenter = horizontal ? g.centerX : g.centerY;
    const float dockCenter = horizontal ? g.dockX : g.dockY;
    const int crossLimit = horizontal ? c.w : c.h;

    for (int ca = 0; ca < canvasAxisSize; ++ca) {
        const float screenAxis = ca + axisOrigin + 0.5f;
        if (screenAxis < bounds.front() || screenAxis >= bounds.back()) {
            continue;
        }
        while (segment < axisSize - 1 && bounds[segment + 1] <= screenAxis) {
            ++segment;
        }
        const float segSize = bounds[segment + 1] - bounds[segment];
        const float fraction =
            segSize > 1e-4f ? (screenAxis - bounds[segment]) / segSize : 0.0f;
        const float axis = (segment + fraction) / axisSize;
        const float morph = MorphAt(leading ? 1.0f - axis : axis, tt);
        const float crossSize = std::max(
            1.0f, crossSourceSize + (g.neckW - crossSourceSize) * morph);
        const float crossCenter =
            sourceCenter + (dockCenter - sourceCenter) * morph;
        const float crossStartF = crossCenter - crossSize * 0.5f - crossOrigin;
        const int crossStart = std::max(0, static_cast<int>(crossStartF));
        const int crossEnd = std::min(
            crossLimit, static_cast<int>(crossStartF + crossSize) + 1);
        const float inv = 1.0f / crossSize;

        if (horizontal) {
            const int sy = std::clamp(static_cast<int>(axis * H), 0, H - 1);
            const uint32_t* srcRow = src + static_cast<size_t>(sy) * W;
            uint32_t* dstRow = c.px + static_cast<size_t>(ca) * c.w;
            for (int x = crossStart; x < crossEnd; ++x) {
                const float u = (x + 0.5f - crossStartF) * inv;
                if (u < 0.0f || u >= 1.0f) continue;
                dstRow[x] =
                    srcRow[std::clamp(static_cast<int>(u * W), 0, W - 1)];
            }
        } else {
            const int sx = std::clamp(static_cast<int>(axis * W), 0, W - 1);
            for (int y = crossStart; y < crossEnd; ++y) {
                const float v = (y + 0.5f - crossStartF) * inv;
                if (v < 0.0f || v >= 1.0f) continue;
                const int sy = std::clamp(static_cast<int>(v * H), 0, H - 1);
                c.px[static_cast<size_t>(y) * c.w + ca] =
                    src[static_cast<size_t>(sy) * W + sx];
            }
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// Animation worker

enum class JobState { Pending, Minimized, Cancelled };

struct GenieJob {
    HWND hWnd = nullptr;
    Snapshot snap;
    DockTarget dock;
    int durationMs = 450;
    HANDLE firstFrame = nullptr;
    std::atomic<JobState> state{JobState::Pending};
    ~GenieJob() {
        if (firstFrame) CloseHandle(firstFrame);
    }
};

static void RunGenie(GenieJob& job) {
    const Snapshot& s = job.snap;
    const RECT& rc = s.rect;
    const DockTarget& d = job.dock;

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST), &mi)) {
        mi.rcMonitor = {GetSystemMetrics(SM_XVIRTUALSCREEN),
                        GetSystemMetrics(SM_YVIRTUALSCREEN),
                        GetSystemMetrics(SM_XVIRTUALSCREEN) +
                            GetSystemMetrics(SM_CXVIRTUALSCREEN),
                        GetSystemMetrics(SM_YVIRTUALSCREEN) +
                            GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    }
    const RECT& m = mi.rcMonitor;

    GenieGeometry g;
    g.W = s.w;
    g.H = s.h;
    g.origLeft = static_cast<float>(rc.left);
    g.origTop = static_cast<float>(rc.top);
    g.centerX = rc.left + s.w * 0.5f;
    g.centerY = rc.top + s.h * 0.5f;
    g.edge = d.edge;
    const bool horizontal = IsHorizontal(d.edge);
    g.dockX = std::clamp(d.x, static_cast<float>(m.left),
                         static_cast<float>(m.right));
    g.dockY = std::clamp(d.y, static_cast<float>(m.top),
                         static_cast<float>(m.bottom));
    g.neckW = std::clamp((horizontal ? s.w : s.h) * 0.03f, 12.0f, 60.0f);

    // Canvas: union of the window and the final neck, plus a little room on
    // the taskbar side, kept on the monitor.
    constexpr int kSafety = 2;
    constexpr int kEdgePad = 40;
    int left, top, right, bottom;
    if (horizontal) {
        const int nl = static_cast<int>(floorf(g.dockX - g.neckW * 0.5f));
        const int nr = static_cast<int>(ceilf(g.dockX + g.neckW * 0.5f));
        left = std::max<int>(m.left, std::min<int>(rc.left, nl) - kSafety);
        right = std::min<int>(m.right, std::max<int>(rc.right, nr) + kSafety);
        top = std::max<int>(
            m.top, std::min<int>(rc.top, static_cast<int>(floorf(g.dockY)) -
                                             (d.edge == TaskbarEdge::Top
                                                  ? kEdgePad
                                                  : 0)));
        bottom = std::min<int>(
            m.bottom,
            std::max<int>(rc.bottom, static_cast<int>(ceilf(g.dockY)) +
                                         (d.edge == TaskbarEdge::Bottom
                                              ? kEdgePad
                                              : 0)));
    } else {
        const int nt = static_cast<int>(floorf(g.dockY - g.neckW * 0.5f));
        const int nb = static_cast<int>(ceilf(g.dockY + g.neckW * 0.5f));
        top = std::max<int>(m.top, std::min<int>(rc.top, nt) - kSafety);
        bottom = std::min<int>(m.bottom, std::max<int>(rc.bottom, nb) + kSafety);
        left = std::max<int>(
            m.left, std::min<int>(rc.left, static_cast<int>(floorf(g.dockX)) -
                                               (d.edge == TaskbarEdge::Left
                                                    ? kEdgePad
                                                    : 0)));
        right = std::min<int>(
            m.right,
            std::max<int>(rc.right, static_cast<int>(ceilf(g.dockX)) +
                                        (d.edge == TaskbarEdge::Right
                                             ? kEdgePad
                                             : 0)));
    }
    const int cw = right - left;
    const int ch = bottom - top;
    if (cw <= 0 || ch <= 0) return;
    g.boundLeft = left;
    g.boundTop = top;

    std::vector<float> bounds;
    try {
        bounds.resize(static_cast<size_t>(horizontal ? s.h : s.w) + 1);
    } catch (...) {
        return;
    }

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP canvasBmp = CreateDib32(screen, cw, ch, &bits);
    HWND ghost = nullptr;

    if (mem && canvasBmp && bits) {
        HGDIOBJ oldBmp = SelectObject(mem, canvasBmp);
        ghost = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST |
                                    WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                                L"STATIC", nullptr, WS_POPUP, left, top, cw, ch,
                                nullptr, nullptr, nullptr, nullptr);
        if (ghost) {
            // Don't let the taskbar treat the ghost as a fullscreen app.
            SetPropW(ghost, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE));
            const Canvas canvas{static_cast<uint32_t*>(bits), cw, ch};
            const size_t canvasBytes = static_cast<size_t>(cw) * ch * 4;
            const double duration = std::max(1, job.durationMs);

            LARGE_INTEGER freq, start, now;
            QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&start);
            bool first = true;

            for (;;) {
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }

                if (!first) {
                    const JobState st = job.state.load();
                    // Minimize refused, window gone, or restored mid-genie.
                    if (st == JobState::Cancelled || !IsWindow(job.hWnd) ||
                        (st == JobState::Minimized && !IsIconic(job.hWnd))) {
                        break;
                    }
                }

                QueryPerformanceCounter(&now);
                const double elapsed =
                    (now.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
                const bool last = !first && elapsed >= duration;
                const float tt =
                    first ? 0.0f
                          : static_cast<float>(std::min(1.0, elapsed / duration));

                memset(canvas.px, 0, canvasBytes);
                RenderGenie(canvas, s, g, tt, bounds);
                const float fade =
                    tt > 0.8f ? std::clamp((1.0f - tt) / 0.2f, 0.0f, 1.0f) : 1.0f;

                POINT dst{left, top};
                SIZE size{cw, ch};
                POINT srcPt{0, 0};
                BLENDFUNCTION blend{AC_SRC_OVER, 0,
                                    static_cast<BYTE>(255.0f * fade),
                                    AC_SRC_ALPHA};
                UpdateLayeredWindow(ghost, screen, &dst, &size, mem, &srcPt, 0,
                                    &blend, ULW_ALPHA);

                if (first) {
                    // Slip the ghost in just below the taskbar so the window
                    // visibly pours into it.
                    if (!d.tray || !IsWindow(d.tray) ||
                        !SetWindowPos(ghost, d.tray, 0, 0, 0, 0,
                                      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                                          SWP_NOOWNERZORDER | SWP_SHOWWINDOW)) {
                        ShowWindow(ghost, SW_SHOWNOACTIVATE);
                    }
                    DwmFlush();
                    // The intact window is on screen as the ghost: let the
                    // hook minimize the real one, then start the clock.
                    SetEvent(job.firstFrame);
                    first = false;
                    QueryPerformanceCounter(&start);
                    continue;
                }

                if (last || g_unloading.load()) break;
                if (FAILED(DwmFlush())) Sleep(1);
            }
        }
        SelectObject(mem, oldBmp);
    }

    if (ghost) DestroyWindow(ghost);
    if (canvasBmp) DeleteObject(canvasBmp);
    if (mem) DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

static DWORD WINAPI GenieThread(LPVOID param) {
    std::unique_ptr<std::shared_ptr<GenieJob>> holder(
        static_cast<std::shared_ptr<GenieJob>*>(param));
    GenieJob& job = **holder;
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    RunGenie(job);
    // Release the hook even if the animation could not start.
    SetEvent(job.firstFrame);
    if (IsWindow(job.hWnd)) SetTransitions(job.hWnd, TRUE);
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_active.erase(job.hWnd);
    }
    return 0;
}

// Captures the window, finds its taskbar button, starts the genie and waits
// for its first frame, then hides the real window behind it. Returns null
// (nothing changed) if it can't.
static std::shared_ptr<GenieJob> BeginGenie(HWND hWnd) {
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (!g_active.insert(hWnd).second) return nullptr;
    }
    auto forget = [&] {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_active.erase(hWnd);
    };

    std::shared_ptr<GenieJob> job;
    try {
        job = std::make_shared<GenieJob>();
        PerMonitorDpiScope dpi;
        if (!CaptureWindow(hWnd, &job->snap)) {
            Wh_Log(L"Snapshot failed hwnd=%p", hWnd);
            forget();
            return nullptr;
        }
        job->dock = ComputeDockTarget(
            hWnd, MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST));
    } catch (...) {
        forget();
        return nullptr;
    }
    job->hWnd = hWnd;
    job->durationMs = g_durationMs.load();
    job->firstFrame = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!job->firstFrame) {
        forget();
        return nullptr;
    }

    auto* param = new (std::nothrow) std::shared_ptr<GenieJob>(job);
    if (!param || !SpawnWorker(GenieThread, param)) {
        delete param;
        forget();
        return nullptr;
    }

    SetTransitions(hWnd, FALSE);
    WaitPumpingSent(job->firstFrame, kFirstFrameWaitMs);
    if (IsWindow(hWnd)) SetCloak(hWnd, TRUE);
    return job;
}

// Called right after the native minimize returned.
static void EndGenieHandoff(HWND hWnd, GenieJob& job) {
    const bool minimized = IsWindow(hWnd) && IsIconic(hWnd);
    job.state.store(minimized ? JobState::Minimized : JobState::Cancelled);
    if (!IsWindow(hWnd)) return;
    // A minimized window draws nothing; uncloak now so a later restore shows.
    SetCloak(hWnd, FALSE);
    if (!minimized) SetTransitions(hWnd, TRUE);
}

thread_local bool t_inNativeMinimize = false;

template <typename NativeMinimize>
static auto MinimizeWithGenie(HWND hWnd, NativeMinimize&& nativeMinimize) {
    const DWORD entryError = GetLastError();
    const bool outer = !t_inNativeMinimize;
    std::shared_ptr<GenieJob> job;
    if (outer && IsMinimizeCandidate(hWnd)) job = BeginGenie(hWnd);

    t_inNativeMinimize = true;
    SetLastError(entryError);
    auto result = nativeMinimize();
    const DWORD nativeError = GetLastError();
    if (outer) t_inNativeMinimize = false;

    if (job) EndGenieHandoff(hWnd, *job);
    SetLastError(nativeError);
    return result;
}

////////////////////////////////////////////////////////////////////////////////
// Cross-process handoff (taskbar clicks are minimized by explorer.exe)

static void MaybeProbe(HWND hWnd) {
    if (!g_msgGenie || g_unloading.load() || !hWnd) return;
    const DWORD saved = GetLastError();
    if (GetAncestor(hWnd, GA_ROOT) == hWnd && IsOurWindow(hWnd) &&
        !GetPropW(hWnd, kPropProtocol) && !GetPropW(hWnd, kPropProbePending) &&
        SetPropW(hWnd, kPropProbePending, reinterpret_cast<HANDLE>(1))) {
        if (!PostMessageW(hWnd, g_msgGenie, 0, kProbe)) {
            RemovePropW(hWnd, kPropProbePending);
        }
    }
    SetLastError(saved);
}

// Hands an async minimize to the window's own process, which owns the genie.
// Only for windows that proved they deliver our message to DefWindowProc.
static bool TryRedirectMinimize(HWND hWnd, int cmd) {
    if (!g_msgGenie || g_unloading.load() || !hWnd) return false;
    const DWORD saved = GetLastError();
    const bool ok =
        reinterpret_cast<ULONG_PTR>(GetPropW(hWnd, kPropProtocol)) == 1 &&
        IsWindowVisible(hWnd) && !IsIconic(hWnd) && !IsWinKeyDown() &&
        PostMessageW(hWnd, g_msgGenie, static_cast<WPARAM>(cmd),
                     kMinimizeRequest);
    SetLastError(saved);
    return ok;
}

////////////////////////////////////////////////////////////////////////////////
// Hooks

BOOL WINAPI ShowWindow_Hook(HWND hWnd, int cmd) {
    if (IsMinimizeCmd(cmd) && hWnd && IsOurWindow(hWnd)) {
        return MinimizeWithGenie(hWnd,
                                 [&] { return ShowWindow_Original(hWnd, cmd); });
    }
    const BOOL result = ShowWindow_Original(hWnd, cmd);
    if (cmd != SW_HIDE && hWnd) MaybeProbe(hWnd);
    return result;
}

BOOL WINAPI ShowWindowAsync_Hook(HWND hWnd, int cmd) {
    if (IsMinimizeCmd(cmd) && TryRedirectMinimize(hWnd, cmd)) return TRUE;
    return ShowWindowAsync_Original(hWnd, cmd);
}

BOOL WINAPI CloseWindow_Hook(HWND hWnd) {
    if (hWnd && IsOurWindow(hWnd)) {
        return MinimizeWithGenie(hWnd,
                                 [&] { return CloseWindow_Original(hWnd); });
    }
    return CloseWindow_Original(hWnd);
}

LRESULT WINAPI DefWindowProcW_Hook(HWND hWnd, UINT msg, WPARAM wParam,
                                   LPARAM lParam) {
    if (msg == WM_SYSCOMMAND && (wParam & 0xFFF0) == SC_MINIMIZE) {
        return MinimizeWithGenie(hWnd, [&] {
            return DefWindowProcW_Original(hWnd, msg, wParam, lParam);
        });
    }

    if (g_msgGenie && msg == g_msgGenie) {
        if (lParam == kProbe) {
            RemovePropW(hWnd, kPropProbePending);
            if (!g_unloading.load()) {
                SetPropW(hWnd, kPropProtocol, reinterpret_cast<HANDLE>(1));
            }
            return 0;
        }
        if (lParam == kMinimizeRequest) {
            int cmd = static_cast<int>(wParam);
            if (!IsMinimizeCmd(cmd)) cmd = SW_MINIMIZE;
            if (!IsIconic(hWnd)) {
                MinimizeWithGenie(
                    hWnd, [&] { return ShowWindow_Original(hWnd, cmd); });
            }
            return 0;
        }
    }

    if (msg == WM_ACTIVATE || (msg == WM_SHOWWINDOW && wParam)) {
        MaybeProbe(hWnd);
    } else if (msg == WM_DESTROY) {
        std::lock_guard<std::mutex> lock(g_dockMutex);
        g_dockCache.erase(hWnd);
    }
    return DefWindowProcW_Original(hWnd, msg, wParam, lParam);
}

////////////////////////////////////////////////////////////////////////////////
// Processes that never own an app window: skip them entirely.

static bool IsNonInteractiveSessionProcess() {
    DWORD sessionId = 0;
    return ProcessIdToSessionId(GetCurrentProcessId(), &sessionId) &&
           sessionId == 0;
}

// Chromium/Electron helper processes (renderer, GPU, utility...).
static bool IsHelperProcess() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;
    bool helper = false;
    for (int i = 1; i < argc && !helper; ++i) {
        const wchar_t* arg = argv[i];
        const wchar_t* role = nullptr;
        if (_wcsnicmp(arg, L"--type=", 7) == 0) {
            role = arg + 7;
        } else if (_wcsicmp(arg, L"--type") == 0 && i + 1 < argc) {
            role = argv[++i];
        }
        if (role) {
            static const wchar_t* const kRoles[] = {
                L"renderer", L"gpu-process", L"utility",     L"crashpad-handler",
                L"plugin",   L"ppapi",       L"zygote",      L"nacl-loader",
                L"nacl-broker",
            };
            for (const wchar_t* r : kRoles) {
                if (_wcsicmp(role, r) == 0) {
                    helper = true;
                    break;
                }
            }
        }
        if (_wcsicmp(arg, L"--node-ipc") == 0 ||
            _wcsnicmp(arg, L"--headless", 10) == 0) {
            helper = true;
        }
    }
    LocalFree(argv);
    return helper;
}

////////////////////////////////////////////////////////////////////////////////
// Mod entry points

static void LoadSettings() {
    g_durationMs.store(std::clamp(Wh_GetIntSetting(L"duration_ms"), 150, 1500));
}

static BOOL CALLBACK ProbeOwnWindows(HWND hWnd, LPARAM) {
    if (IsWindowVisible(hWnd) && IsOurWindow(hWnd)) MaybeProbe(hWnd);
    return TRUE;
}

static BOOL CALLBACK RemoveOwnProps(HWND hWnd, LPARAM) {
    if (IsOurWindow(hWnd)) {
        RemovePropW(hWnd, kPropProtocol);
        RemovePropW(hWnd, kPropProbePending);
    }
    return TRUE;
}

BOOL Wh_ModInit() {
    if (IsNonInteractiveSessionProcess() || IsHelperProcess()) return FALSE;

    LoadSettings();
    g_msgGenie = RegisterWindowMessageW(L"Windhawk.GenieMinimize.V1");

    bool ok = true;
    ok &= WindhawkUtils::SetFunctionHook(ShowWindow, ShowWindow_Hook,
                                         &ShowWindow_Original);
    ok &= WindhawkUtils::SetFunctionHook(ShowWindowAsync, ShowWindowAsync_Hook,
                                         &ShowWindowAsync_Original);
    ok &= WindhawkUtils::SetFunctionHook(CloseWindow, CloseWindow_Hook,
                                         &CloseWindow_Original);
    ok &= WindhawkUtils::SetFunctionHook(DefWindowProcW, DefWindowProcW_Hook,
                                         &DefWindowProcW_Original);
    if (!ok) {
        Wh_Log(L"Failed to set hooks");
        return FALSE;
    }
    return TRUE;
}

// Hooks are live now: let existing windows prove they accept minimize
// requests from the taskbar.
void Wh_ModAfterInit() {
    EnumWindows(ProbeOwnWindows, 0);
}

void Wh_ModSettingsChanged() {
    LoadSettings();
}

// Stop new animations and make running ones finish at their next frame.
void Wh_ModBeforeUninit() {
    std::lock_guard<std::mutex> lock(g_workerMutex);
    g_unloading.store(true);
}

void Wh_ModUninit() {
    {
        std::lock_guard<std::mutex> lock(g_workerMutex);
        g_unloading.store(true);
    }
    // explorer.exe must stop redirecting minimizes to this process.
    EnumWindows(RemoveOwnProps, 0);

    std::vector<HANDLE> workers;
    {
        std::lock_guard<std::mutex> lock(g_workerMutex);
        workers.swap(g_workers);
    }
    for (HANDLE h : workers) {
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }

    std::lock_guard<std::mutex> lock(g_dockMutex);
    g_dockCache.clear();
    g_processDockCache.clear();
    g_dockInFlight.clear();
}
