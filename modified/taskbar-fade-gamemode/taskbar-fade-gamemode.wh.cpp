// ==WindhawkMod==
// @id              taskbar-fade-gamemode
// @name            Taskbar Fade (Game Mode)
// @description     Reduces visual clutter by automatically dimming or hiding the taskbar when idle. Ideal for focused workflows and preventing OLED burn-in. Pauses while a fullscreen game has focus.
// @version         2.2.0-gamemode.1
// @author          Lukvbp (fullscreen pause by penicius-code)
// @github          https://github.com/lukvbp
// @include         windhawk.exe
// @architecture    x86
// @architecture    x86-64
// @compilerOptions -luser32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Taskbar Fade (Game Mode)

A modified copy of *Taskbar Fade* 2.2.0 by Lukvbp. The change is described under
"Game mode (this fork)" below. Disable the original mod while this one is enabled.

Automatically dims or hides the taskbar when the computer is idle.

## Game mode (this fork)
The original polls the cursor and idle time 15 times a second (60 times a
second while fading), also while a game runs. This fork stops that loop while
a fullscreen window (exclusive or borderless fullscreen game, F11 browser,
fullscreen video) has focus and the taskbar is hidden behind it. The taskbar
keeps its current opacity and the loop resumes as soon as you switch away.
If a taskbar is visible on another monitor, the loop keeps running.
Can be switched off with the *Fullscreen game mode* setting.

This mod is designed to prevent OLED burn-in and reduce visual distractions by lowering the taskbar's opacity when it is not in use.

### Features
* **Idle Dimming:** The taskbar fades to a lower transparency level (configurable) when the mouse is away.
* **Idle Hiding:** Optionally hides the taskbar completely after a set timeout.
* **Instant Wake:** Moving the mouse to the taskbar or the bottom of the screen restores full opacity immediately.
* **Hover Fade Delay:** (Optional) Waits a configurable amount of time after the cursor leaves the taskbar before starting the dim fade, so brief accidental exits don't trigger it.
* **Smart Idle:** (Optional) Only hide the taskbar if the desktop is focused. If windows are open, keep the taskbar dimmed instead.
* **Clean Desktop:** (Optional) Hide desktop icons when the taskbar hides for a completely black screen.
* **Customization:** Settings available for fade speed, transparency levels, and timeout duration.

### Configuration
* **Default Transparency:** The opacity level when the taskbar is dimmed (0-100%).
* **Hover Fade Delay:** Milliseconds to wait after the cursor leaves the taskbar before the dim fade begins.
* **Enable Idle Hiding:** Check this to make the taskbar vanish completely after a timeout.
* **Smart Idle:** If checked, idle hiding is disabled when any window is open/focused.
* **Hide Desktop Icons:** If checked, desktop icons will also vanish when idle.
* **Idle Timeout:** Seconds of inactivity before the taskbar hides.
* **Fade Speed:** Duration of the fade animation in milliseconds.

*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- DimTransparency: 50
  $name: Default Transparency (%)
  $description: How see-through the bar is when active. (0=Solid, 100=Invisible).
- HoverFadeDelay: 0
  $name: Hover Fade Delay
  $description: Time (milliseconds) to wait after leaving the taskbar before beginning the dim fade.
- EnableIdleFade: true
  $name: Enable Idle Hiding
  $description: If checked, the taskbar will vanish completely when you are away.
- SmartIdle: false
  $name: Smart Idle (Only hide on empty desktop)
  $description: Prevents the taskbar from fully hiding if you have a window (like a browser) open and focused.
- HideDesktopIcons: false
  $name: Hide Desktop Icons on Idle
  $description: When the taskbar fully hides, also hide desktop icons for a perfectly clean screen.
- IdleTimeout: 15
  $name: Idle Timeout (Seconds)
  $description: Time before the taskbar disappears (if Idle Hiding is enabled).
- FadeDuration: 250
  $name: Wake/Dim Speed (ms)
  $description: How fast it animates when you are using the PC.
- IdleFadeDuration: 2000
  $name: Idle Fade Speed (ms)
  $description: How fast it fades out when you walk away.
- TargetFPS: 60
  $name: Animation Smoothness (FPS)
  $description: Higher is smoother but uses more CPU. (Range 15 to 120).
