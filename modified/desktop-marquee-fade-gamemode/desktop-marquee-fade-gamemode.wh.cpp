// ==WindhawkMod==
// @id              desktop-marquee-fade-gamemode
// @name            Desktop Marquee Fade (Game Mode)
// @description     Adds a smooth fade-out animation to the desktop selection rectangle when you release the mouse button. No global mouse hook: only the desktop's own mouse input is watched.
// @version         1.0.0-gamemode.2
// @author          angel_lov (desktop-thread hook changes by penicius-code)
// @github          https://github.com/Twilight0Sparkle
// @include         explorer.exe
// @compilerOptions -luser32 -lgdi32
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*

# Desktop Marquee Fade

A modified copy of *Desktop Marquee Fade* 1.0.0 by angel_lov. The change is described
under "Game Mode fork" below. Disable the original mod while this one is enabled.

Adds a smooth fade animation to the desktop selection rectangle.
Instead of vanishing instantly when you release the mouse button, the
selection area dissolves gracefully — making the Windows desktop feel
more polished and modern.

![preview](https://i.imgur.com/oH4fh6O.gif)

---

## Key Features

* **Smooth Fade Effect** — The selection rectangle fades out instead of
  disappearing instantly.
* **Highly Customizable** — Adjust animation speed and duration via mod
  settings.
* **Smart Auto-Opacity** — Automatically calculates the starting opacity to
  match your speed and duration so the animation always looks natural.
* **Smart Interrupt** — If another window comes to the foreground while the
  animation is playing, the mod speeds it up 4x to stay out of your way.
* **Single Instance Mode** — Prevents overlapping animations; only one
  selection rectangle fades at a time.
* **Custom Fade Colors** — Override the fill and border colors used by the
  fade overlay. Only the fade overlay is affected — your system-wide
  selection colors are never touched.

## How to use

1. Click and drag on the desktop to draw a selection rectangle.
2. Release the mouse button — the rectangle fades out smoothly.

## Game Mode fork: no global mouse hook

The original mod watches the left mouse button with a global low-level
mouse hook (`WH_MOUSE_LL`). With that hook, every mouse event in every
program, games included, has to wait for a round trip through
explorer.exe.

This version removes the global hook completely:

* It installs a thread-local mouse hook (`WH_MOUSE`) on the explorer thread
  that owns the desktop icon view. That hook only runs for mouse messages
  sent to the desktop's own windows. Mouse input to any other window or game
  never reaches the mod.
* While you draw the selection rectangle, the desktop holds mouse capture,
  so the button release still reaches it even if you let go over the taskbar
  or another window. A fade only starts if the desktop still held capture at
  that moment. A selection cancelled with Esc or by losing capture leaves no
  ghost rectangle.
* When nothing happens on the desktop, the mod does no work at all.
* The hook follows the desktop when it moves between Progman and WorkerW
  (Wallpaper Engine, slideshow) or when the icon view is re-created. It
  waits for the desktop to appear after an Explorer restart.

Settings and appearance are unchanged.

## Configuration

| Setting | Default | Description |
|---|---|---|
| Animation Speed (%) | 100 | Multiplier for how fast the animation runs (50–500). |
| Animation Duration (ms) | 300 | Total time the fade lasts (100–2000 ms). |
| Auto-calculate Opacity | Off | Scales starting opacity automatically to match speed/duration. |
| Manual Initial Opacity | 90 | Starting alpha (1–255) when auto-calculate is off. |
| Accelerate if Covered | On | Speeds up the fade 4× whenever another window is in the foreground. |
| Single Instance Only | Off | Destroys any previous fade when a new drag starts. |
| Use Custom Colors | Off | Overrides the fade overlay's fill and border colors with your custom hex values. The live drag rectangle is **not** affected. |
| Custom Fill Color | 8C8C8C | Hex color for the overlay fill. |
| Custom Border Color | B4B4B4 | Hex color for the overlay border. |

*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- speed: 100
  $name: Animation Speed (%)
  $description: Animation speed in percent (50-500). Default is 100.
- duration: 300
  $name: Animation Duration (ms)
  $description: Animation duration in milliseconds (100-2000). Default is 300.
- initial_alpha: 90
  $name: Manual Initial Opacity
  $description: Starting opacity (1-255). Only used if Auto-calculate Opacity is disabled. Default is 90.
- auto_calc_opacity: false
  $name: Auto-calculate Opacity
  $description: Automatically sets starting opacity based on speed and duration settings.
- accelerate_if_covered: true
  $name: Accelerate if covered
  $description: Speeds up the fade 4x whenever another window is in the foreground.
- single_instance: false
  $name: Single Instance Only
  $description: If ON, only one fading rectangle can be active at a time.
- use_custom_color: false
  $name: Use Custom Colors
  $description: Overrides the fade overlay's fill and border colors. The live drag rectangle keeps its normal system color.
- custom_fill_color: "8C8C8C"
  $name: Custom Fill Color (HEX)
  $description: Hex fill color for the fade overlay (e.g. 783838).
- custom_border_color: "B4B4B4"
  $name: Custom Border Color (HEX)
  $description: Hex border color for the fade overlay (e.g. FF8F8F).
*/
// ==/WindhawkModSettings==

#include <map>
#include <atomic>
#include <algorithm>
#include <cmath>
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <windhawk_api.h>
#include <windhawk_utils.h>

std::atomic<int>  g_settingSpeed{100};
std::atomic<int>  g_settingDuration{300};
std::atomic<int>  g_settingInitialAlpha{90};
std::atomic<bool> g_settingAutoCalcAlpha{false};
std::atomic<bool> g_settingAccelerateIfCovered{true};
std::atomic<bool> g_settingSingleInstance{false};
std::atomic<bool> g_settingUseCustomColor{false};
std::atomic<DWORD> g_settingCustomFillColor{0x8C8C8C};
std::atomic<DWORD> g_settingCustomBorderColor{0xB4B4B4};

// Worker thread: owns the manager window, the overlays and all hook
// bookkeeping below unless noted otherwise.
HWND   g_hManagerWnd = NULL;  // written before any hook exists, read by the hook
HANDLE g_hThread     = NULL;
DWORD  g_dwThreadId  = 0;

// Desktop tracking.
HHOOK         g_hMouseHook       = NULL;  // WH_MOUSE on the desktop thread only
DWORD         g_desktopThreadId  = 0;
HANDLE        g_hDesktopThread   = NULL;  // SYNCHRONIZE; signalled if it exits
HWINEVENTHOOK g_hWatchCreateShow = NULL;  // only while waiting for the desktop
HWINEVENTHOOK g_hWatchParent     = NULL;
bool          g_resyncPending    = false;
std::atomic<HWND> g_hDesktopListView{NULL};  // shared with the hook
std::atomic<int>  g_hookCallsInFlight{0};    // shared with the hook

// Touched only inside DesktopMouseProc (i.e. on the desktop thread).
HWND g_armedListView = NULL;

// Drag state, worker thread only. Filled by WM_PRECHECK_START, consumed by
// WM_DRAG_END. Both are posted in order from the desktop thread to the same
// window, so the check for a press always completes before its release is
// handled.
POINT g_dragStart             = {0, 0};
bool  g_dragStartValid        = false;
HWND  g_dragListView          = NULL;
HWND  g_dragDesktopTopLevel   = NULL;

#define TIMER_ID_FADE        1
#define WM_PRECHECK_START    (WM_USER + 2)  // wParam = list view, lParam = screen pt
#define WM_DRAG_END          (WM_USER + 3)  // wParam = list view, lParam = screen pt
#define WM_DESKTOP_RESYNC    (WM_USER + 4)

// The animation border starts at this alpha rather than fully opaque.
// The live drag marquee is visually softer than a flat 255 border, so
// starting fully solid produced a visible "pop" at mouse-up. 190 keeps
// the handoff seamless.
constexpr int kAnimBorderStartAlpha = 190;

struct AnimState {
    float alpha;
    float decrement;
};
std::map<HWND, AnimState> g_animations;

// ---------------------------------------------------------------------------
// Color helpers
// ---------------------------------------------------------------------------

COLORREF GetTargetFillColor() {
    if (g_settingUseCustomColor.load()) {
        DWORD hex = g_settingCustomFillColor.load();
        return RGB((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF);
    }
    return GetSysColor(COLOR_HIGHLIGHT);
}

COLORREF GetTargetBorderColor() {
    if (g_settingUseCustomColor.load()) {
        DWORD hex = g_settingCustomBorderColor.load();
        return RGB((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF);
    }
    return GetSysColor(COLOR_HIGHLIGHT);
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

int CalculateSmartAlpha(int speed, int duration) {
    // Perceptually what matters is how long the marquee actually stays visible:
    //   actualDuration = duration * 100 / speed
    // A short flash needs more initial punch; a long linger looks better dimmer.
    int actualDuration = (duration * 100) / speed;
    if (actualDuration < 16) actualDuration = 16;

    // Anchor: at the defaults (speed=100, duration=300) this returns ~90,
    // matching the manual default so toggling auto-calc changes nothing.
    const double kRefMs    = 300.0;
    const double kRefAlpha = 90.0;
    double scale      = std::sqrt(kRefMs / (double)actualDuration);
    double calculated = kRefAlpha * scale;
    return std::max(30, std::min(150, (int)lround(calculated)));
}

DWORD ParseHexColor(PCWSTR hexStr) {
    if (!hexStr) return 0;
    const wchar_t* p = hexStr;
    while (*p == L' ' || *p == L'\t') p++;
    if      (p[0] == L'0' && (p[1] == L'x' || p[1] == L'X')) p += 2;
    else if (p[0] == L'#') p += 1;
    return wcstoul(p, nullptr, 16);
}

DWORD ParseHexColorOrDefault(PCWSTR hexStr, DWORD defaultValue) {
    // Wh_GetStringSetting returns L"" (not NULL) for unset fields; an empty
    // string would silently parse to 0 (black) without this guard.
    if (!hexStr || hexStr[0] == L'\0') return defaultValue;
    return ParseHexColor(hexStr);
}

void LoadSettings() {
    g_settingSpeed.store(
        std::max(50, std::min(500, Wh_GetIntSetting(L"speed"))));
    g_settingDuration.store(
        std::max(100, std::min(2000, Wh_GetIntSetting(L"duration"))));
    g_settingInitialAlpha.store(
        std::max(1, std::min(255, Wh_GetIntSetting(L"initial_alpha"))));
    g_settingAutoCalcAlpha.store(
        Wh_GetIntSetting(L"auto_calc_opacity") != 0);
    g_settingAccelerateIfCovered.store(
        Wh_GetIntSetting(L"accelerate_if_covered") != 0);
    g_settingSingleInstance.store(
        Wh_GetIntSetting(L"single_instance") != 0);
    g_settingUseCustomColor.store(
        Wh_GetIntSetting(L"use_custom_color") != 0);

    // RAII: auto-frees the string on scope exit.
    WindhawkUtils::StringSetting fillStr =
        WindhawkUtils::StringSetting::make(L"custom_fill_color");
    g_settingCustomFillColor.store(
        ParseHexColorOrDefault(fillStr.get(), 0x8C8C8C));

    WindhawkUtils::StringSetting borderStr =
        WindhawkUtils::StringSetting::make(L"custom_border_color");
    g_settingCustomBorderColor.store(
        ParseHexColorOrDefault(borderStr.get(), 0xB4B4B4));
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void PaintMarqueeDIB(HWND hwnd, int width, int height,
                     int alphaBorder, int alphaFill, int borderThickness) {
    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem    = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi        = {0};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = width;
    bmi.bmiHeader.biHeight      = -height;  // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void*   pBits     = nullptr;
    HBITMAP hBitmap   = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS,
                                         &pBits, NULL, 0);
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);

    if (pBits) {
        COLORREF fillCol   = GetTargetFillColor();
        COLORREF borderCol = GetTargetBorderColor();

        BYTE fillR = GetRValue(fillCol),   fillG = GetGValue(fillCol),   fillB = GetBValue(fillCol);
        BYTE borR  = GetRValue(borderCol), borG  = GetGValue(borderCol), borB  = GetBValue(borderCol);

        // Clamp border thickness so a tiny rect never becomes all border.
        int bt    = borderThickness;
        int maxBt = std::max(1, std::min(width, height) / 2);
        bt = std::max(1, std::min(bt, maxBt));

        DWORD* pixels = (DWORD*)pBits;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                bool isBorder = (x < bt || x >= width - bt ||
                                 y < bt || y >= height - bt);
                if (isBorder) {
                    BYTE r = (borR * alphaBorder) / 255;
                    BYTE g = (borG * alphaBorder) / 255;
                    BYTE b = (borB * alphaBorder) / 255;
                    pixels[y * width + x] =
                        ((DWORD)alphaBorder << 24) | (r << 16) | (g << 8) | b;
                } else {
                    BYTE r = (fillR * alphaFill) / 255;
                    BYTE g = (fillG * alphaFill) / 255;
                    BYTE b = (fillB * alphaFill) / 255;
                    pixels[y * width + x] =
                        ((DWORD)alphaFill << 24) | (r << 16) | (g << 8) | b;
                }
            }
        }

        POINT        ptSrc  = {0, 0};
        SIZE         szWnd  = {width, height};
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(hwnd, hdcScreen, NULL, &szWnd,
                            hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);
    }

    SelectObject(hdcMem, hOldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

// ---------------------------------------------------------------------------
// Overlay window — one per active animation
// ---------------------------------------------------------------------------

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg,
                                 WPARAM wParam, LPARAM lParam) {
    if (msg == WM_TIMER && wParam == TIMER_ID_FADE) {
        auto it = g_animations.find(hwnd);
        if (it != g_animations.end()) {
            AnimState& st = it->second;

            if (g_settingAccelerateIfCovered.load()) {
                HWND fg = GetForegroundWindow();
                if (fg) {
                    HWND hRoot = GetAncestor(fg, GA_ROOT);
                    wchar_t cls[64];
                    if (GetClassNameW(hRoot, cls, 64) &&
                        wcscmp(cls, L"Progman") != 0 &&
                        wcscmp(cls, L"WorkerW") != 0) {
                        float fastDec = 255.0f / 5.0f;
                        if (st.decrement < fastDec) st.decrement = fastDec;
                    }
                }
            }

            st.alpha -= st.decrement;
            if (st.alpha <= 0.0f) {
                KillTimer(hwnd, TIMER_ID_FADE);
                g_animations.erase(it);
                DestroyWindow(hwnd);
            } else {
                BLENDFUNCTION blend = {AC_SRC_OVER, 0, (BYTE)st.alpha, AC_SRC_ALPHA};
                UpdateLayeredWindow(hwnd, NULL, NULL, NULL, NULL, NULL, 0, &blend, ULW_ALPHA);
            }
        }
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void StartFadeAnimation(const RECT& rc) {
    if (g_settingSingleInstance.load()) {
        for (auto const& pair : g_animations) {
            KillTimer(pair.first, TIMER_ID_FADE);
            DestroyWindow(pair.first);
        }
        g_animations.clear();
    }

    HWND hOverlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT |
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"FadeMarqueeOverlayClass", NULL, WS_POPUP,
        rc.left, rc.top,
        rc.right  - rc.left,
        rc.bottom - rc.top,
        NULL, NULL, GetModuleHandle(NULL), NULL);

    if (!hOverlay) return;

    int speed    = g_settingSpeed.load();
    int duration = g_settingDuration.load();
    int targetFillAlpha = g_settingAutoCalcAlpha.load()
        ? CalculateSmartAlpha(speed, duration)
        : g_settingInitialAlpha.load();

    int actualDuration = (duration * 100) / speed;
    if (actualDuration < 16) actualDuration = 16;

    // DPI-scale the border thickness for consistent appearance on
    // mixed-DPI multi-monitor setups.
    UINT dpi = GetDpiForWindow(hOverlay);
    if (dpi == 0) dpi = 96;
    int borderThickness = std::max(1, (int)lround((double)dpi / 96.0));

    int w = rc.right  - rc.left;
    int h = rc.bottom - rc.top;

    AnimState st;
    st.alpha      = 255.0f;
    st.decrement  = 255.0f / ((float)actualDuration / 16.0f);

    g_animations[hOverlay] = st;

    // Build the bitmap exactly ONCE
    PaintMarqueeDIB(hOverlay, w, h,
                    kAnimBorderStartAlpha, targetFillAlpha,
                    borderThickness);

    // Insert just above whatever window sits directly above the desktop
    // so the overlay is never drawn on top of real windows.
    HWND hAbove = g_dragDesktopTopLevel
        ? GetWindow(g_dragDesktopTopLevel, GW_HWNDPREV) : NULL;
    SetWindowPos(hOverlay,
                 hAbove ? hAbove : HWND_TOP,
                 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE |
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);

    SetTimer(hOverlay, TIMER_ID_FADE, 16, NULL);
}

// ---------------------------------------------------------------------------
// Desktop list-view discovery
// ---------------------------------------------------------------------------

HWND GetRealDesktopListView() {
    HWND hProgman      = FindWindowW(L"Progman", L"Program Manager");
    HWND hDesktopDefView =
        hProgman ? FindWindowExW(hProgman, NULL, L"SHELLDLL_DefView", NULL)
                 : NULL;

    if (!hDesktopDefView) {
        // Older builds with a wallpaper engine / slideshow: SHELLDLL_DefView
        // is moved into a top-level WorkerW.
        HWND hWorkerW = NULL;
        while ((hWorkerW = FindWindowExW(NULL, hWorkerW,
                                        L"WorkerW", NULL)) != NULL) {
            hDesktopDefView =
                FindWindowExW(hWorkerW, NULL, L"SHELLDLL_DefView", NULL);
            if (hDesktopDefView) break;
        }
    }
    if (!hDesktopDefView) return NULL;
    return FindWindowExW(hDesktopDefView, NULL, L"SysListView32", L"FolderView");
}

static bool HasClass(HWND hwnd, PCWSTR className) {
    WCHAR cls[32];
    return hwnd && GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) &&
           wcscmp(cls, className) == 0;
}

// Called on the desktop thread from the hook, for button presses only.
static bool IsDesktopListView(HWND hwnd) {
    if (!hwnd) return false;
    if (hwnd == g_hDesktopListView.load(std::memory_order_relaxed)) return true;

    // Slow path: the icon view may have been re-created on this thread.
    if (!HasClass(hwnd, L"SysListView32")) return false;
    HWND parent = GetParent(hwnd);
    if (!HasClass(parent, L"SHELLDLL_DefView")) return false;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!HasClass(root, L"Progman") && !HasClass(root, L"WorkerW")) return false;

    g_hDesktopListView.store(hwnd, std::memory_order_relaxed);
    return true;
}

static LPARAM PackPoint(POINT pt) {
    return MAKELPARAM((WORD)(SHORT)pt.x, (WORD)(SHORT)pt.y);
}

static POINT UnpackPoint(LPARAM lParam) {
    return POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
}

// ---------------------------------------------------------------------------
// Thread-local mouse hook (WH_MOUSE) on the desktop thread.
//
// Runs in explorer's desktop thread, and only when that thread takes a mouse
// message from its queue, i.e. for input aimed at the desktop's own windows.
// During a marquee drag the list view holds mouse capture, so the release
// is delivered here even if it happens over another window. Mouse input to
// any other window or process never reaches this code.
// ---------------------------------------------------------------------------

LRESULT CALLBACK DesktopMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    g_hookCallsInFlight.fetch_add(1);

    if (nCode == HC_ACTION && lParam) {
        const MOUSEHOOKSTRUCT* m = (const MOUSEHOOKSTRUCT*)lParam;

        if (wParam == WM_LBUTTONDOWN || wParam == WM_LBUTTONDBLCLK) {
            g_armedListView = NULL;
            if (m->wHitTestCode == HTCLIENT && IsDesktopListView(m->hwnd)) {
                g_armedListView = m->hwnd;
                // Icon hit-test runs on the worker thread.
                PostMessageW(g_hManagerWnd, WM_PRECHECK_START,
                             (WPARAM)m->hwnd, PackPoint(m->pt));
            }
        } else if (wParam == WM_LBUTTONUP && g_armedListView) {
            HWND lv = g_armedListView;
            g_armedListView = NULL;
            // A marquee drag holds capture until it receives this release.
            // If capture is gone the drag was cancelled (Esc, focus loss,
            // capture taken by someone else), so there is nothing to fade.
            if (m->hwnd == lv && GetCapture() == lv) {
                PostMessageW(g_hManagerWnd, WM_DRAG_END,
                             (WPARAM)lv, PackPoint(m->pt));
            }
        }
    }

    LRESULT result = CallNextHookEx(NULL, nCode, wParam, lParam);
    g_hookCallsInFlight.fetch_sub(1);
    return result;
}

