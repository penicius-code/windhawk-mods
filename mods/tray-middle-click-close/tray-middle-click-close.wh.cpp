// ==WindhawkMod==
// @id              tray-middle-click-close
// @name            Middle click to close tray apps
// @description     Close programs with a middle click on their notification area (tray) icon
// @version         1.0.1
// @author          penicius-code
// @github          https://github.com/penicius-code
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -luser32 -lshell32
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Middle click to close tray apps

Companion to "Middle click to close on the taskbar": middle-click an icon in
the notification area (the small icons next to the clock, also inside the ^
overflow flyout) to close the program that owns it.

How a program is closed:
* **Close, then end task if it keeps running** (default): the program's
  windows get a normal close request (like clicking X). Many tray programs only
  hide on close, so if the program is still running after the timeout it is
  ended. If the program opened a new window in the meantime (for example a
  "Save changes?" prompt) it is left alone so you can answer it.
* **Close only**: only the normal close request.
* **End task**: the program is ended right away.

Ctrl + middle click ends the task right away (a setting, on by default; same
key as the taskbar mod). Programs in the exclusion list are never touched. Programs running as
administrator cannot be closed from the tray (Windows blocks it).

The tray icon's own middle-click action (if the program has one) is replaced.
System icons (volume, network, battery, clock) are not affected.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- closeMethod: closeThenEndTask
  $name: Close method
  $options:
  - closeThenEndTask: Close, then end task if it keeps running
  - close: Close only
  - endTask: End task
- endTaskTimeout: 3000
  $name: End task timeout (ms)
  $description: How long to wait after the close request before ending the task
- ctrlEndsTask: true
  $name: Ctrl + middle click ends the task immediately
- excludedPrograms: [""]
  $name: Excluded programs
  $description: File names never closed, for example explorer.exe
- debugLog: false
  $name: Debug log file
  $description: Write a log to %TEMP%\tray-middle-click-close.log
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>

#include <shellapi.h>

#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <vector>

enum class CloseMethod { CloseThenEndTask, Close, EndTask };

struct Settings {
    CloseMethod closeMethod = CloseMethod::CloseThenEndTask;
    DWORD endTaskTimeout = 3000;
    bool ctrlEndsTask = true;
    std::vector<std::wstring> excluded;
    bool debugLog = false;
};

Settings g_settings;
std::mutex g_settingsMutex;

// ---------------------------------------------------------------- logging

std::mutex g_logMutex;

void DebugLog(PCWSTR fmt, ...) {
    WCHAR buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    Wh_Log(L"%s", buf);

    bool enabled;
    {
        std::lock_guard lock(g_settingsMutex);
        enabled = g_settings.debugLog;
    }
    if (!enabled) {
        return;
    }

    WCHAR path[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, path);
    if (!n || n > MAX_PATH - 40) {
        return;
    }
    wcscat_s(path, L"tray-middle-click-close.log");

    SYSTEMTIME st;
    GetLocalTime(&st);
    WCHAR line[1200];
    int len = swprintf_s(line, L"%02d:%02d:%02d.%03d [%lu] %s\r\n", st.wHour,
                         st.wMinute, st.wSecond, st.wMilliseconds,
                         GetCurrentThreadId(), buf);
    if (len <= 0) {
        return;
    }
    char utf8[2400];
    int bytes = WideCharToMultiByte(CP_UTF8, 0, line, len, utf8, sizeof(utf8),
                                    nullptr, nullptr);

    std::lock_guard lock(g_logMutex);
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(f, utf8, bytes, &written, nullptr);
        CloseHandle(f);
    }
}

// ---------------------------------------------------------------- helpers

bool IsTrayWindowUnderCursor() {
    POINT pt;
    if (!GetCursorPos(&pt)) {
        return false;
    }
    HWND hwnd = WindowFromPoint(pt);
    if (!hwnd) {
        return false;
    }
    HWND root = GetAncestor(hwnd, GA_ROOT);
    WCHAR cls[64];
    if (!root || !GetClassNameW(root, cls, ARRAYSIZE(cls))) {
        return false;
    }
    return wcscmp(cls, L"Shell_TrayWnd") == 0 ||
           wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
           wcscmp(cls, L"TopLevelWindowForOverflowXamlIsland") == 0 ||
           wcscmp(cls, L"NotifyIconOverflowWindow") == 0;  // Win10
}

std::wstring ProcessFileName(HANDLE process) {
    WCHAR path[MAX_PATH];
    DWORD size = ARRAYSIZE(path);
    if (!QueryFullProcessImageNameW(process, 0, path, &size)) {
        return L"";
    }
    PCWSTR name = wcsrchr(path, L'\\');
    return name ? name + 1 : path;
}

struct EnumData {
    DWORD pid;
    std::vector<HWND> windows;
};