- FullscreenGameMode: pause
  $name: Fullscreen game mode
  $description: Pause the fade loop while a fullscreen window (game, F11 browser, video) has focus and covers every visible taskbar.
  $options:
  - pause: Pause while a fullscreen window has focus
  - off: Off (original behaviour)
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <shellapi.h>
#include <atomic>
#include <thread>
#include <vector>
#include <cmath>
#include <mutex>

// --- MOD LOGIC ---

// Global Coordination
std::atomic<bool> g_stopThread(false);
std::thread g_workerThread;
std::mutex g_settingsMutex;
std::atomic<bool> g_weHidIcons(false); // Tracks if we were the ones who hid the icons

// Game mode (fork): the worker sleeps on g_wakeEvent instead of Sleep() so it
// can be woken for stop and for game-mode changes. g_gamePaused is written by
// the game-mode thread only.
HANDLE g_wakeEvent = nullptr;            // auto-reset
std::atomic<bool> g_gamePaused(false);
std::atomic<bool> g_gameModeEnabled(true);
HANDLE g_gameThread = nullptr;
DWORD g_gameThreadId = 0;
#define WM_GAMEMODE_RECHECK (WM_APP + 1)

// State Machine
enum class FadeState {
    Active, // Mouse is hovering
    Dimmed, // Mouse away, but not yet idle (or idle hiding disabled)
    Idle    // Idle timeout reached (Overrides hover)
};

// Runtime Settings Cache
struct {
    bool enableIdleFade;
    bool smartIdle;
    bool hideDesktopIcons;
    DWORD idleTimeoutMs;
    int fadeDuration;
    int idleFadeDuration;
    int frameTimeMs;
    double targetDimAlpha;
    int hoverFadeDelayMs;
} g_cache;

// Window Tracking
struct TaskbarState {
    HWND hwnd;
    bool isLayered;     // CACHE
    bool isTransparent; // CACHE
    int lastAlpha;
    RECT rect;          // Cached geometry
};

// --- SETTINGS ---
void LoadSettings() {
    std::lock_guard<std::mutex> lock(g_settingsMutex);

    bool enableIdle = Wh_GetIntSetting(L"EnableIdleFade") != 0;
    bool smartIdle = Wh_GetIntSetting(L"SmartIdle") != 0;
    bool hideIcons = Wh_GetIntSetting(L"HideDesktopIcons") != 0;

    int rawTimeout = Wh_GetIntSetting(L"IdleTimeout");
    int rawTransp = Wh_GetIntSetting(L"DimTransparency");
    int rawFade = Wh_GetIntSetting(L"FadeDuration");
    int rawIdleFade = Wh_GetIntSetting(L"IdleFadeDuration");
    int rawFPS = Wh_GetIntSetting(L"TargetFPS");

    int rawHoverDelay = Wh_GetIntSetting(L"HoverFadeDelay");

    if (rawTimeout < 1) {
        rawTimeout = 1;
    }
    if (rawTransp < 0) {
        rawTransp = 0;
    }
    if (rawTransp > 100) {
        rawTransp = 100;
    }

    // FPS Clamp: 15 to 120
    if (rawFPS < 15) {
        rawFPS = 15;
    }
    if (rawFPS > 120) {
        rawFPS = 120;
    }
    if (rawHoverDelay < 0) {
        rawHoverDelay = 0;
    }
    if (rawHoverDelay > 10000) {
        rawHoverDelay = 10000;
    }

    int calculatedFrameTime = 1000 / rawFPS;

    // Validate Durations
    if (rawFade < calculatedFrameTime) {
        rawFade = calculatedFrameTime;
    }
    if (rawIdleFade < calculatedFrameTime) {
        rawIdleFade = calculatedFrameTime;
    }

    g_cache.enableIdleFade = enableIdle;
    g_cache.smartIdle = smartIdle;
    g_cache.hideDesktopIcons = hideIcons;

    // (DWORD) cast prevents signed integer overflow during multiplication
    // if user enters a large duration (e.g. > 24 days)
    g_cache.idleTimeoutMs = (DWORD)rawTimeout * 1000;

    g_cache.fadeDuration = rawFade;
    g_cache.idleFadeDuration = rawIdleFade;
    g_cache.frameTimeMs = calculatedFrameTime;

    double opacityPercent = (100.0 - (double)rawTransp);
    g_cache.targetDimAlpha = (opacityPercent * 255.0) / 100.0;
    
    g_cache.hoverFadeDelayMs = rawHoverDelay;

    // Empty (setting not written yet) counts as the default "pause".
    PCWSTR gameMode = Wh_GetStringSetting(L"FullscreenGameMode");
    g_gameModeEnabled = !(gameMode && wcscmp(gameMode, L"off") == 0);
    Wh_FreeStringSetting(gameMode);
}