// ---------------------------------------------------------------------------
// Desktop tracking (worker thread)
//
// * Hooked: WH_MOUSE on the thread that owns the desktop list view, plus a
//   wait on that thread's handle in the worker's message loop. No timers, no
//   WinEvent hooks. Re-creation of the list view on the same thread and moves
//   between Progman / WorkerW are handled by the hook itself (the hook is
//   per thread, not per window).
// * Waiting (explorer still starting, or the desktop thread exited): WinEvent
//   hooks limited to this process watch for the desktop windows to appear.
//   They are removed as soon as the desktop is hooked.
// ---------------------------------------------------------------------------

static void CALLBACK Desktop_OnWinEvent(HWINEVENTHOOK, DWORD, HWND hwnd,
                                        LONG idObject, LONG idChild,
                                        DWORD, DWORD) {
    if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF ||
        g_resyncPending) {
        return;
    }
    if (!HasClass(hwnd, L"SysListView32") &&
        !HasClass(hwnd, L"SHELLDLL_DefView")) {
        return;
    }
    // Don't touch the hooks from inside their own callback.
    g_resyncPending = true;
    PostMessageW(g_hManagerWnd, WM_DESKTOP_RESYNC, 0, 0);
}

static void Desktop_StartWatching() {
    if (g_hWatchCreateShow || g_hWatchParent) return;
    DWORD pid = GetCurrentProcessId();
    DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNTHREAD;
    g_hWatchCreateShow = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW,
                                         NULL, Desktop_OnWinEvent, pid, 0, flags);
    g_hWatchParent = SetWinEventHook(EVENT_OBJECT_PARENTCHANGE,
                                     EVENT_OBJECT_PARENTCHANGE, NULL,
                                     Desktop_OnWinEvent, pid, 0, flags);
    Wh_Log(L"Waiting for the desktop window");
}

