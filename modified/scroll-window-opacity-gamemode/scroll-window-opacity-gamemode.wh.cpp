// ==WindhawkMod==
// @id              scroll-window-opacity-gamemode
// @name            Scroll Window Opacity (Game Mode)
// @description     Hold a key combination and scroll the mouse wheel to change the opacity of the window under the cursor. Fullscreen windows (games, F11, videos) are protected.
// @version         1.0.3-gamemode.3
// @author          Sondre Myrmel (fullscreen protection by penicius-code)
// @github          https://github.com/Sondre234
// @twitter         https://x.com/ShaolinLoL
// @include         windhawk.exe
// @architecture    x86
// @architecture    x86-64
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Scroll Window Opacity (Game Mode)

A modified copy of *Scroll Window Opacity* 1.0.3 by Sondre Myrmel. The changes are described
under "Fullscreen protection (this fork)" and "Desktop cost (this fork)" below.
Disable the original mod while this one is enabled.

Hold a configurable key combination (default: **Ctrl + Alt**) and scroll the mouse
wheel to adjust the transparency of the top-level window under the cursor.

- **Scroll up** - decrease opacity (more transparent)
- **Scroll down** - increase opacity (more opaque)

## Fullscreen protection (this fork)

- A fullscreen window (exclusive or borderless fullscreen game, F11 browser,
  fullscreen video) is never made transparent. The wheel event goes through
  to the window unchanged, so Shift/Ctrl + scroll still works in games.
- While a fullscreen window has focus the low-level mouse hook is removed
  completely, so the mod adds nothing to the game's input path.
- If a window this mod made transparent goes fullscreen later, it is put
  back to 100% opacity within a second.

Maximized normal windows (with a title bar) are not treated as fullscreen,
even with an auto-hidden taskbar.

## Desktop cost (this fork)

The original keeps a low-level mouse hook all the time, so every mouse
movement of every app passes through this mod. Here the mouse hook is only
installed while the modifier key(s) are held down; key presses are watched
with keyboard Raw Input, which does not sit in the input path.

Common shell windows such as the taskbar and desktop are excluded. For windows
that this mod makes layered, the original window style is restored when returned
to 100% opacity.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- modifierKey:
  - value: ctrl+alt
    $name: Ctrl + Alt
  - value: ctrl+shift
    $name: Ctrl + Shift
  - value: alt+shift
    $name: Alt + Shift
  - value: ctrl
    $name: Ctrl only
  - value: alt
    $name: Alt only
  - value: shift
    $name: Shift only
  $name: Modifier Key(s)
  $description: Key(s) to hold while scrolling the mouse wheel to change opacity

- opacityStep: 1
  $name: Opacity Step (%)
  $description: How much opacity changes per wheel notch (1-50)

- minOpacity: 10
  $name: Minimum Opacity (%)
  $description: Lowest opacity a window can be set to (1-90)

- protectFullscreen: true
  $name: Protect fullscreen windows
  $description: Never change the opacity of a fullscreen window, and pause the mouse hook while one has focus

- restoreIfFullscreen: true
  $name: Restore when a window goes fullscreen
  $description: If a window made transparent by this mod becomes fullscreen, set it back to 100% opacity
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <shellapi.h>

#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>

struct Settings {
    bool needCtrl            = true;
    bool needAlt             = true;
    bool needShift           = false;
    int  step                = 1;
    int  minPct              = 10;
    bool protectFullscreen   = true;
    bool restoreIfFullscreen = true;
} g_settings;

void LoadSettings() {
    PCWSTR raw = Wh_GetStringSetting(L"modifierKey");
    std::wstring key = (raw && raw[0] != L'\0') ? raw : L"ctrl+alt";
    Wh_FreeStringSetting(raw);

    g_settings.needCtrl  = key.find(L"ctrl")  != std::wstring::npos;
    g_settings.needAlt   = key.find(L"alt")   != std::wstring::npos;
    g_settings.needShift = key.find(L"shift") != std::wstring::npos;

    if (!g_settings.needCtrl && !g_settings.needAlt && !g_settings.needShift) {
        g_settings.needCtrl = true;
        g_settings.needAlt  = true;
    }

    int step = Wh_GetIntSetting(L"opacityStep");
    g_settings.step = (step >= 1 && step <= 50) ? step : 1;

    int minPct = Wh_GetIntSetting(L"minOpacity");
    g_settings.minPct = (minPct >= 1 && minPct <= 90) ? minPct : 10;

    g_settings.protectFullscreen   = Wh_GetIntSetting(L"protectFullscreen") != 0;
    g_settings.restoreIfFullscreen = Wh_GetIntSetting(L"restoreIfFullscreen") != 0;

    Wh_Log(L"Settings: ctrl=%d alt=%d shift=%d step=%d minPct=%d protect=%d restore=%d",
           g_settings.needCtrl,
           g_settings.needAlt,
           g_settings.needShift,
           g_settings.step,
           g_settings.minPct,
           g_settings.protectFullscreen,
           g_settings.restoreIfFullscreen);
}