void WhTool_ModSettingsChanged() {
    LoadSettings();
    if (g_gameThreadId) {
        PostThreadMessageW(g_gameThreadId, WM_GAMEMODE_RECHECK, 0, 0);
    }
}

// --- WINDOW HELPERS ---

void RestoreTaskbarStyle(HWND hwnd) {
    if (!IsWindow(hwnd)) {
        return;
    }
    
    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_LAYERED) || (exStyle & WS_EX_TRANSPARENT)) {
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle & ~(WS_EX_LAYERED | WS_EX_TRANSPARENT));
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}

// Helper to find the Desktop Icons window (SysListView32)
// Handles both standard Progman and WorkerW (wallpaper engine / task view) cases
HWND GetDesktopListView() {
    HWND hProgman = FindWindowW(L"Progman", L"Program Manager");
    HWND hDefView = FindWindowExW(hProgman, NULL, L"SHELLDLL_DefView", NULL);

    if (!hDefView) {
        // Fallback: iterate WorkerW windows
        HWND hWorkerW = NULL;
        while ((hWorkerW = FindWindowExW(NULL, hWorkerW, L"WorkerW", NULL)) != NULL) {
            hDefView = FindWindowExW(hWorkerW, NULL, L"SHELLDLL_DefView", NULL);
            if (hDefView) {
                break;
            }
        }
    }

    if (hDefView) {
        return FindWindowExW(hDefView, NULL, L"SysListView32", L"FolderView");
    }
    return NULL;
}