static void Desktop_StopWatching() {
    if (g_hWatchCreateShow) {
        UnhookWinEvent(g_hWatchCreateShow);
        g_hWatchCreateShow = NULL;
    }
    if (g_hWatchParent) {
        UnhookWinEvent(g_hWatchParent);
        g_hWatchParent = NULL;
    }
}

static BOOL CALLBACK FirstThreadWindowProc(HWND hwnd, LPARAM lParam) {
    *(HWND*)lParam = hwnd;
    return FALSE;
}

static void Desktop_Unhook() {
    if (g_hMouseHook) {
        UnhookWindowsHookEx(g_hMouseHook);
        g_hMouseHook = NULL;

        // The desktop thread may be inside DesktopMouseProc right now.
        // Round-trip a message through it: it can only process a sent message
        // after leaving the hook proc (or nested inside CallNextHookEx, where
        // the in-flight counter is still raised). Then wait for the counter.
        bool alive = !g_hDesktopThread ||
                     WaitForSingleObject(g_hDesktopThread, 0) == WAIT_TIMEOUT;
        if (alive && g_desktopThreadId) {
            HWND target = NULL;
            EnumThreadWindows(g_desktopThreadId, FirstThreadWindowProc,
                              (LPARAM)&target);
            if (target) {
                DWORD_PTR ignored = 0;
                SendMessageTimeoutW(target, WM_NULL, 0, 0,
                                    SMTO_NORMAL | SMTO_ABORTIFHUNG, 1000,
                                    &ignored);
            }
        }
        for (int i = 0; g_hookCallsInFlight.load() > 0; i++) {
            if (g_hDesktopThread) {
                if (WaitForSingleObject(g_hDesktopThread, 1) != WAIT_TIMEOUT)
                    break;  // thread is gone, it can't return into our code
            } else {
                if (i >= 5000) break;
                Sleep(1);
            }
        }
    }

    if (g_hDesktopThread) {
        CloseHandle(g_hDesktopThread);
        g_hDesktopThread = NULL;
    }
    g_desktopThreadId = 0;
}