struct WindowState {
    int  pct;
    bool hadLayered;
};

std::unordered_map<HWND, WindowState> g_states;
std::mutex g_mutex;

static bool IsModifierHeld() {
    if (g_settings.needCtrl  && !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) return false;
    if (g_settings.needAlt   && !(GetAsyncKeyState(VK_MENU)    & 0x8000)) return false;
    if (g_settings.needShift && !(GetAsyncKeyState(VK_SHIFT)   & 0x8000)) return false;
    return true;
}

static bool IsSystemWindow(HWND hwnd) {
    WCHAR cls[256] = {};
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));

    return _wcsicmp(cls, L"Shell_TrayWnd") == 0 ||
           _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
           _wcsicmp(cls, L"Progman") == 0 ||
           _wcsicmp(cls, L"WorkerW") == 0 ||
           _wcsicmp(cls, L"DV2ControlHost") == 0 ||
           _wcsicmp(cls, L"Windows.UI.Core.CoreWindow") == 0;
}

// ---------------------------------------------------------------------------
// Fullscreen detection
// ---------------------------------------------------------------------------

// A window is fullscreen when it covers its whole monitor and is not just a
// maximized normal window (title bar + maximized; with an auto-hidden taskbar
// those also cover the monitor).
static bool IsFullscreenWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd) ||
        IsSystemWindow(hwnd)) {
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

// Exclusive fullscreen D3D app or presentation mode active (applies to the
// foreground window).
static bool IsD3DFullscreenActive() {
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

static bool IsProtectedWindow(HWND hwnd) {
    if (!g_settings.protectFullscreen) return false;
    if (IsFullscreenWindow(hwnd)) return true;
    return hwnd == GetForegroundWindow() && IsD3DFullscreenActive();
}

// ---------------------------------------------------------------------------
// Opacity
// ---------------------------------------------------------------------------

static HWND GetRootWindowAt(POINT pt) {
    HWND hwnd = WindowFromPoint(pt);
    if (!hwnd) {
        return nullptr;
    }

    HWND root = GetAncestor(hwnd, GA_ROOT);
    return root ? root : hwnd;
}

// Puts a window back the way it was before this mod touched it. Caller holds
// g_mutex and erases the map entry.
static void RestoreWindowLocked(HWND hwnd, const WindowState& state) {
    if (!IsWindow(hwnd)) return;

    if (state.hadLayered) {
        if (!SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA)) {
            Wh_Log(L"Restore alpha failed for 0x%p: %u", (void*)hwnd,
                   GetLastError());
        }
    } else {
        LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
        SetLastError(ERROR_SUCCESS);
        LONG_PTR result =
            SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle & ~WS_EX_LAYERED);
        if (result == 0 && GetLastError() != ERROR_SUCCESS) {
            Wh_Log(L"Restore style failed for 0x%p: %u", (void*)hwnd,
                   GetLastError());
        }
    }
}

static bool AdjustOpacity(HWND hwnd, int delta) {
    if (!hwnd || !IsWindow(hwnd) || IsSystemWindow(hwnd) || !IsWindowVisible(hwnd)) {
        return false;
    }

    if (IsProtectedWindow(hwnd)) {
        Wh_Log(L"Window 0x%p is fullscreen: left alone", (void*)hwnd);
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mutex);

    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);

    auto it = g_states.find(hwnd);
    if (it == g_states.end()) {
        bool hadLayered = (exStyle & WS_EX_LAYERED) != 0;
        int curPct = 100;

        if (hadLayered) {
            BYTE alpha = 255;
            DWORD flags = 0;
            if (GetLayeredWindowAttributes(hwnd, nullptr, &alpha, &flags) &&
                (flags & LWA_ALPHA)) {
                curPct = (int)((alpha * 100) / 255);
                if (curPct < 1) curPct = 1;
                if (curPct > 100) curPct = 100;
            }
        }

        g_states[hwnd] = {curPct, hadLayered};
        it = g_states.find(hwnd);
    }

    int oldPct = it->second.pct;
    int newPct = oldPct + delta;

    if (newPct < g_settings.minPct) newPct = g_settings.minPct;
    if (newPct > 100) newPct = 100;

    if (newPct == oldPct) {
        return false;
    }

    if (newPct >= 100 && !it->second.hadLayered) {
        SetLastError(ERROR_SUCCESS);
        LONG_PTR result = SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle & ~WS_EX_LAYERED);
        if (result == 0 && GetLastError() != ERROR_SUCCESS) {
            Wh_Log(L"SetWindowLongPtr(remove layered) failed for 0x%p: %u",
                   (void*)hwnd, GetLastError());
            return false;
        }

        g_states.erase(it);
        Wh_Log(L"Restored window 0x%p to full opacity", (void*)hwnd);
        return true;
    }

    BYTE alpha = (BYTE)((newPct * 255) / 100);

    if (!(exStyle & WS_EX_LAYERED)) {
        SetLastError(ERROR_SUCCESS);
        LONG_PTR result = SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);
        if (result == 0 && GetLastError() != ERROR_SUCCESS) {
            Wh_Log(L"SetWindowLongPtr(add layered) failed for 0x%p: %u",
                   (void*)hwnd, GetLastError());
            return false;
        }
    }

    if (!SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA)) {
        Wh_Log(L"SetLayeredWindowAttributes failed for 0x%p: %u",
               (void*)hwnd, GetLastError());
        return false;
    }

    it->second.pct = newPct;
    Wh_Log(L"Window 0x%p opacity -> %d%%", (void*)hwnd, newPct);
    return true;
}