// 5-Stage Pipeline: Safe Ordering for Style Changes
void UpdateBarOpacity(TaskbarState& bar, int alpha) {
    if (!IsWindow(bar.hwnd)) {
        return;
    }

    // Early-Out if state is already correct
    if (bar.lastAlpha == alpha) {
        return;
    }

    bool needLayered = (alpha < 255);
    bool needTransparent = (alpha == 0);

    // 1. PRE-CLEANUP: Remove Transparent
    if (bar.isTransparent && !needTransparent) {
        LONG_PTR exStyle = GetWindowLongPtr(bar.hwnd, GWL_EXSTYLE);
        SetWindowLongPtr(bar.hwnd, GWL_EXSTYLE, exStyle & ~WS_EX_TRANSPARENT);
        bar.isTransparent = false;
    }

    // 2. SETUP: Enable Layered
    if (!bar.isLayered && needLayered) {
        LONG_PTR exStyle = GetWindowLongPtr(bar.hwnd, GWL_EXSTYLE);
        SetWindowLongPtr(bar.hwnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);
        bar.isLayered = true;
        // Force update on next step
        bar.lastAlpha = -1;
    }

    // 3. APPLY: Set Alpha
    if (needLayered) {
        if (SetLayeredWindowAttributes(bar.hwnd, 0, (BYTE)alpha, LWA_ALPHA)) {
            bar.lastAlpha = alpha;
        }
    }

    // 4. POST-POLISH: Enable Transparent
    if (!bar.isTransparent && needTransparent) {
        LONG_PTR exStyle = GetWindowLongPtr(bar.hwnd, GWL_EXSTYLE);
        SetWindowLongPtr(bar.hwnd, GWL_EXSTYLE, exStyle | WS_EX_TRANSPARENT);
        bar.isTransparent = true;
    }

    // 5. CLEANUP: Remove Layered
    if (bar.isLayered && !needLayered) {
        LONG_PTR exStyle = GetWindowLongPtr(bar.hwnd, GWL_EXSTYLE);
        SetWindowLongPtr(bar.hwnd, GWL_EXSTYLE, exStyle & ~WS_EX_LAYERED);
        bar.isLayered = false;
        SetWindowPos(bar.hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}

// --- WORKER LOOP ---
void WorkerLoop() {
    std::vector<TaskbarState> taskbars;
    LASTINPUTINFO lii = { sizeof(LASTINPUTINFO) };

    // Animation State
    double currentAlpha = 255.0;
    double lastTarget = -1.0;
    double animStartAlpha = 255.0;
    FadeState lastState = FadeState::Dimmed;

    // Timing State
    ULONGLONG animStartTime = 0;
    ULONGLONG lastGeometryUpdate = 0;

    // Local snapshot variables
    bool enableIdleFade;
    bool smartIdle;
    bool hideDesktopIcons;
    bool isDesktopFocused = true; // Assumed true if smart idle is off
    DWORD idleTimeoutMs;
    int fadeDuration;
    int idleFadeDuration;
    int frameTimeMs;
    double targetDimAlpha;
    int hoverFadeDelayMs;

    // Hover delay tracking (persists across iterations)
    ULONGLONG lastHoverTime = 0;

    // Game mode (fork)
    bool wasPaused = false;

    while (!g_stopThread) {
        // Game mode: a fullscreen window covers the taskbar. Do nothing (the
        // taskbar keeps its current opacity) until the game-mode thread or
        // uninit signals the wake event.
        if (g_gamePaused) {
            wasPaused = true;
            WaitForSingleObject(g_wakeEvent, INFINITE);
            continue;
        }
        if (wasPaused) {
            wasPaused = false;
            // Continue an interrupted fade from where it stopped instead of
            // jumping, and refresh taskbar geometry right away.
            animStartAlpha = currentAlpha;
            animStartTime = GetTickCount64();
            lastGeometryUpdate = 0;
        }

        // 0. Settings Snapshot
        {
            std::lock_guard<std::mutex> lock(g_settingsMutex);
            enableIdleFade = g_cache.enableIdleFade;
            smartIdle = g_cache.smartIdle;
            hideDesktopIcons = g_cache.hideDesktopIcons;
            idleTimeoutMs = g_cache.idleTimeoutMs;
            fadeDuration = g_cache.fadeDuration;
            idleFadeDuration = g_cache.idleFadeDuration;
            frameTimeMs = g_cache.frameTimeMs;
            targetDimAlpha = g_cache.targetDimAlpha;
            hoverFadeDelayMs = g_cache.hoverFadeDelayMs;
        }

        ULONGLONG now = GetTickCount64();

        // 1. Discovery & Recovery
        bool needsRefresh = taskbars.empty();
        if (!needsRefresh) {
            for (const auto& bar : taskbars) {
                if (!IsWindow(bar.hwnd)) {
                    needsRefresh = true;
                    break;
                }
            }
        }

        if (needsRefresh) {
            taskbars.clear();

            auto AddTaskbar = [&](HWND h) {
                if (h) {
                    RECT r = {0}; 
                    GetWindowRect(h, &r);

                    // Ignore empty rectangles (e.g. native auto-hide active)
                    if (!IsRectEmpty(&r)) {
                        LONG_PTR style = GetWindowLongPtr(h, GWL_EXSTYLE);
                        bool isLayered = (style & WS_EX_LAYERED) != 0;
                        bool isTrans = (style & WS_EX_TRANSPARENT) != 0;
                        taskbars.push_back({ h, isLayered, isTrans, -1, r });
                    }
                }
            };

            AddTaskbar(FindWindow(L"Shell_TrayWnd", NULL));
            HWND hSec = NULL;
            while ((hSec = FindWindowEx(NULL, hSec, L"Shell_SecondaryTrayWnd", NULL)) != NULL) {
                AddTaskbar(hSec);
            }

            if (taskbars.empty()) {
                // Prevent CPU spin if no taskbars found (e.g. Explorer restarting)
                WaitForSingleObject(g_wakeEvent, 250);
                continue;
            }

            // Reset Animation State
            currentAlpha = 255.0;
            lastTarget = 255.0;
            animStartAlpha = 255.0;
            animStartTime = now;
            lastGeometryUpdate = now;
            lastState = FadeState::Dimmed;
        }

        // 2. Geometry & Smart Idle Refresh (1Hz Timer)
        if (now - lastGeometryUpdate > 1000) {
            for (auto& bar : taskbars) {
                if (IsWindow(bar.hwnd)) {
                    RECT r = {0};
                    GetWindowRect(bar.hwnd, &r);
                    // Only update if geometry is valid
                    if (!IsRectEmpty(&r)) {
                        bar.rect = r;
                    }
                }
            }

            // Only check foreground window if Smart Idle is enabled
            if (smartIdle) {
                HWND hForeground = GetForegroundWindow();
                if (hForeground) {
                    WCHAR szClass[64];
                    if (GetClassNameW(hForeground, szClass, 64)) {
                        // Check if focused window is the Desktop or Taskbar itself
                        isDesktopFocused = (wcscmp(szClass, L"Progman") == 0 || 
                                            wcscmp(szClass, L"WorkerW") == 0 || 
                                            wcscmp(szClass, L"Shell_TrayWnd") == 0);
                    } else {
                        isDesktopFocused = true;
                    }
                } else {
                    isDesktopFocused = true;
                }
            } else {
                isDesktopFocused = true; // Always allow idle if smart check disabled
            }

            lastGeometryUpdate = now;
        }

        // 3. Logic: Determine State
        GetLastInputInfo(&lii);
        // 32-bit subtraction to handle rollover
        DWORD idleTime = (DWORD)now - lii.dwTime;

        POINT pt = {0};
        GetCursorPos(&pt);

        bool isHovering = false;
        for (const auto& bar : taskbars) {
            if (PtInRect(&bar.rect, pt)) { 
                isHovering = true; 
                break; 
            }
        }
        
        // Track the most recent time the cursor was over any taskbar
        if (isHovering) {
            lastHoverTime = now;
        }

        FadeState targetState = FadeState::Dimmed;

        // Condition: Enabled AND Timeout reached AND (Smart Idle check passed)
        // Idle ALWAYS takes priority and is never delayed
        if (enableIdleFade && idleTime > idleTimeoutMs && isDesktopFocused) {
            targetState = FadeState::Idle;
        } else if (isHovering) {
            targetState = FadeState::Active;
        } else if ((now - lastHoverTime) < (ULONGLONG)hoverFadeDelayMs) {
            // cursor recently left the taskbar - hold at active until the 
            // hover-fade state delay elapses, then fall through to Dimmed below.
            targetState = FadeState::Active;
        }

        // 4. Reactive Geometry & State Updates
        if (targetState != lastState) {
            // Hide Desktop Icons
            if (hideDesktopIcons) {
                HWND hIcons = GetDesktopListView();
                if (hIcons) {
                    if (targetState == FadeState::Idle) {
                        // Only hide them (and set the flag) if they are currently visible natively
                        if (IsWindowVisible(hIcons)) {
                            ShowWindow(hIcons, SW_HIDE);
                            g_weHidIcons = true;
                        } else {
                            g_weHidIcons = false;
                        }
                    } else if (lastState == FadeState::Idle) {
                        // Waking up: Only show them if we were the ones who hid them
                        if (g_weHidIcons) {
                            ShowWindow(hIcons, SW_SHOW);
                            g_weHidIcons = false;
                        }
                    }
                }
            }

            // Force geometry refresh on state change to ensure hover rects are accurate
            for (auto& bar : taskbars) {
                if (IsWindow(bar.hwnd)) {
                    RECT r = {0};
                    GetWindowRect(bar.hwnd, &r);
                    if (!IsRectEmpty(&r)) {
                        bar.rect = r;
                    }
                }
            }
            lastGeometryUpdate = now;
            lastState = targetState;
        }

        // 5. Target Alpha Calculation
        double target = 255.0;
        int duration = fadeDuration;

        switch (targetState) {
            case FadeState::Active: 
                target = 255.0; 
                duration = fadeDuration; 
                break;
            case FadeState::Idle:   
                target = 0.0;   
                duration = idleFadeDuration; 
                break;
            case FadeState::Dimmed: 
                target = targetDimAlpha; 
                duration = fadeDuration; 
                break;
        }

        // 6. Animation (Stateful Cubic Ease-Out)
        bool isAnimating = false;

        // Exact float comparison (target only takes predefined values)
        if (target != lastTarget) {
            lastTarget = target;
            animStartAlpha = currentAlpha;
            animStartTime = now;
        }

        double dist = std::abs(currentAlpha - target);

        // SNAP THRESHOLD: 1.0 (Prevents sub-pixel CPU churn)
        if (dist >= 1.0) {
            isAnimating = true;

            ULONGLONG elapsed = now - animStartTime;

            if (elapsed >= (ULONGLONG)duration) {
                currentAlpha = target;
            } else {
                double t = (double)elapsed / (double)duration;
                if (t < 0.0) {
                    t = 0.0;
                }
                if (t > 1.0) {
                    t = 1.0;
                }

                double invT = 1.0 - t;
                double ease = 1.0 - (invT * invT * invT);

                currentAlpha = animStartAlpha + ((target - animStartAlpha) * ease);
            }

            int applyAlpha = (int)currentAlpha;
            if (applyAlpha < 0) {
                applyAlpha = 0;
            }
            if (applyAlpha > 255) {
                applyAlpha = 255;
            }

            for (auto& bar : taskbars) {
                UpdateBarOpacity(bar, applyAlpha);
            }
        } else {
            // Snap to exact target
            if (currentAlpha != target) {
                currentAlpha = target;
                for (auto& bar : taskbars) {
                    UpdateBarOpacity(bar, (int)currentAlpha);
                }
            }
        }

        // 7. Dynamic Sleep (same intervals as the original; the wait returns
        // early on stop or a game-mode change)
        if (isAnimating) {
            WaitForSingleObject(g_wakeEvent, (DWORD)frameTimeMs);
        } else {
            WaitForSingleObject(g_wakeEvent, 66);
        }
    }
}

// --- GAME MODE (fork) ---
// A fullscreen window in the foreground hides the taskbar on its monitor, so
// the fade loop has nothing visible to do there. Same detection as
// scroll-window-opacity-gamemode: the window covers its monitor and is
// not a maximized captioned window, or the shell reports exclusive D3D
// fullscreen / busy / presentation mode. Driven by foreground changes plus a
// 1-second check, on its own thread.

// Shell windows never count as fullscreen (desktop, taskbar, Start, and the
// shell's own full-monitor windows such as Task View and Alt+Tab).
static bool GameMode_IsShellWindow(HWND hwnd) {
    WCHAR cls[256] = {};
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    if (_wcsicmp(cls, L"Shell_TrayWnd") == 0 ||
        _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
        _wcsicmp(cls, L"Progman") == 0 ||
        _wcsicmp(cls, L"WorkerW") == 0 ||
        _wcsicmp(cls, L"DV2ControlHost") == 0 ||
        _wcsicmp(cls, L"Windows.UI.Core.CoreWindow") == 0) {
        return true;
    }

    DWORD shellPid = 0;
    DWORD pid = 0;
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray) GetWindowThreadProcessId(tray, &shellPid);
    GetWindowThreadProcessId(hwnd, &pid);
    return shellPid != 0 && pid == shellPid;
}

static bool GameMode_IsFullscreenWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return false;
    }

    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((style & WS_CAPTION) == WS_CAPTION && IsZoomed(hwnd)) {
        return false;
    }

    RECT r;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetWindowRect(hwnd, &r) ||
        !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
        return false;
    }

    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
           r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}