static void Desktop_Sync() {
    HWND lv = GetRealDesktopListView();
    DWORD pid = 0;
    DWORD tid = lv ? GetWindowThreadProcessId(lv, &pid) : 0;

    if (!tid || pid != GetCurrentProcessId()) {
        // Not created yet (explorer starting) or owned by another process.
        Desktop_StartWatching();
        return;
    }

    if (g_hMouseHook && tid == g_desktopThreadId) {
        g_hDesktopListView.store(lv);
        Desktop_StopWatching();
        return;
    }

    Desktop_Unhook();
    g_hDesktopListView.store(lv);

    HANDLE hThread = OpenThread(SYNCHRONIZE, FALSE, tid);
    // Thread in this process: hMod must be NULL (documented).
    HHOOK hook = SetWindowsHookExW(WH_MOUSE, DesktopMouseProc, NULL, tid);
    if (!hook) {
        Wh_Log(L"SetWindowsHookEx(WH_MOUSE, thread %u) failed: %u", tid,
               GetLastError());
        if (hThread) CloseHandle(hThread);
        Desktop_StartWatching();
        return;
    }

    g_hMouseHook      = hook;
    g_desktopThreadId = tid;
    g_hDesktopThread  = hThread;
    Desktop_StopWatching();
    Wh_Log(L"Desktop mouse hook installed on thread %u", tid);
}

