// Test tray app: adds two notification-area icons (uID 1 = NOTIFYICON_VERSION_4,
// uID 2 = legacy version) and logs every callback message, how it was
// delivered (posted / sent / notify) and how the app is asked to close.
// Usage: traytest.exe <logfile> [hide|exit|prompt] [visible]
//   hide   = ignore WM_CLOSE (like apps that "minimize to tray")   (default)
//   exit   = quit on WM_CLOSE
//   prompt = show a confirmation box on WM_CLOSE
//   visible = also show a normal top-level window
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>

static FILE* g_log;
static int g_mode = 0;  // 0 hide, 1 exit, 2 prompt
static HWND g_hwnd, g_main;

static void Log(const char* fmt, ...) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static const char* How() {
    DWORD f = InSendMessageEx(nullptr);
    if (f == ISMEX_NOSEND) return "posted";
    if (f & ISMEX_NOTIFY) return "notify";
    if (f & ISMEX_CALLBACK) return "callback";
    if (f & ISMEX_SEND) return "sent";
    return "?";
}

static void AddIcon(UINT id, UINT ver) {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = g_hwnd;
    nid.uID = id;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP + id;
    nid.hIcon = LoadIcon(nullptr, id == 1 ? IDI_INFORMATION : IDI_WARNING);
    swprintf(nid.szTip, 128, L"TrayTest %u (v%u)", id, ver);
    BOOL ok = Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = ver;
    BOOL ok2 = Shell_NotifyIconW(NIM_SETVERSION, &nid);
    NOTIFYICONIDENTIFIER nii = {sizeof(nii)};
    nii.hWnd = g_hwnd;
    nii.uID = id;
    RECT rc = {};
    HRESULT hr = Shell_NotifyIconGetRect(&nii, &rc);
    Log("ICON %u add=%d setver=%d rect_hr=0x%08lx rect=%ld,%ld,%ld,%ld", id,
        ok, ok2, hr, rc.left, rc.top, rc.right, rc.bottom);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_APP + 1 || m == WM_APP + 2) {
        UINT ev = (m == WM_APP + 1) ? LOWORD(l) : (UINT)l;
        if (ev != WM_MOUSEMOVE)
            Log("CB msg=WM_APP+%u how=%s wParam=0x%llx lParam=0x%llx ev=0x%x",
                m - WM_APP, How(), (unsigned long long)w,
                (unsigned long long)l, ev);
        return 0;
    }
    switch (m) {
        case WM_CLOSE:
            Log("WM_CLOSE hwnd=%s how=%s", h == g_hwnd ? "owner" : "main",
                How());
            if (g_mode == 1) {
                PostQuitMessage(0);
                return 0;
            }
            if (g_mode == 2) {
                int r = MessageBoxW(nullptr, L"Really quit TrayTest?",
                                    L"TrayTest", MB_YESNO);
                Log("prompt answer=%d", r);
                if (r == IDYES) PostQuitMessage(0);
                return 0;
            }
            if (h == g_main) ShowWindow(h, SW_HIDE);
            return 0;
        case WM_QUERYENDSESSION:
            Log("WM_QUERYENDSESSION lParam=0x%llx", (unsigned long long)l);
            return TRUE;
        case WM_ENDSESSION:
            Log("WM_ENDSESSION w=%llu lParam=0x%llx", (unsigned long long)w,
                (unsigned long long)l);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc < 2) return 1;
    g_log = _wfopen(argv[1], L"a");
    if (!g_log) return 2;
    bool visible = false;
    for (int i = 2; i < argc; i++) {
        if (!lstrcmpiW(argv[i], L"exit")) g_mode = 1;
        if (!lstrcmpiW(argv[i], L"prompt")) g_mode = 2;
        if (!lstrcmpiW(argv[i], L"visible")) visible = true;
    }
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"TrayTestWnd";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    g_hwnd = CreateWindowW(L"TrayTestWnd", L"TrayTest owner", WS_OVERLAPPED, 0,
                           0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (visible) {
        g_main = CreateWindowW(L"TrayTestWnd", L"TrayTest main window",
                               WS_OVERLAPPEDWINDOW, 200, 200, 400, 200, nullptr,
                               nullptr, inst, nullptr);
        ShowWindow(g_main, SW_SHOWNOACTIVATE);
    }
    Log("START pid=%lu owner=0x%llx mode=%d visible=%d", GetCurrentProcessId(),
        (unsigned long long)(ULONG_PTR)g_hwnd, g_mode, visible);
    AddIcon(1, NOTIFYICON_VERSION_4);
    AddIcon(2, 0);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    nid.uID = 2;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    Log("EXIT clean");
    return 0;
}