using SHQueryUserNotificationState_t =
    HRESULT(WINAPI*)(QUERY_USER_NOTIFICATION_STATE*);

static bool GameMode_IsD3DFullscreenActive() {
    static const auto query = []() -> SHQueryUserNotificationState_t {
        HMODULE shell32 = GetModuleHandleW(L"shell32.dll");
        if (!shell32) shell32 = LoadLibraryW(L"shell32.dll");
        return shell32 ? (SHQueryUserNotificationState_t)GetProcAddress(
                             shell32, "SHQueryUserNotificationState")
                       : nullptr;
    }();

    QUERY_USER_NOTIFICATION_STATE state;
    return query && SUCCEEDED(query(&state)) &&
           (state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_BUSY ||
            state == QUNS_PRESENTATION_MODE);
}

static bool GameMode_ShouldPause() {
    if (!g_gameModeEnabled) return false;

    HWND fg = GetForegroundWindow();
    if (!fg || GameMode_IsShellWindow(fg)) return false;
    if (!GameMode_IsFullscreenWindow(fg) && !GameMode_IsD3DFullscreenActive()) {
        return false;
    }

    // A taskbar on another monitor stays visible: keep the loop running then.
    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray && MonitorFromWindow(tray, MONITOR_DEFAULTTONEAREST) != mon) {
        return false;
    }
    HWND sec = nullptr;
    while ((sec = FindWindowExW(nullptr, sec, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) {
        if (IsWindowVisible(sec) &&
            MonitorFromWindow(sec, MONITOR_DEFAULTTONEAREST) != mon) {
            return false;
        }
    }
    return true;
}

static void GameMode_Update() {
    const bool pause = GameMode_ShouldPause();
    if (pause != g_gamePaused) {
        g_gamePaused = pause;
        if (pause) {
            Wh_Log(L"Fullscreen window in foreground: fade loop paused");
        } else {
            Wh_Log(L"Fade loop resumed");
        }
        SetEvent(g_wakeEvent);
    }
}

static void CALLBACK GameMode_OnForeground(HWINEVENTHOOK, DWORD, HWND, LONG,
                                           LONG, DWORD, DWORD) {
    GameMode_Update();
}

static void CALLBACK GameMode_OnTimer(HWND, UINT, UINT_PTR, DWORD) {
    GameMode_Update();
}

static DWORD WINAPI GameModeThreadProc(LPVOID) {
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // create queue

    HWINEVENTHOOK foregroundHook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
        GameMode_OnForeground, 0, 0, WINEVENT_OUTOFCONTEXT);
    UINT_PTR timer = SetTimer(nullptr, 0, 1000, GameMode_OnTimer);
    if (!foregroundHook || !timer) {
        Wh_Log(L"Game mode: SetWinEventHook/SetTimer failed: %u", GetLastError());
    }

    GameMode_Update();

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.hwnd == nullptr && msg.message == WM_GAMEMODE_RECHECK) {
            GameMode_Update();
            continue;
        }
        DispatchMessageW(&msg);
    }

    if (timer) KillTimer(nullptr, timer);
    if (foregroundHook) UnhookWinEvent(foregroundHook);
    return 0;
}