static void Desktop_OnThreadExited() {
    Wh_Log(L"Desktop thread %u exited", g_desktopThreadId);
    Desktop_Unhook();
    g_hDesktopListView.store(NULL);
    g_armedListView = NULL;  // no hook can run now
    Desktop_Sync();
}

// ---------------------------------------------------------------------------
// Manager window — receives posted messages from the desktop thread's hook
// ---------------------------------------------------------------------------

LRESULT CALLBACK ManagerWndProc(HWND hwnd, UINT msg,
                                 WPARAM wParam, LPARAM lParam) {
    if (msg == WM_PRECHECK_START) {
        g_dragStartValid = false;

        HWND lv = (HWND)wParam;
        if (!IsWindow(lv)) return 0;

        POINT pt = UnpackPoint(lParam);

        // Only start a fade for drags that begin on empty desktop space.
        LVHITTESTINFO lvhti;
        ZeroMemory(&lvhti, sizeof(lvhti));
        lvhti.pt = pt;
        ScreenToClient(lv, &lvhti.pt);
        DWORD_PTR dwResult = 0;

        bool isOnIcon = false;
        if (SendMessageTimeoutW(lv, LVM_HITTEST, 0, (LPARAM)&lvhti,
                                SMTO_ABORTIFHUNG | SMTO_NORMAL,
                                50, &dwResult)) {
            isOnIcon = ((int)dwResult != -1);
        }
        if (isOnIcon) return 0;

        g_dragStart           = pt;
        g_dragListView        = lv;
        g_dragDesktopTopLevel = GetAncestor(lv, GA_ROOT);
        g_dragStartValid      = true;
        return 0;
    }

    if (msg == WM_DRAG_END) {
        if (!g_dragStartValid || (HWND)wParam != g_dragListView) return 0;
        g_dragStartValid = false;

        POINT end = UnpackPoint(lParam);
        int w = abs((int)end.x - (int)g_dragStart.x);
        int h = abs((int)end.y - (int)g_dragStart.y);

        if (w > 10 && h > 10) {
            int left = std::min((int)g_dragStart.x, (int)end.x);
            int top  = std::min((int)g_dragStart.y, (int)end.y);
            RECT rc  = {left, top, left + w, top + h};
            StartFadeAnimation(rc);
        }
        return 0;
    }

    if (msg == WM_DESKTOP_RESYNC) {
        g_resyncPending = false;
        Desktop_Sync();
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Worker thread — owns the message loop, the hooks, and the overlay windows
// ---------------------------------------------------------------------------

DWORD WINAPI WorkerThreadProc(LPVOID) {
    WNDCLASSW wcm = {0, ManagerWndProc, 0, 0, GetModuleHandle(NULL),
                     NULL, NULL, NULL, NULL, L"FadeMarqueeManagerClass"};
    RegisterClassW(&wcm);

    WNDCLASSW wco = {0, OverlayWndProc, 0, 0, GetModuleHandle(NULL),
                     NULL, NULL, NULL, NULL, L"FadeMarqueeOverlayClass"};
    RegisterClassW(&wco);

    g_hManagerWnd = CreateWindowExW(0, L"FadeMarqueeManagerClass", NULL, 0,
                                    0, 0, 0, 0, HWND_MESSAGE, NULL,
                                    GetModuleHandle(NULL), NULL);

    if (g_hManagerWnd) {
        Desktop_Sync();
    } else {
        Wh_Log(L"CreateWindowEx failed: %u", GetLastError());
    }

    MSG msg;
    bool quit = false;
    while (!quit) {
        if (g_hDesktopThread) {
            // Sleep until a message arrives or the desktop thread exits.
            DWORD r = MsgWaitForMultipleObjectsEx(1, &g_hDesktopThread,
                                                  INFINITE, QS_ALLINPUT,
                                                  MWMO_INPUTAVAILABLE);
            if (r == WAIT_OBJECT_0) {
                Desktop_OnThreadExited();
                continue;
            }
            if (r == WAIT_FAILED) {
                CloseHandle(g_hDesktopThread);
                g_hDesktopThread = NULL;
                continue;
            }
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    quit = true;
                    break;
                }
                DispatchMessageW(&msg);
            }
        } else {
            BOOL ret = GetMessageW(&msg, NULL, 0, 0);
            if (ret == 0 || ret == -1) break;
            DispatchMessageW(&msg);
        }
    }

    // Hooks first, so nothing posts to the manager window any more.
    Desktop_StopWatching();
    Desktop_Unhook();
    g_hDesktopListView.store(NULL);

    // Discard leftovers (no posted message carries a heap payload).
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
    }

    // Tear down all live overlays.
    for (auto const& pair : g_animations) {
        KillTimer(pair.first, TIMER_ID_FADE);
        DestroyWindow(pair.first);
    }
    g_animations.clear();

    if (g_hManagerWnd) DestroyWindow(g_hManagerWnd);

    UnregisterClassW(L"FadeMarqueeManagerClass", GetModuleHandle(NULL));
    UnregisterClassW(L"FadeMarqueeOverlayClass", GetModuleHandle(NULL));
    return 0;
}