static void RestoreAllWindows() {
    std::lock_guard<std::mutex> lock(g_mutex);

    for (auto& [hwnd, state] : g_states) {
        RestoreWindowLocked(hwnd, state);
    }

    g_states.clear();
}

// Safety net: drops closed windows from the map and puts any window this mod
// made transparent back to 100% once it has gone fullscreen.
static void RestoreFullscreenWindows() {
    std::lock_guard<std::mutex> lock(g_mutex);

    for (auto it = g_states.begin(); it != g_states.end();) {
        HWND hwnd = it->first;
        if (!IsWindow(hwnd)) {
            it = g_states.erase(it);
            continue;
        }

        if (g_settings.restoreIfFullscreen &&
            (IsFullscreenWindow(hwnd) ||
             (hwnd == GetForegroundWindow() && IsD3DFullscreenActive()))) {
            RestoreWindowLocked(hwnd, it->second);
            Wh_Log(L"Window 0x%p went fullscreen: opacity restored to 100%%",
                   (void*)hwnd);
            it = g_states.erase(it);
            continue;
        }

        ++it;
    }
}

HHOOK  g_mouseHook      = nullptr;
HANDLE g_hookThread     = nullptr;
DWORD  g_hookThreadId   = 0;
int    g_wheelRemainder = 0;
HWND   g_inputWnd       = nullptr;   // message-only window for Raw Input
bool   g_rawKbActive    = false;
bool   g_gamePaused     = false;

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && wParam == WM_MOUSEWHEEL && IsModifierHeld()) {
        auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);

        g_wheelRemainder += GET_WHEEL_DELTA_WPARAM(ms->mouseData);

        int notches = g_wheelRemainder / WHEEL_DELTA;
        g_wheelRemainder %= WHEEL_DELTA;

        if (notches != 0) {
            int delta = (notches > 0 ? -1 : 1) * g_settings.step * std::abs(notches);

            HWND target = GetRootWindowAt(ms->pt);
            if (AdjustOpacity(target, delta)) {
                return 1;
            }
        }
    }

    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Hook management (everything on the hook thread).
//
// Desktop: a WH_MOUSE_LL hook puts every mouse event of every app (1000+/s
// with a gaming mouse) through this process. It is only needed while the
// modifier key(s) are held, so it is installed on modifier key-down and
// removed on key-up. Key presses are seen through keyboard Raw Input
// (RIDEV_INPUTSINK), which is not part of the input hook chain and only fires
// on key events.
//
// Game mode: while a fullscreen window (exclusive or borderless) has focus,
// both the Raw Input registration and the mouse hook are removed, so nothing
// of this mod runs during the game. Driven by foreground-change events plus a
// 1-second check (windows that go fullscreen without a focus change), which
// also runs the restore safety net and fixes a missed key-up.
// ---------------------------------------------------------------------------

static bool GameMode_IsGameInForeground() {
    if (!g_settings.protectFullscreen) return false;
    if (IsD3DFullscreenActive()) return true;
    return IsFullscreenWindow(GetForegroundWindow());
}

static void SetMouseHook(bool on) {
    if (on && !g_mouseHook) {
        g_wheelRemainder = 0;
        g_mouseHook =
            SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, nullptr, 0);
        if (!g_mouseHook) {
            Wh_Log(L"SetWindowsHookEx failed: %u", GetLastError());
        }
    } else if (!on && g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
        g_wheelRemainder = 0;
    }
}

static void SetRawKeyboard(bool on) {
    if (on == g_rawKbActive || !g_inputWnd) return;
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;  // generic desktop
    rid.usUsage     = 0x06;  // keyboard
    rid.dwFlags     = on ? RIDEV_INPUTSINK : RIDEV_REMOVE;
    rid.hwndTarget  = on ? g_inputWnd : nullptr;
    if (RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        g_rawKbActive = on;
    } else {
        Wh_Log(L"RegisterRawInputDevices(%s) failed: %u", on ? L"add" : L"remove",
               GetLastError());
    }
}

