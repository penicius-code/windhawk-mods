// ==WindhawkMod==
// @id              thanos-close-animation
// @name            Thanos Close Animation
// @description     Windows turn to dust and blow away when you close them (Thanos snap)
// @version         1.1.1
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
// @compilerOptions -ldwmapi -lgdi32 -luser32 -lshell32
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Thanos Close Animation

Closing a window turns it into dust that drifts away, starting from the
bottom-right corner. This is only the close effect: it does not touch
minimize, restore or opening windows, so it pairs with a minimize mod such as
*MacOS Minimize Animation*.

## Use with other animation mods

- **MacOS Minimize Animation**: turn its **Animate window close** setting off,
  otherwise both mods animate the same close.
- **Windows Animations**: disable it, or turn off its close animation.

## Settings

- **Particle size**: 1 is the finest dust (the look in most previews) but the
  heaviest to draw; 2 is a good balance; 3+ looks blockier.
- **Duration**: how long the dust takes to blow away.
- **Animate windows hidden to the tray**: browsers (Chrome, Edge, Brave),
  Discord, Steam and other tray apps hide their window instead of closing it.
  Keep this on or those closes are not animated.

## Not animated

Store/system apps (Settings, Calculator, Photos), MMC consoles, small dialogs
and popups, windows smaller than 300x300, and fullscreen windows (games,
F11 browsers, fullscreen video).

Effect math and close detection adapted from *Windows Animations* by ReDrag
(MIT).
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- duration_ms: 800
  $name: Duration (ms)
  $description: How long the dust takes to blow away. 200 to 3000.
- particle_size: 2
  $name: Particle size (px)
  $description: 1 = finest dust, 2 = balanced, 3+ = blockier and lighter on the CPU. Very large windows are automatically drawn with slightly bigger particles.
- hide_as_close: true
  $name: Animate windows hidden to the tray
  $description: Needed for browsers and tray apps, which hide their window instead of closing it.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <windhawk_utils.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

////////////////////////////////////////////////////////////////////////////////
// Settings and global state

std::atomic<int> g_durationMs{800};
std::atomic<int> g_particleSize{2};
std::atomic<bool> g_hideAsClose{true};
std::atomic<bool> g_unloading{false};

constexpr PCWSTR kPropActive = L"ThanosClose.Active";
constexpr PCWSTR kPropBypass = L"ThanosClose.Bypass";
constexpr PCWSTR kPropGhost = L"ThanosClose.Ghost";

// Windows with a close animation in flight (process-local).
std::mutex g_stateMutex;
std::unordered_set<HWND> g_active;

// Every worker is tracked so unloading can join them before the DLL goes away.
std::mutex g_workerMutex;
std::vector<HANDLE> g_workers;

// Thanos constants (from Windows Animations).
constexpr float kThanosBaseStartMax = 0.6f;
constexpr float kThanosWaveNoise = 0.15f;
constexpr float kThanosLifeSpan = 0.4f;
// The last particles start at 0.65 and live 0.4, so stretch progress slightly
// so every particle is gone exactly when the animation ends.
constexpr float kThanosProgressScale = 1.05f;
// Upper bound on particles per animation (memory and per-frame cost).
constexpr size_t kMaxParticles = 3000000;

using DestroyWindow_t = decltype(&DestroyWindow);
using ShowWindow_t = decltype(&ShowWindow);
using ShowWindowAsync_t = decltype(&ShowWindowAsync);
using SetWindowPos_t = decltype(&SetWindowPos);
using SetWindowPlacement_t = decltype(&SetWindowPlacement);
using DefWindowProcW_t = decltype(&DefWindowProcW);

DestroyWindow_t DestroyWindow_Original;
ShowWindow_t ShowWindow_Original;
ShowWindowAsync_t ShowWindowAsync_Original;
SetWindowPos_t SetWindowPos_Original;
SetWindowPlacement_t SetWindowPlacement_Original;
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

// Undo everything a close animation did to a window that is still alive
// (hidden to the tray, or a close that was cancelled).
static void RestoreWindowState(HWND hWnd) {
    if (!IsWindow(hWnd)) return;
    SetCloak(hWnd, FALSE);
    SetTransitions(hWnd, TRUE);
    RemovePropW(hWnd, kPropActive);
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
    // taskbar and shell surfaces must never dust away.
    if (IsExplorerProcess() && _wcsicmp(cls, L"CabinetWClass") != 0 &&
        _wcsicmp(cls, L"ExploreWClass") != 0) {
        return true;
    }
    return false;
}