// Visible, unowned top-level windows of the process (what the user sees as
// the program's windows).
std::vector<HWND> VisibleWindowsOf(DWORD pid) {
    EnumData data{pid};
    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            auto* d = (EnumData*)lParam;
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == d->pid && IsWindowVisible(hwnd)) {
                d->windows.push_back(hwnd);
            }
            return TRUE;
        },
        (LPARAM)&data);
    return data.windows;
}

void RemoveGhostIcon(HWND owner, UINT id1, UINT id2) {
    // The owner window is gone, so these only remove the stale icon left by
    // a process that was ended. Shell_NotifyIcon talks to Shell_TrayWnd,
    // which lives on another thread of this process, so this is safe here.
    for (UINT id : {id1, id2}) {
        NOTIFYICONDATAW nid = {sizeof(nid)};
        nid.hWnd = owner;
        nid.uID = id;
        BOOL ok = Shell_NotifyIconW(NIM_DELETE, &nid);
        DebugLog(L"ghost icon delete hwnd=%p id=%u -> %d", owner, id, ok);
        if (id1 == id2) {
            break;
        }
    }
}

// ---------------------------------------------------------------- closing

struct CloseRequest {
    HWND owner;
    UINT id1;  // uID if the icon uses NOTIFYICON_VERSION_4
    UINT id2;  // uID if the icon uses the legacy version
    bool forceEndTask;
};

void DoClose(LPVOID param) {
    CloseRequest req = *(CloseRequest*)param;
    delete (CloseRequest*)param;

    Settings s;
    {
        std::lock_guard lock(g_settingsMutex);
        s = g_settings;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(req.owner, &pid);
    if (!pid || pid == GetCurrentProcessId()) {
        DebugLog(L"skip: owner %p pid %lu", req.owner, pid);
        return;
    }

    HANDLE process = OpenProcess(
        PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, pid);
    if (!process) {
        process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                              FALSE, pid);
    }
    std::wstring name = process ? ProcessFileName(process) : L"";
    DebugLog(L"close request: pid=%lu name=%s owner=%p force=%d method=%d",
             pid, name.c_str(), req.owner, req.forceEndTask,
             (int)s.closeMethod);

    for (const auto& ex : s.excluded) {
        if (!ex.empty() && _wcsicmp(ex.c_str(), name.c_str()) == 0) {
            DebugLog(L"excluded: %s", name.c_str());
            if (process) {
                CloseHandle(process);
            }
                return;
        }
    }

    CloseMethod method =
        req.forceEndTask ? CloseMethod::EndTask : s.closeMethod;

    bool ended = false;

    if (method != CloseMethod::EndTask) {
        std::vector<HWND> before = VisibleWindowsOf(pid);
        std::set<HWND> targets(before.begin(), before.end());
        targets.insert(req.owner);
        for (HWND hwnd : targets) {
            // Close only top-level windows that are not owned by another
            // window (dialogs close with their owner).
            if (hwnd != req.owner && GetWindow(hwnd, GW_OWNER)) {
                continue;
            }
            BOOL ok = PostMessageW(hwnd, WM_CLOSE, 0, 0);
            DebugLog(L"WM_CLOSE -> %p (%d)", hwnd, ok);
        }

        if (method == CloseMethod::Close || !process) {
            if (process && WaitForSingleObject(process, s.endTaskTimeout) ==
                               WAIT_OBJECT_0) {
                DebugLog(L"process exited after close");
            }
            if (process) {
                CloseHandle(process);
            }
                return;
        }

        if (WaitForSingleObject(process, s.endTaskTimeout) == WAIT_OBJECT_0) {
            DebugLog(L"process exited after close");
            CloseHandle(process);
                return;
        }

        // Still running. If it put up a new window (a confirmation or "save
        // changes?" prompt), leave the decision to the user.
        std::set<HWND> old(before.begin(), before.end());
        for (HWND hwnd : VisibleWindowsOf(pid)) {
            if (!old.count(hwnd)) {
                WCHAR title[128] = L"";
                GetWindowTextW(hwnd, title, ARRAYSIZE(title));
                DebugLog(L"new window %p '%s' appeared, not ending task", hwnd,
                         title);
                CloseHandle(process);
                        return;
            }
        }
    }

    if (!process) {
        DebugLog(L"cannot open process %lu", pid);
        return;
    }

    if (TerminateProcess(process, 1)) {
        DebugLog(L"ended task %s", name.c_str());
        ended = WaitForSingleObject(process, 2000) == WAIT_OBJECT_0;
    } else {
        DebugLog(L"TerminateProcess failed: %lu (elevated program?)",
                 GetLastError());
    }
    CloseHandle(process);

    if (ended) {
        RemoveGhostIcon(req.owner, req.id1, req.id2);
    }

    return;
}

DWORD WINAPI CloseWorker(LPVOID param) {
    DoClose(param);
    // The thread holds a reference on the mod DLL (taken when it was
    // started), so the mod cannot be unloaded under it.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)CloseWorker, &self);
    FreeLibraryAndExitThread(self, 0);
}