static bool IsRequiredModifierVk(USHORT vk) {
    switch (vk) {
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
            return g_settings.needShift;
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
            return g_settings.needCtrl;
        case VK_MENU: case VK_LMENU: case VK_RMENU:
            return g_settings.needAlt;
    }
    return false;
}

// Re-evaluates everything from scratch (focus change, timer, settings).
static void UpdateHooks() {
    RestoreFullscreenWindows();

    const bool game = GameMode_IsGameInForeground();
    if (game != g_gamePaused) {
        g_gamePaused = game;
        Wh_Log(L"%s", game ? L"Fullscreen window in foreground: paused"
                           : L"Fullscreen window gone: resumed");
    }
    SetRawKeyboard(!game);
    // Without the Raw Input window the modifier key can't be watched: keep
    // the hook whenever not paused (original behaviour).
    SetMouseHook(!game && (!g_inputWnd || IsModifierHeld()));
}

static LRESULT CALLBACK InputWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                     LPARAM lParam) {
    if (msg == WM_INPUT) {
        RAWINPUT raw;
        UINT size = sizeof(raw);
        if (!g_gamePaused &&
            GetRawInputData((HRAWINPUT)lParam, RID_INPUT, &raw, &size,
                            sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            raw.header.dwType == RIM_TYPEKEYBOARD &&
            IsRequiredModifierVk(raw.data.keyboard.VKey)) {
            if (!(raw.data.keyboard.Flags & RI_KEY_BREAK)) {
                // Key down (or auto-repeat): be ready before the wheel turns.
                SetMouseHook(true);
            } else if (!IsModifierHeld()) {
                SetMouseHook(false);
            }
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);  // required for WM_INPUT
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void CALLBACK GameMode_OnForeground(HWINEVENTHOOK, DWORD, HWND, LONG,
                                           LONG, DWORD, DWORD) {
    UpdateHooks();
}

static void CALLBACK GameMode_OnTimer(HWND, UINT, UINT_PTR, DWORD) {
    UpdateHooks();
}

DWORD WINAPI HookThreadProc(LPVOID) {
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    WNDCLASSW wc{};
    wc.lpfnWndProc   = InputWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WhScrollOpacityInput";
    RegisterClassW(&wc);
    g_inputWnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                 HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!g_inputWnd) {
        Wh_Log(L"CreateWindowEx failed: %u; using a permanent mouse hook",
               GetLastError());
    }

    HWINEVENTHOOK foregroundHook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
        GameMode_OnForeground, 0, 0, WINEVENT_OUTOFCONTEXT);
    UINT_PTR timer = SetTimer(nullptr, 0, 1000, GameMode_OnTimer);

    UpdateHooks();

    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.hwnd == nullptr && msg.message == WM_APP) {  // settings changed
            UpdateHooks();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (timer) KillTimer(nullptr, timer);
    if (foregroundHook) UnhookWinEvent(foregroundHook);
    SetMouseHook(false);
    SetRawKeyboard(false);
    if (g_inputWnd) {
        DestroyWindow(g_inputWnd);
        g_inputWnd = nullptr;
    }
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    Wh_Log(L"Hooks removed");
    return 0;
}

BOOL WhTool_ModInit() {
    Wh_Log(L"Scroll Window Opacity: init");
    LoadSettings();

    g_hookThread = CreateThread(nullptr, 0, HookThreadProc, nullptr, 0, &g_hookThreadId);
    if (!g_hookThread) {
        Wh_Log(L"CreateThread failed: %u", GetLastError());
        return FALSE;
    }

    return TRUE;
}

void WhTool_ModSettingsChanged() {
    Wh_Log(L"Scroll Window Opacity: settings changed");
    LoadSettings();
    if (g_hookThreadId) {
        PostThreadMessageW(g_hookThreadId, WM_APP, 0, 0);
    }
}

void WhTool_ModUninit() {
    Wh_Log(L"Scroll Window Opacity: uninit");

    if (g_hookThreadId) {
        PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
    }

    if (g_hookThread) {
        WaitForSingleObject(g_hookThread, 5000);
        CloseHandle(g_hookThread);
        g_hookThread = nullptr;
        g_hookThreadId = 0;
    }

    RestoreAllWindows();
}

bool g_isToolModProcessLauncher;
HANDLE g_toolModProcessMutex;

void WINAPI EntryPoint_Hook() {
    Wh_Log(L">");
    ExitThread(0);
}

BOOL Wh_ModInit() {
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

    WCHAR commandLine[MAX_PATH + 2 +
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