// These close through DefWindowProc's WM_CLOSE / SC_CLOSE and never reach a
// visible DestroyWindow on their own thread.
static bool IsCloseMessageClass(HWND hWnd) {
    WCHAR cls[128];
    if (!GetClassNameW(hWnd, cls, ARRAYSIZE(cls))) return false;
    return _wcsicmp(cls, L"ConsoleWindowClass") == 0 ||
           _wcsicmp(cls, L"CASCADIA_HOSTING_WINDOW_CLASS") == 0 ||
           _wcsicmp(cls, L"TaskManagerWindow") == 0 ||
           _wcsicmp(cls, L"WinUIDesktopWin32WindowClass") == 0;
}

// Fullscreen games, F11 browsers and fullscreen video are never animated: the
// snapshot would run on the game's window thread (a full-resolution copy) and
// quitting would be delayed by the animation while the game still holds its
// VRAM. Fullscreen = covers its whole monitor and is not a maximized normal
// (captioned) window, or the foreground window while Windows reports
// exclusive D3D fullscreen.
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

// A visible, top-level, unowned app window of this process, at least 300x300.
static bool IsCloseCandidate(HWND hWnd) {
    if (g_unloading.load() || !hWnd || !IsWindow(hWnd) ||
        !IsWindowVisible(hWnd) || IsIconic(hWnd) ||
        GetAncestor(hWnd, GA_ROOT) != hWnd || GetWindow(hWnd, GW_OWNER)) {
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);
    if (pid != GetCurrentProcessId()) return false;

    const LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
    const LONG_PTR exStyle = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);
    if ((style & WS_CHILD) || (exStyle & WS_EX_TOOLWINDOW)) return false;

    if (GetPropW(hWnd, kPropActive) || GetPropW(hWnd, kPropBypass) ||
        GetPropW(hWnd, kPropGhost)) {
        return false;
    }

    WCHAR cls[128];
    if (!GetClassNameW(hWnd, cls, ARRAYSIZE(cls)) || IsExcludedClass(cls)) {
        return false;
    }
    if (IsCloaked(hWnd)) return false;

    RECT r;
    return GetWindowRect(hWnd, &r) && r.right - r.left >= 300 &&
           r.bottom - r.top >= 300 && !IsFullscreenWindow(hWnd);
}

// Does the process still have another visible app window? If not, the close
// probably ends the process, so the caller is held until the dust is gone -
// the animation runs inside this process and would otherwise be cut short.
struct OtherWindowSearch {
    HWND self;
    DWORD pid;
    bool found;
};

static BOOL CALLBACK FindOtherAppWindow(HWND w, LPARAM lp) {
    auto* s = reinterpret_cast<OtherWindowSearch*>(lp);
    if (w == s->self || !IsWindowVisible(w) || GetAncestor(w, GA_ROOT) != w ||
        GetWindow(w, GW_OWNER) ||
        (GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) ||
        GetPropW(w, kPropGhost)) {
        return TRUE;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == s->pid && !IsCloaked(w)) {
        s->found = true;
        return FALSE;
    }
    return TRUE;
}