// Returns true if the message is a forwarded middle click on a tray icon and
// was handled (swallowed).
bool HandleTrayCallback(PCWSTR via, HWND hWnd, UINT Msg, WPARAM wParam,
                        LPARAM lParam) {
    // Tray callback messages are app-defined (WM_USER and above). The event
    // is the low word of lParam (NOTIFYICON_VERSION_4: high word = icon id;
    // legacy: high word = 0 and wParam = icon id).
    if (Msg < WM_USER || !hWnd) {
        return false;
    }
    UINT event = LOWORD(lParam);
    if (event != WM_MBUTTONDOWN && event != WM_MBUTTONUP &&
        event != WM_MBUTTONDBLCLK) {
        return false;
    }
    if ((ULONG_PTR)lParam >> 32) {
        return false;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);
    if (!pid || pid == GetCurrentProcessId()) {
        return false;
    }

    if (!IsTrayWindowUnderCursor()) {
        DebugLog(L"%s msg=0x%X lParam=0x%IX to %p: cursor not over tray, "
                 L"ignored",
                 via, Msg, lParam, hWnd);
        return false;
    }

    DebugLog(L"%s tray middle event 0x%X msg=0x%X wParam=0x%IX lParam=0x%IX "
             L"hwnd=%p pid=%lu",
             via, event, Msg, wParam, lParam, hWnd, pid);

    if (event != WM_MBUTTONUP) {
        return true;  // swallow down / double-click, act on the release
    }

    bool force = false;
    {
        std::lock_guard lock(g_settingsMutex);
        force = g_settings.ctrlEndsTask && GetKeyState(VK_CONTROL) < 0;
    }

    auto* req = new CloseRequest{hWnd, HIWORD(lParam), (UINT)wParam, force};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (LPCWSTR)CloseWorker, &self);  // +1 ref for the thread
    HANDLE thread = CreateThread(nullptr, 0, CloseWorker, req, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        delete req;
        if (self) {
            FreeLibrary(self);
        }
    }
    return true;
}

// ---------------------------------------------------------------- hooks

using SendNotifyMessageW_t = decltype(&SendNotifyMessageW);
SendNotifyMessageW_t SendNotifyMessageW_Original;
BOOL WINAPI SendNotifyMessageW_Hook(HWND hWnd,
                                    UINT Msg,
                                    WPARAM wParam,
                                    LPARAM lParam) {
    if (HandleTrayCallback(L"notify", hWnd, Msg, wParam, lParam)) {
        return TRUE;
    }
    return SendNotifyMessageW_Original(hWnd, Msg, wParam, lParam);
}

using PostMessageW_t = decltype(&PostMessageW);
PostMessageW_t PostMessageW_Original;
BOOL WINAPI PostMessageW_Hook(HWND hWnd,
                              UINT Msg,
                              WPARAM wParam,
                              LPARAM lParam) {
    if (HandleTrayCallback(L"post", hWnd, Msg, wParam, lParam)) {
        return TRUE;
    }
    return PostMessageW_Original(hWnd, Msg, wParam, lParam);
}

// ---------------------------------------------------------------- settings

void LoadSettings() {
    Settings s;

    PCWSTR method = Wh_GetStringSetting(L"closeMethod");
    if (wcscmp(method, L"close") == 0) {
        s.closeMethod = CloseMethod::Close;
    } else if (wcscmp(method, L"endTask") == 0) {
        s.closeMethod = CloseMethod::EndTask;
    }
    Wh_FreeStringSetting(method);

    int timeout = Wh_GetIntSetting(L"endTaskTimeout");
    s.endTaskTimeout = timeout > 0 ? timeout : 3000;
    s.ctrlEndsTask = Wh_GetIntSetting(L"ctrlEndsTask");
    s.debugLog = Wh_GetIntSetting(L"debugLog");

    for (int i = 0;; i++) {
        PCWSTR ex = Wh_GetStringSetting(L"excludedPrograms[%d]", i);
        bool empty = !*ex;
        if (!empty) {
            s.excluded.push_back(ex);
        }
        Wh_FreeStringSetting(ex);
        if (empty) {
            break;
        }
    }

    std::lock_guard lock(g_settingsMutex);
    g_settings = std::move(s);
}

BOOL Wh_ModInit() {
    LoadSettings();
    DebugLog(L"init");

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    WindhawkUtils::Wh_SetFunctionHookT(
        (SendNotifyMessageW_t)GetProcAddress(user32, "SendNotifyMessageW"),
        SendNotifyMessageW_Hook, &SendNotifyMessageW_Original);
    WindhawkUtils::Wh_SetFunctionHookT(
        (PostMessageW_t)GetProcAddress(user32, "PostMessageW"),
        PostMessageW_Hook, &PostMessageW_Original);
    return TRUE;
}

void Wh_ModUninit() {
    DebugLog(L"uninit");
}

void Wh_ModSettingsChanged() {
    LoadSettings();
    DebugLog(L"settings changed");
}