static void GameMode_StopThread() {
    if (!g_gameThread) return;
    // The thread may not have created its message queue yet: retry briefly.
    for (int i = 0; i < 100 && !PostThreadMessageW(g_gameThreadId, WM_QUIT, 0, 0); i++) {
        if (WaitForSingleObject(g_gameThread, 10) != WAIT_TIMEOUT) break;
    }
    WaitForSingleObject(g_gameThread, 5000);
    CloseHandle(g_gameThread);
    g_gameThread = nullptr;
    g_gameThreadId = 0;
}

// --- ENTRY POINTS ---

BOOL WhTool_ModInit() {
    LoadSettings();
    g_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_wakeEvent) {
        Wh_Log(L"CreateEvent failed: %u", GetLastError());
        return FALSE;
    }
    g_stopThread = false;
    g_workerThread = std::thread(WorkerLoop);

    g_gameThread = CreateThread(nullptr, 0, GameModeThreadProc, nullptr, 0, &g_gameThreadId);
    if (!g_gameThread) {
        // Without it the fork behaves like the original (never paused).
        Wh_Log(L"Game mode thread: CreateThread failed: %u", GetLastError());
        g_gameThreadId = 0;
    }
    return TRUE;
}

void WhTool_ModUninit() {
    // Game-mode thread first (removes its WinEvent hook and timer), then the
    // worker (woken from any wait, including the paused one).
    GameMode_StopThread();

    g_stopThread = true;
    if (g_wakeEvent) {
        SetEvent(g_wakeEvent);
    }
    if (g_workerThread.joinable()) {
        g_workerThread.join();
    }
    if (g_wakeEvent) {
        CloseHandle(g_wakeEvent);
        g_wakeEvent = nullptr;
    }

    RestoreTaskbarStyle(FindWindow(L"Shell_TrayWnd", NULL));
    HWND hSec = NULL;
    while ((hSec = FindWindowEx(NULL, hSec, L"Shell_SecondaryTrayWnd", NULL)) != NULL) {
        RestoreTaskbarStyle(hSec);
    }

    // Ensure desktop icons are visible when mod unloads,
    // but only if we were the ones who hid them!
    if (g_weHidIcons) {
        HWND hIcons = GetDesktopListView();
        if (hIcons && !IsWindowVisible(hIcons)) {
            ShowWindow(hIcons, SW_SHOW);
        }
    }
}