// ---------------------------------------------------------------------------
// Windhawk entry points
// ---------------------------------------------------------------------------

BOOL Wh_ModInit() {
    // Skip explorer instances launched as /factory or -Embedding (folder
    // windows in a separate process); they never own the desktop.
    PCWSTR cmdLine = GetCommandLineW();
    if (cmdLine && (wcsstr(cmdLine, L"/factory") || wcsstr(cmdLine, L"-Embedding"))) {
        return FALSE;
    }

    // Only run in the shell explorer instance.
    HWND hShell = GetShellWindow();
    if (hShell) {
        DWORD shellPid = 0;
        GetWindowThreadProcessId(hShell, &shellPid);
        if (shellPid != 0 && shellPid != GetCurrentProcessId()) {
            return FALSE;
        }
    }

    LoadSettings();
    g_hThread = CreateThread(NULL, 0, WorkerThreadProc, NULL, 0, &g_dwThreadId);
    return g_hThread != NULL;
}

void Wh_ModSettingsChanged() {
    LoadSettings();
}

void Wh_ModUninit() {
    if (g_hThread) {
        // PostThreadMessage fails until the worker has a message queue
        // (created by its first window call), so retry until it has one or
        // has exited.
        while (!PostThreadMessageW(g_dwThreadId, WM_QUIT, 0, 0) &&
               WaitForSingleObject(g_hThread, 10) == WAIT_TIMEOUT) {
        }
        // Wait unconditionally: a timed-out wait would unload the DLL while
        // the worker thread is still running code that lives in it.
        WaitForSingleObject(g_hThread, INFINITE);
        CloseHandle(g_hThread);
        g_hThread = NULL;
    }
}