static bool HasOtherAppWindow(HWND hWnd) {
    OtherWindowSearch search{hWnd, GetCurrentProcessId(), false};
    EnumWindows(FindOtherAppWindow, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

////////////////////////////////////////////////////////////////////////////////
// Snapshot

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
// same corners off (otherwise square bits of the desktop dust away too).
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
// Animation worker

enum class CloseMode {
    Destroy,       // DestroyWindow: the hook performs the destroy
    SyncHide,      // ShowWindow/SetWindowPos/SetWindowPlacement hide
    AsyncHide,     // ShowWindowAsync(SW_HIDE): the hook submits it right away
    CloseMessage,  // WM_CLOSE / SC_CLOSE swallowed: worker reposts it
};

struct CloseJob {
    HWND hWnd = nullptr;
    CloseMode mode = CloseMode::Destroy;
    UINT closeMsg = 0;
    Snapshot snap;
    int durationMs = 800;
    int particleSize = 2;
    HANDLE firstFrame = nullptr;  // worker-owned duplicate
    HANDLE done = nullptr;        // worker-owned duplicate
};

struct Particle {
    float start;
    float windX;
    float windY;
    float curveX;
    float curveY;
};

static inline uint32_t ScalePremultiplied(uint32_t p, uint32_t a) {
    const uint32_t rb = ((p & 0x00FF00FFu) * a >> 8) & 0x00FF00FFu;
    const uint32_t ag = (((p >> 8) & 0x00FF00FFu) * a) & 0xFF00FF00u;
    return rb | ag;
}

struct Canvas {
    uint32_t* px;
    int w;
    int h;
};

// Copies one block of the snapshot to (dx, dy), clipped to the canvas width
// and to the canvas rows [clipTop, clipBottom).
static inline void DrawBlock(const Canvas& c, const uint32_t* src, int srcW,
                             int sx, int sy, int bw, int bh, int dx, int dy,
                             uint32_t alpha, int clipTop, int clipBottom) {
    const int x0 = std::max(0, -dx);
    const int x1 = std::min(bw, c.w - dx);
    const int y0 = std::max(0, clipTop - dy);
    const int y1 = std::min(bh, clipBottom - dy);
    if (x0 >= x1 || y0 >= y1) return;
    const int n = x1 - x0;
    for (int y = y0; y < y1; ++y) {
        const uint32_t* s = src + static_cast<size_t>(sy + y) * srcW + sx + x0;
        uint32_t* d = c.px + static_cast<size_t>(dy + y) * c.w + dx + x0;
        if (alpha >= 256) {
            memcpy(d, s, static_cast<size_t>(n) * sizeof(uint32_t));
        } else {
            for (int k = 0; k < n; ++k) d[k] = ScalePremultiplied(s[k], alpha);
        }
    }
}

// Each block starts later the closer it is to the top-left, then drifts with
// a wind pushing right/down plus a per-particle curve, fading as it goes.
static void BuildParticles(int W, int H, int b, std::vector<Particle>* out) {
    const int cols = (W + b - 1) / b;
    const int rows = (H + b - 1) / b;
    out->clear();
    out->reserve(static_cast<size_t>(cols) * rows);
    const float maxDist = static_cast<float>(W + H);
    for (int sy = 0; sy < H; sy += b) {
        const float swirl = (static_cast<float>(sy) / H - 0.5f) * 2.0f;
        for (int sx = 0; sx < W; sx += b) {
            const uint32_t hash = (static_cast<uint32_t>(sx) * 73856093u) ^
                                  (static_cast<uint32_t>(sy) * 19349663u);
            const float dist = static_cast<float>((W - sx) + (H - sy));
            float start = dist / maxDist * kThanosBaseStartMax +
                          ((hash % 100) / 100.0f - 0.5f) * kThanosWaveNoise;
            start = std::clamp(start, 0.0f, 0.65f);
            const float nx = ((hash >> 4) % 200) / 100.0f - 1.0f;
            const float ny = ((hash >> 8) % 200) / 100.0f - 1.0f;
            Particle p;
            p.start = start;
            p.windX = W * 0.5f + nx * W * 0.2f;
            p.windY = H * 0.2f + swirl * H * 0.3f + ny * H * 0.3f;
            p.curveX = ny * W * 0.4f;
            p.curveY = -nx * H * 0.4f;
            out->push_back(p);
        }
    }
}

static inline int RoundToInt(float v) {
    return static_cast<int>(v < 0.0f ? v - 0.5f : v + 0.5f);
}

// Where a particle lands this frame, in canvas coordinates. alpha 0 = skip.
struct Placed {
    int16_t x;
    int16_t y;
    uint16_t alpha;
};

// Canvas-space rectangle, exclusive right/bottom.
struct Box {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    bool Empty() const { return left >= right || top >= bottom; }
    void Add(const Box& o) {
        if (o.Empty()) return;
        if (Empty()) {
            *this = o;
            return;
        }
        left = std::min(left, o.left);
        top = std::min(top, o.top);
        right = std::max(right, o.right);
        bottom = std::max(bottom, o.bottom);
    }
};

// Runs task(0..count-1) on a small private thread pool plus the calling
// thread, and returns when all of them are done. Falls back to running
// everything on the calling thread if the pool can't be created.
class ParallelRunner {
public:
    ParallelRunner() {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        const int helpers =
            std::clamp(static_cast<int>(si.dwNumberOfProcessors) - 1, 0, 7);
        if (!helpers) return;
        pool_ = CreateThreadpool(nullptr);
        if (!pool_) return;
        SetThreadpoolThreadMaximum(pool_, helpers);
        SetThreadpoolThreadMinimum(pool_, helpers);  // spin threads up now
        InitializeThreadpoolEnvironment(&env_);
        envReady_ = true;
        SetThreadpoolCallbackPool(&env_, pool_);
        work_ = CreateThreadpoolWork(WorkCallback, this, &env_);
        if (work_) helpers_ = helpers;
    }

    ~ParallelRunner() {
        if (work_) {
            WaitForThreadpoolWorkCallbacks(work_, FALSE);
            CloseThreadpoolWork(work_);
        }
        if (pool_) CloseThreadpool(pool_);
        if (envReady_) DestroyThreadpoolEnvironment(&env_);
    }

    ParallelRunner(const ParallelRunner&) = delete;
    ParallelRunner& operator=(const ParallelRunner&) = delete;

    int Threads() const { return helpers_ + 1; }

    template <typename F>
    void Run(int count, F& task) {
        if (count <= 0) return;
        call_ = [](void* ctx, int i) { (*static_cast<F*>(ctx))(i); };
        ctx_ = &task;
        count_ = count;
        next_.store(0);
        const int submits = std::min(helpers_, count - 1);
        for (int k = 0; k < submits; ++k) SubmitThreadpoolWork(work_);
        Drain();
        if (submits) WaitForThreadpoolWorkCallbacks(work_, FALSE);
    }

private:
    static VOID CALLBACK WorkCallback(PTP_CALLBACK_INSTANCE, PVOID self,
                                      PTP_WORK) {
        static_cast<ParallelRunner*>(self)->Drain();
    }

    void Drain() {
        for (int i; (i = next_.fetch_add(1)) < count_;) call_(ctx_, i);
    }

    PTP_POOL pool_ = nullptr;
    PTP_WORK work_ = nullptr;
    TP_CALLBACK_ENVIRON env_{};
    bool envReady_ = false;
    int helpers_ = 0;
    std::atomic<int> next_{0};
    int count_ = 0;
    void (*call_)(void*, int) = nullptr;
    void* ctx_ = nullptr;
};

// The vblank the next presented frame will most likely land on, so every
// frame samples the animation exactly one refresh apart (even motion instead
// of whatever moment the loop happened to wake up).
static LONGLONG NextVBlank(LONGLONG now) {
    DWM_TIMING_INFO ti{};
    ti.cbSize = sizeof(ti);
    if (FAILED(DwmGetCompositionTimingInfo(nullptr, &ti)) ||
        ti.qpcRefreshPeriod == 0 || ti.qpcVBlank == 0) {
        return now;
    }
    const LONGLONG period = static_cast<LONGLONG>(ti.qpcRefreshPeriod);
    LONGLONG t = static_cast<LONGLONG>(ti.qpcVBlank);
    if (t <= now) t += ((now - t) / period + 1) * period;
    return t;
}

static LONGLONG QpcNow() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static void RunAnimation(CloseJob& job) {
    const Snapshot& s = job.snap;
    const RECT& rc = s.rect;

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

    // Room for the dust to drift (mostly right and down), kept on the monitor
    // but never smaller than the window itself.
    const int W = s.w, H = s.h;
    int left = std::max<int>(mi.rcMonitor.left, rc.left - W / 2);
    int top = std::max<int>(mi.rcMonitor.top, rc.top - H / 2);
    int right = std::min<int>(mi.rcMonitor.right, rc.right + W * 3 / 2);
    int bottom = std::min<int>(mi.rcMonitor.bottom, rc.bottom + H * 3 / 2);
    left = std::min<int>(left, rc.left);
    top = std::min<int>(top, rc.top);
    right = std::max<int>(right, rc.right);
    bottom = std::max<int>(bottom, rc.bottom);
    const int cw = right - left;
    const int ch = bottom - top;
    // Placed stores 16-bit canvas coordinates.
    if (cw <= 0 || ch <= 0 || cw > 30000 || ch > 30000) return;

    int b = std::clamp(job.particleSize, 1, 10);
    while (static_cast<size_t>((W + b - 1) / b) * ((H + b - 1) / b) >
           kMaxParticles) {
        ++b;
    }
    const int cols = (W + b - 1) / b;
    const int rows = (H + b - 1) / b;
    std::vector<Particle> particles;
    std::vector<Placed> placed;
    std::vector<Box> rowBox;
    try {
        BuildParticles(W, H, b, &particles);
        placed.resize(particles.size());
        rowBox.resize(rows);
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
            SetPropW(ghost, kPropGhost, reinterpret_cast<HANDLE>(1));
            const Canvas canvas{static_cast<uint32_t*>(bits), cw, ch};
            memset(canvas.px, 0, static_cast<size_t>(cw) * ch * 4);
            const uint32_t* src = s.px.data();
            const int offX = rc.left - left;
            const int offY = rc.top - top;
            const double duration = std::max(1, job.durationMs);

            ParallelRunner runner;
            constexpr int kRowsPerTask = 8;
            const int placeTasks = (rows + kRowsPerTask - 1) / kRowsPerTask;
            const int bands = std::min(ch, runner.Threads() * 4);

            float p = 0.0f;  // scaled progress of the frame being built
            Box prev;        // everything the previous frame drew

            // Pass 1, split by source rows: where each particle goes this
            // frame, plus the area each row covers.
            auto place = [&](int task) {
                const int r0 = task * kRowsPerTask;
                const int r1 = std::min(rows, r0 + kRowsPerTask);
                for (int r = r0; r < r1; ++r) {
                    const int sy = r * b;
                    const int bh = std::min(b, H - sy);
                    Box box;
                    size_t i = static_cast<size_t>(r) * cols;
                    for (int sx = 0; sx < W; sx += b, ++i) {
                        const Particle& q = particles[i];
                        Placed& out = placed[i];
                        out.alpha = 0;
                        const float local =
                            (p - q.start) * (1.0f / kThanosLifeSpan);
                        if (local >= 1.0f) continue;
                        int x = offX + sx;
                        int y = offY + sy;
                        uint32_t alpha = 256;
                        if (local > 0.0f) {
                            const float l2 = local * local;
                            x += RoundToInt(q.windX * local + q.curveX * l2);
                            y += RoundToInt(q.windY * local + q.curveY * l2);
                            alpha =
                                static_cast<uint32_t>((1.0f - local) * 256.0f);
                            if (!alpha) continue;
                        }
                        const int bw = std::min(b, W - sx);
                        if (x + bw <= 0 || x >= cw || y + bh <= 0 || y >= ch) {
                            continue;
                        }
                        out.x = static_cast<int16_t>(x);
                        out.y = static_cast<int16_t>(y);
                        out.alpha = static_cast<uint16_t>(alpha);
                        box.Add(Box{std::max(x, 0), std::max(y, 0),
                                    std::min(x + bw, cw), std::min(y + bh, ch)});
                    }
                    rowBox[r] = box;
                }
            };

            // Pass 2, split by horizontal canvas bands: erase the previous
            // frame's dust in the band, then draw every block touching it in
            // the original order. Bands never overlap, so no locking.
            auto draw = [&](int band) {
                const int bandTop =
                    static_cast<int>(static_cast<int64_t>(ch) * band / bands);
                const int bandBottom = static_cast<int>(
                    static_cast<int64_t>(ch) * (band + 1) / bands);
                if (!prev.Empty()) {
                    const int y0 = std::max(bandTop, prev.top);
                    const int y1 = std::min(bandBottom, prev.bottom);
                    const size_t n =
                        static_cast<size_t>(prev.right - prev.left) * 4;
                    for (int y = y0; y < y1; ++y) {
                        memset(canvas.px + static_cast<size_t>(y) * cw +
                                   prev.left,
                               0, n);
                    }
                }
                for (int r = 0; r < rows; ++r) {
                    const Box& rb = rowBox[r];
                    if (rb.Empty() || rb.bottom <= bandTop ||
                        rb.top >= bandBottom) {
                        continue;
                    }
                    const int sy = r * b;
                    const int bh = std::min(b, H - sy);
                    size_t i = static_cast<size_t>(r) * cols;
                    for (int sx = 0; sx < W; sx += b, ++i) {
                        const Placed& q = placed[i];
                        if (!q.alpha || q.y + bh <= bandTop ||
                            q.y >= bandBottom) {
                            continue;
                        }
                        DrawBlock(canvas, src, W, sx, sy, std::min(b, W - sx),
                                  bh, q.x, q.y, q.alpha, bandTop, bandBottom);
                    }
                }
            };

            // Only the changed part of the canvas is sent to DWM.
            auto present = [&](const RECT* dirty) {
                POINT dst{left, top};
                SIZE size{cw, ch};
                POINT srcPt{0, 0};
                BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
                UPDATELAYEREDWINDOWINFO info{};
                info.cbSize = sizeof(info);
                info.hdcDst = screen;
                info.pptDst = &dst;
                info.psize = &size;
                info.hdcSrc = mem;
                info.pptSrc = &srcPt;
                info.pblend = &blend;
                info.dwFlags = ULW_ALPHA;
                info.prcDirty = dirty;
                if (!UpdateLayeredWindowIndirect(ghost, &info) && dirty) {
                    info.prcDirty = nullptr;
                    UpdateLayeredWindowIndirect(ghost, &info);
                }
            };

            LARGE_INTEGER freq;
            QueryPerformanceFrequency(&freq);
            LONGLONG start = QpcNow();
            bool first = true;

            for (;;) {
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }

                double elapsed = 0.0;
                if (!first) {
                    elapsed = (NextVBlank(QpcNow()) - start) * 1000.0 /
                              freq.QuadPart;
                }
                const bool last = !first && elapsed >= duration;
                p = static_cast<float>(std::min(1.0, elapsed / duration)) *
                    kThanosProgressScale;

                runner.Run(placeTasks, place);
                Box cur;
                for (const Box& rb : rowBox) cur.Add(rb);
                runner.Run(bands, draw);

                Box dirty = prev;
                dirty.Add(cur);
                prev = cur;

                if (first) {
                    present(nullptr);
                    // The intact window is on screen as the ghost: let the
                    // hook hide the real one, then start the clock.
                    ShowWindow(ghost, SW_SHOWNOACTIVATE);
                    DwmFlush();
                    SetEvent(job.firstFrame);
                    first = false;
                    start = QpcNow();
                    continue;
                }

                if (!dirty.Empty()) {
                    const RECT r{dirty.left, dirty.top, dirty.right,
                                 dirty.bottom};
                    present(&r);
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


// Waits (bounded) until the window is gone or hidden.
static void WaitWindowGoneOrHidden(HWND hWnd, DWORD timeoutMs) {
    const DWORD start = GetTickCount();
    while (IsWindow(hWnd) && IsWindowVisible(hWnd) &&
           GetTickCount() - start < timeoutMs) {
        Sleep(10);
    }
}

static void FinishJob(CloseJob& job) {
    HWND hWnd = job.hWnd;
    switch (job.mode) {
        case CloseMode::Destroy:
        case CloseMode::SyncHide:
            break;  // the hook completes these itself

        case CloseMode::AsyncHide:
            WaitWindowGoneOrHidden(hWnd, 500);
            RestoreWindowState(hWnd);
            break;

        case CloseMode::CloseMessage:
            if (!IsWindow(hWnd)) break;
            SetPropW(hWnd, kPropBypass, reinterpret_cast<HANDLE>(1));
            if (job.closeMsg == WM_SYSCOMMAND) {
                PostMessageW(hWnd, WM_SYSCOMMAND, SC_CLOSE, 0);
            } else {
                PostMessageW(hWnd, WM_CLOSE, 0, 0);
            }
            WaitWindowGoneOrHidden(hWnd, 2000);
            // Still here: the app asked "are you sure?" or refused to close.
            if (IsWindow(hWnd)) {
                RestoreWindowState(hWnd);
                RemovePropW(hWnd, kPropBypass);
            }
            break;
    }
}

static DWORD WINAPI CloseAnimThread(LPVOID param) {
    std::unique_ptr<CloseJob> job(static_cast<CloseJob*>(param));
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    RunAnimation(*job);
    // Release the hook even if the animation could not start.
    SetEvent(job->firstFrame);
    FinishJob(*job);
    SetEvent(job->done);

    CloseHandle(job->firstFrame);
    CloseHandle(job->done);
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_active.erase(job->hWnd);
    }
    return 0;
}

// Captures the window, starts the dust, waits for its first frame, then hides
// the real window behind it. Returns false (nothing changed) if it can't.
static bool BeginClose(HWND hWnd, CloseMode mode, UINT closeMsg,
                       HANDLE* doneOut) {
    *doneOut = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (!g_active.insert(hWnd).second) return false;
    }
    auto forget = [&] {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_active.erase(hWnd);
    };

    auto job = std::make_unique<CloseJob>();
    bool captured = false;
    try {
        captured = CaptureWindow(hWnd, &job->snap);
    } catch (...) {
        captured = false;
    }
    if (!captured) {
        Wh_Log(L"Snapshot failed hwnd=%p", hWnd);
        forget();
        return false;
    }

    HANDLE firstFrame = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE self = GetCurrentProcess();
    if (!firstFrame || !done ||
        !DuplicateHandle(self, firstFrame, self, &job->firstFrame, 0, FALSE,
                         DUPLICATE_SAME_ACCESS) ||
        !DuplicateHandle(self, done, self, &job->done, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        if (job->firstFrame) CloseHandle(job->firstFrame);
        if (job->done) CloseHandle(job->done);
        if (firstFrame) CloseHandle(firstFrame);
        if (done) CloseHandle(done);
        forget();
        return false;
    }

    job->hWnd = hWnd;
    job->mode = mode;
    job->closeMsg = closeMsg;
    job->durationMs = g_durationMs.load();
    job->particleSize = g_particleSize.load();

    SetPropW(hWnd, kPropActive, reinterpret_cast<HANDLE>(1));
    SetTransitions(hWnd, FALSE);

    CloseJob* raw = job.release();
    if (!SpawnWorker(CloseAnimThread, raw)) {
        CloseHandle(raw->firstFrame);
        CloseHandle(raw->done);
        delete raw;
        CloseHandle(firstFrame);
        CloseHandle(done);
        RestoreWindowState(hWnd);
        forget();
        return false;
    }

    WaitPumpingSent(firstFrame, 500);
    CloseHandle(firstFrame);
    if (IsWindow(hWnd)) SetCloak(hWnd, TRUE);
    *doneOut = done;
    return true;
}

static DWORD FullAnimationWaitMs() {
    return static_cast<DWORD>(g_durationMs.load()) + 700;
}

////////////////////////////////////////////////////////////////////////////////
// Hooks

BOOL WINAPI DestroyWindow_Hook(HWND hWnd) {
    const DWORD entryError = GetLastError();
    bool animated = false;
    if (hWnd && GetWindowThreadProcessId(hWnd, nullptr) == GetCurrentThreadId() &&
        IsCloseCandidate(hWnd)) {
        HANDLE done = nullptr;
        animated = BeginClose(hWnd, CloseMode::Destroy, 0, &done);
        if (animated && !HasOtherAppWindow(hWnd)) {
            WaitPumpingSent(done, FullAnimationWaitMs());
        }
        if (done) CloseHandle(done);
    }

    SetLastError(entryError);
    const BOOL result = DestroyWindow_Original(hWnd);
    if (animated && IsWindow(hWnd)) {
        const DWORD error = GetLastError();
        RestoreWindowState(hWnd);  // destroy failed: bring the window back
        SetLastError(error);
    }
    return result;
}

// Synchronous hide treated as a close (browsers, tray apps).
template <typename NativeHide>
static BOOL HideWithAnimation(HWND hWnd, NativeHide&& nativeHide) {
    const DWORD entryError = GetLastError();
    bool animated = false;
    if (g_hideAsClose.load() && IsCloseCandidate(hWnd) &&
        !IsCloseMessageClass(hWnd)) {
        HANDLE done = nullptr;
        animated = BeginClose(hWnd, CloseMode::SyncHide, 0, &done);
        if (animated && !HasOtherAppWindow(hWnd)) {
            WaitPumpingSent(done, FullAnimationWaitMs());
        }
        if (done) CloseHandle(done);
    }

    SetLastError(entryError);
    const BOOL result = nativeHide();
    if (animated && IsWindow(hWnd)) {
        const DWORD error = GetLastError();
        // Hidden now; uncloak so it shows normally when the app brings it back.
        RestoreWindowState(hWnd);
        SetLastError(error);
    }
    return result;
}

BOOL WINAPI ShowWindow_Hook(HWND hWnd, int cmd) {
    if (cmd == SW_HIDE) {
        return HideWithAnimation(
            hWnd, [&] { return ShowWindow_Original(hWnd, cmd); });
    }
    return ShowWindow_Original(hWnd, cmd);
}

BOOL WINAPI ShowWindowAsync_Hook(HWND hWnd, int cmd) {
    if (cmd == SW_HIDE && g_hideAsClose.load() && IsCloseCandidate(hWnd) &&
        !IsCloseMessageClass(hWnd)) {
        const DWORD entryError = GetLastError();
        HANDLE done = nullptr;
        if (BeginClose(hWnd, CloseMode::AsyncHide, 0, &done) && done) {
            CloseHandle(done);
        }
        SetLastError(entryError);
    }
    return ShowWindowAsync_Original(hWnd, cmd);
}

BOOL WINAPI SetWindowPos_Hook(HWND hWnd, HWND insertAfter, int x, int y, int cx,
                              int cy, UINT flags) {
    if ((flags & SWP_HIDEWINDOW) && !(flags & SWP_ASYNCWINDOWPOS)) {
        return HideWithAnimation(hWnd, [&] {
            return SetWindowPos_Original(hWnd, insertAfter, x, y, cx, cy, flags);
        });
    }
    return SetWindowPos_Original(hWnd, insertAfter, x, y, cx, cy, flags);
}

BOOL WINAPI SetWindowPlacement_Hook(HWND hWnd, const WINDOWPLACEMENT* wp) {
    if (wp && wp->showCmd == SW_HIDE &&
        !(wp->flags & WPF_ASYNCWINDOWPLACEMENT)) {
        return HideWithAnimation(
            hWnd, [&] { return SetWindowPlacement_Original(hWnd, wp); });
    }
    return SetWindowPlacement_Original(hWnd, wp);
}

LRESULT WINAPI DefWindowProcW_Hook(HWND hWnd, UINT msg, WPARAM wParam,
                                   LPARAM lParam) {
    const bool closeMessage =
        msg == WM_CLOSE || (msg == WM_SYSCOMMAND && (wParam & 0xFFF0) == SC_CLOSE);
    if (!closeMessage) {
        return DefWindowProcW_Original(hWnd, msg, wParam, lParam);
    }

    // Our own repost after the dust: let it through. SC_CLOSE turns into
    // WM_CLOSE synchronously, so the marker is consumed at WM_CLOSE.
    if (GetPropW(hWnd, kPropBypass)) {
        if (msg == WM_CLOSE) RemovePropW(hWnd, kPropBypass);
        return DefWindowProcW_Original(hWnd, msg, wParam, lParam);
    }
    // Second click while it is already dusting.
    if (GetPropW(hWnd, kPropActive)) return 0;

    if (IsCloseMessageClass(hWnd) &&
        GetWindowThreadProcessId(hWnd, nullptr) == GetCurrentThreadId() &&
        IsCloseCandidate(hWnd)) {
        HANDLE done = nullptr;
        if (BeginClose(hWnd, CloseMode::CloseMessage, msg, &done)) {
            if (done) CloseHandle(done);
            return 0;
        }
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
    g_durationMs.store(std::clamp(Wh_GetIntSetting(L"duration_ms"), 200, 3000));
    g_particleSize.store(std::clamp(Wh_GetIntSetting(L"particle_size"), 1, 10));
    g_hideAsClose.store(Wh_GetIntSetting(L"hide_as_close") != 0);
}

BOOL Wh_ModInit() {
    if (IsNonInteractiveSessionProcess() || IsHelperProcess()) return FALSE;

    LoadSettings();

    bool ok = true;
    ok &= WindhawkUtils::SetFunctionHook(DestroyWindow, DestroyWindow_Hook,
                                         &DestroyWindow_Original);
    ok &= WindhawkUtils::SetFunctionHook(ShowWindow, ShowWindow_Hook,
                                         &ShowWindow_Original);
    ok &= WindhawkUtils::SetFunctionHook(ShowWindowAsync, ShowWindowAsync_Hook,
                                         &ShowWindowAsync_Original);
    ok &= WindhawkUtils::SetFunctionHook(SetWindowPos, SetWindowPos_Hook,
                                         &SetWindowPos_Original);
    ok &= WindhawkUtils::SetFunctionHook(SetWindowPlacement,
                                         SetWindowPlacement_Hook,
                                         &SetWindowPlacement_Original);
    ok &= WindhawkUtils::SetFunctionHook(DefWindowProcW, DefWindowProcW_Hook,
                                         &DefWindowProcW_Original);
    if (!ok) {
        Wh_Log(L"Failed to set hooks");
        return FALSE;
    }
    return TRUE;
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
    std::vector<HANDLE> workers;
    {
        std::lock_guard<std::mutex> lock(g_workerMutex);
        workers.swap(g_workers);
    }
    for (HANDLE h : workers) {
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }
}