// --- WINDHAWK TOOL BOILERPLATE (DO NOT EDIT) ---

bool g_isToolModProcessLauncher;
HANDLE g_toolModProcessMutex;

void WINAPI EntryPoint_Hook() {
    Wh_Log(L">");
    ExitThread(0);
}

BOOL Wh_ModInit() {
    DWORD sessionId;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &sessionId) &&
        sessionId == 0) {
        return FALSE;
    }

    bool isExcluded = false;
    bool isToolModProcess = false;
    bool isCurrentToolModProcess = false;
    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLine(), &argc);
    if (!argv) {
        Wh_Log(L"CommandLineToArgvW failed");
        return FALSE;
    }

    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"-service") == 0 ||
            wcscmp(argv[i], L"-service-start") == 0 ||
            wcscmp(argv[i], L"-service-stop") == 0) {
            isExcluded = true;
            break;
        }
    }

    for (int i = 1; i < argc - 1; i++) {
        if (wcscmp(argv[i], L"-tool-mod") == 0) {
            isToolModProcess = true;
            if (wcscmp(argv[i + 1], WH_MOD_ID) == 0) {
                isCurrentToolModProcess = true;
            }
            break;
        }
    }

    LocalFree(argv);

    if (isExcluded) {
        return FALSE;
    }

    if (isCurrentToolModProcess) {
        g_toolModProcessMutex =
            CreateMutex(nullptr, TRUE, L"windhawk-tool-mod_" WH_MOD_ID);
        if (!g_toolModProcessMutex) {
            Wh_Log(L"CreateMutex failed");
            ExitProcess(1);
        }

        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            Wh_Log(L"Tool mod already running (%s)", WH_MOD_ID);
            ExitProcess(1);
        }

        if (!WhTool_ModInit()) {
            ExitProcess(1);
        }

        IMAGE_DOS_HEADER* dosHeader =
            (IMAGE_DOS_HEADER*)GetModuleHandle(nullptr);
        IMAGE_NT_HEADERS* ntHeaders =
            (IMAGE_NT_HEADERS*)((BYTE*)dosHeader + dosHeader->e_lfanew);

        DWORD entryPointRVA = ntHeaders->OptionalHeader.AddressOfEntryPoint;
        void* entryPoint = (BYTE*)dosHeader + entryPointRVA;

        Wh_SetFunctionHook(entryPoint, (void*)EntryPoint_Hook, nullptr);
        return TRUE;
    }

    if (isToolModProcess) {
        return FALSE;
    }

    g_isToolModProcessLauncher = true;
    return TRUE;
}

void Wh_ModAfterInit() {
    if (!g_isToolModProcessLauncher) {
        return;
    }

    WCHAR currentProcessPath[MAX_PATH];
    switch (GetModuleFileName(nullptr, currentProcessPath,
                              ARRAYSIZE(currentProcessPath))) {
        case 0:
        case ARRAYSIZE(currentProcessPath):
            Wh_Log(L"GetModuleFileName failed");
            return;
    }

    WCHAR
    commandLine[MAX_PATH + 2 +
                (sizeof(L" -tool-mod \"" WH_MOD_ID "\"") / sizeof(WCHAR)) - 1];
    swprintf_s(commandLine, L"\"%s\" -tool-mod \"%s\"", currentProcessPath,
               WH_MOD_ID);

    HMODULE kernelModule = GetModuleHandle(L"kernelbase.dll");
    if (!kernelModule) {
        kernelModule = GetModuleHandle(L"kernel32.dll");
        if (!kernelModule) {
            Wh_Log(L"No kernelbase.dll/kernel32.dll");
            return;
        }
    }

    using CreateProcessInternalW_t = BOOL(WINAPI*)(
        HANDLE hUserToken, LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
        LPSECURITY_ATTRIBUTES lpProcessAttributes,
        LPSECURITY_ATTRIBUTES lpThreadAttributes, WINBOOL bInheritHandles,
        DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory,
        LPSTARTUPINFOW lpStartupInfo,
        LPPROCESS_INFORMATION lpProcessInformation,
        PHANDLE hRestrictedUserToken);
    CreateProcessInternalW_t pCreateProcessInternalW =
        (CreateProcessInternalW_t)GetProcAddress(kernelModule,
                                                 "CreateProcessInternalW");
    if (!pCreateProcessInternalW) {
        Wh_Log(L"No CreateProcessInternalW");
        return;
    }

    STARTUPINFO si{
        .cb = sizeof(STARTUPINFO),
        .dwFlags = STARTF_FORCEOFFFEEDBACK,
    };
    PROCESS_INFORMATION pi;
    if (!pCreateProcessInternalW(nullptr, currentProcessPath, commandLine,
                                 nullptr, nullptr, FALSE, NORMAL_PRIORITY_CLASS,
                                 nullptr, nullptr, &si, &pi, nullptr)) {
        Wh_Log(L"CreateProcess failed");
        return;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

void Wh_ModSettingsChanged() {
    if (g_isToolModProcessLauncher) {
        return;
    }

    WhTool_ModSettingsChanged();
}

void Wh_ModUninit() {
    if (g_isToolModProcessLauncher) {
        return;
    }

    WhTool_ModUninit();
    ExitProcess(0);
}
