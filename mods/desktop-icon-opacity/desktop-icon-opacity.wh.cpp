// ==WindhawkMod==
// @id              desktop-icon-opacity
// @name            Desktop Icon Opacity
// @description     Make desktop icons semi-transparent, optionally back to full opacity when hovered or selected
// @version         1.0.0
// @author          penicius-code
// @github          https://github.com/penicius-code
// @include         explorer.exe
// @compilerOptions -lcomctl32
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Desktop Icon Opacity

Fades the icons on the desktop. Only the icon images are affected (including
overlays such as the shortcut arrow) - labels and the selection highlight are
left alone, so this works side by side with *Desktop Icon Selection Style*.

Optionally, an icon returns to full opacity while the mouse is over it or while
it is selected.

Desktop-attach code adapted from *Desktop Icon Selection Style* by RiteshK (MIT).
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- iconOpacity: 50
  $name: Icon opacity
  $description: Opacity of desktop icons in percent. 100 is the stock look, 0 hides the icon image entirely (the label stays).
- fullOpacityOnHover: true
  $name: Full opacity on hover
  $description: Show an icon at full opacity while the mouse is over it.
- fullOpacityOnSelect: true
  $name: Full opacity when selected
  $description: Show selected icons at full opacity.
*/
// ==/WindhawkModSettings==

#include <commctrl.h>
#include <commoncontrols.h>
#include <psapi.h>

#include <windhawk_utils.h>

#include <algorithm>
#include <atomic>

#ifndef ILS_ALPHA
#define ILS_ALPHA 0x00000008
#endif

struct {
    int iconOpacity;
    bool fullOnHover;
    bool fullOnSelect;
} g_settings;

std::atomic<HWND> g_lv{nullptr};

// Non-zero while the desktop list view is painting on this thread. Icon draws
// are only touched inside that window, so image lists drawn anywhere else in
// Explorer (menus, the taskbar, folder windows) are never affected.
thread_local int g_lvPaintDepth = 0;

// ImageList_DrawIndirect forwards to IImageList::Draw, and both are hooked so a
// caller using either path is caught. This flag stops the inner one applying
// the alpha a second time.
thread_local bool g_inImageListDraw = false;

using ImageList_GetIconSize_t = BOOL(WINAPI*)(HIMAGELIST, int*, int*);
ImageList_GetIconSize_t g_getIconSize;

////////////////////////////////////////////////////////////////////////////////
// Deciding the opacity for one icon draw

// The list view paints through a buffered-paint DC whose coordinates match the
// window's client area, so the draw position can be hit-tested directly.
bool IsHotOrSelected(const IMAGELISTDRAWPARAMS* p) {
    HWND lv = g_lv.load(std::memory_order_relaxed);
    if (!lv) {
        return false;
    }

    int cx = p->cx;
    int cy = p->cy;
    if ((cx <= 0 || cy <= 0) && g_getIconSize && p->himl) {
        g_getIconSize(p->himl, &cx, &cy);
    }

    LVHITTESTINFO hti{};
    hti.pt.x = p->x + cx / 2;
    hti.pt.y = p->y + cy / 2;
    int index = ListView_HitTest(lv, &hti);
    if (index < 0) {
        return false;
    }

    if (g_settings.fullOnHover && ListView_GetHotItem(lv) == index) {
        return true;
    }

    if (g_settings.fullOnSelect &&
        (ListView_GetItemState(lv, index, LVIS_SELECTED) & LVIS_SELECTED)) {
        return true;
    }

    return false;
}

int OpacityFor(const IMAGELISTDRAWPARAMS* p) {
    if (g_lvPaintDepth <= 0) {
        return 100;
    }

    int opacity = g_settings.iconOpacity;
    if (opacity >= 100) {
        return 100;
    }

    if ((g_settings.fullOnHover || g_settings.fullOnSelect) &&
        IsHotOrSelected(p)) {
        return 100;
    }

    return opacity;
}

// Works on a copy, never the caller's struct. Older callers can pass the short
// V3 struct without fState/Frame, so only cbSize bytes are copied.
void ApplyOpacity(const IMAGELISTDRAWPARAMS* in,
                  IMAGELISTDRAWPARAMS* out,
                  int opacity) {
    ZeroMemory(out, sizeof(*out));
    UINT size = std::min<UINT>(in->cbSize, sizeof(*out));
    memcpy(out, in, size);
    out->cbSize = sizeof(*out);

    DWORD alpha = (DWORD)MulDiv(opacity, 255, 100);
    if (out->fState & ILS_ALPHA) {
        // Already translucent (rare) - multiply rather than override.
        out->Frame = (DWORD)MulDiv((int)out->Frame, (int)alpha, 255);
    } else {
        out->fState |= ILS_ALPHA;
        out->Frame = alpha;
    }
}

////////////////////////////////////////////////////////////////////////////////
// Hooks

using ImageList_DrawIndirect_t = BOOL(WINAPI*)(IMAGELISTDRAWPARAMS*);
ImageList_DrawIndirect_t ImageList_DrawIndirect_Original;

BOOL WINAPI ImageList_DrawIndirect_Hook(IMAGELISTDRAWPARAMS* p) {
    if (!p || g_inImageListDraw || g_lvPaintDepth <= 0) {
        return ImageList_DrawIndirect_Original(p);
    }

    g_inImageListDraw = true;
    BOOL result;
    int opacity = OpacityFor(p);
    if (opacity < 100) {
        IMAGELISTDRAWPARAMS copy;
        ApplyOpacity(p, &copy, opacity);
        result = ImageList_DrawIndirect_Original(&copy);
    } else {
        result = ImageList_DrawIndirect_Original(p);
    }
    g_inImageListDraw = false;
    return result;
}

using IImageList_Draw_t = HRESULT(STDMETHODCALLTYPE*)(void*, IMAGELISTDRAWPARAMS*);
IImageList_Draw_t IImageList_Draw_Original;

HRESULT STDMETHODCALLTYPE IImageList_Draw_Hook(void* self,
                                               IMAGELISTDRAWPARAMS* p) {
    if (!p || g_inImageListDraw || g_lvPaintDepth <= 0) {
        return IImageList_Draw_Original(self, p);
    }

    g_inImageListDraw = true;
    HRESULT result;
    int opacity = OpacityFor(p);
    if (opacity < 100) {
        IMAGELISTDRAWPARAMS copy;
        ApplyOpacity(p, &copy, opacity);
        result = IImageList_Draw_Original(self, &copy);
    } else {
        result = IImageList_Draw_Original(self, p);
    }
    g_inImageListDraw = false;
    return result;
}

////////////////////////////////////////////////////////////////////////////////
// Finding comctl32 v6
//
// Image list alpha (ILS_ALPHA) and IImageList only exist in v6, and Explorer can
// have both v5 and v6 loaded under the same file name. The one exporting
// HIMAGELIST_QueryInterface is v6.

HMODULE FindComctl32v6() {
    HMODULE byName = LoadLibraryW(L"comctl32.dll");
    if (byName && GetProcAddress(byName, "HIMAGELIST_QueryInterface")) {
        return byName;
    }

    HMODULE modules[1024];
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules),
                               &needed)) {
        return nullptr;
    }

    DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), ARRAYSIZE(modules));
    for (DWORD i = 0; i < count; i++) {
        WCHAR name[MAX_PATH];
        if (K32GetModuleBaseNameW(GetCurrentProcess(), modules[i], name,
                                  ARRAYSIZE(name)) &&
            _wcsicmp(name, L"comctl32.dll") == 0 &&
            GetProcAddress(modules[i], "HIMAGELIST_QueryInterface")) {
            return modules[i];
        }
    }

    return nullptr;
}

// IImageList::Draw is reached through the image list object's vtable: IUnknown
// (3 slots), then Add, ReplaceIcon, SetOverlayImage, Replace, AddMasked, Draw.
void* FindImageListDraw(HMODULE comctl) {
    using Create_t = HIMAGELIST(WINAPI*)(int, int, UINT, int, int);
    using Destroy_t = BOOL(WINAPI*)(HIMAGELIST);
    using QI_t = HRESULT(WINAPI*)(HIMAGELIST, REFIID, void**);

    auto create = (Create_t)GetProcAddress(comctl, "ImageList_Create");
    auto destroy = (Destroy_t)GetProcAddress(comctl, "ImageList_Destroy");
    auto qi = (QI_t)GetProcAddress(comctl, "HIMAGELIST_QueryInterface");
    if (!create || !destroy || !qi) {
        return nullptr;
    }

    // {46EB5926-582E-4017-9FDF-E8998DAA0950}
    static const IID kIID_IImageList = {
        0x46eb5926, 0x582e, 0x4017,
        {0x9f, 0xdf, 0xe8, 0x99, 0x8d, 0xaa, 0x09, 0x50}};

    HIMAGELIST il = create(16, 16, ILC_COLOR32, 1, 1);
    if (!il) {
        return nullptr;
    }

    void* draw = nullptr;
    IUnknown* unk = nullptr;
    if (SUCCEEDED(qi(il, kIID_IImageList, (void**)&unk)) && unk) {
        void** vtbl = *(void***)unk;
        draw = vtbl[8];
        unk->Release();
    }

    destroy(il);
    return draw;
}

////////////////////////////////////////////////////////////////////////////////
// Finding and subclassing the desktop list view

bool IsDesktopListView(HWND hWnd) {
    WCHAR buffer[64];

    if (!GetClassNameW(hWnd, buffer, ARRAYSIZE(buffer)) ||
        _wcsicmp(buffer, L"SysListView32") != 0) {
        return false;
    }

    if (!GetWindowTextW(hWnd, buffer, ARRAYSIZE(buffer)) ||
        _wcsicmp(buffer, L"FolderView") != 0) {
        return false;
    }

    HWND shellView = GetAncestor(hWnd, GA_PARENT);
    if (!shellView || !GetClassNameW(shellView, buffer, ARRAYSIZE(buffer)) ||
        _wcsicmp(buffer, L"SHELLDLL_DefView") != 0) {
        return false;
    }

    HWND top = GetAncestor(shellView, GA_PARENT);
    if (!top) {
        return false;
    }

    if (top == GetShellWindow()) {
        return true;
    }

    if (!GetClassNameW(top, buffer, ARRAYSIZE(buffer))) {
        return false;
    }

    return _wcsicmp(buffer, L"Progman") == 0 || _wcsicmp(buffer, L"WorkerW") == 0;
}

HWND ListViewUnder(HWND shellView) {
    if (!shellView) {
        return nullptr;
    }

    HWND listView = FindWindowExW(shellView, nullptr, L"SysListView32", nullptr);
    if (!listView) {
        return nullptr;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(listView, &pid);
    return pid == GetCurrentProcessId() ? listView : nullptr;
}

HWND GetExistingDesktopListView() {
    if (HWND listView =
            ListViewUnder(FindWindowExW(GetShellWindow(), nullptr,
                                        L"SHELLDLL_DefView", nullptr))) {
        return listView;
    }

    HWND worker = nullptr;
    while ((worker = FindWindowExW(nullptr, worker, L"WorkerW", nullptr)) !=
           nullptr) {
        if (HWND listView = ListViewUnder(FindWindowExW(
                worker, nullptr, L"SHELLDLL_DefView", nullptr))) {
            return listView;
        }
    }

    return nullptr;
}

LRESULT CALLBACK LvSubclassProc(HWND hWnd,
                                UINT uMsg,
                                WPARAM wParam,
                                LPARAM lParam,
                                DWORD_PTR dwRefData) {
    switch (uMsg) {
        case WM_NCDESTROY: {
            HWND self = hWnd;
            g_lv.compare_exchange_strong(self, nullptr, std::memory_order_relaxed);
            break;
        }

        case WM_PAINT:
        case WM_PRINTCLIENT: {
            g_lvPaintDepth++;
            LRESULT result = DefSubclassProc(hWnd, uMsg, wParam, lParam);
            g_lvPaintDepth--;
            return result;
        }
    }

    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

void AttachToListView(HWND hWnd) {
    if (!WindhawkUtils::SetWindowSubclassFromAnyThread(hWnd, LvSubclassProc, 0)) {
        return;
    }

    HWND previous = g_lv.exchange(hWnd, std::memory_order_relaxed);
    if (previous && previous != hWnd) {
        WindhawkUtils::RemoveWindowSubclassFromAnyThread(previous, LvSubclassProc);
    }

    InvalidateRect(hWnd, nullptr, TRUE);
}

using CreateWindowExW_t = decltype(&CreateWindowExW);
CreateWindowExW_t CreateWindowExW_Original;

HWND WINAPI CreateWindowExW_Hook(DWORD dwExStyle,
                                 LPCWSTR lpClassName,
                                 LPCWSTR lpWindowName,
                                 DWORD dwStyle,
                                 int X,
                                 int Y,
                                 int nWidth,
                                 int nHeight,
                                 HWND hWndParent,
                                 HMENU hMenu,
                                 HINSTANCE hInstance,
                                 PVOID lpParam) {
    HWND hWnd = CreateWindowExW_Original(dwExStyle, lpClassName, lpWindowName,
                                         dwStyle, X, Y, nWidth, nHeight,
                                         hWndParent, hMenu, hInstance, lpParam);
    if (!hWnd) {
        return hWnd;
    }

    if (!IS_INTRESOURCE(lpClassName) &&
        _wcsicmp(lpClassName, L"SysListView32") != 0) {
        return hWnd;
    }

    if (IsDesktopListView(hWnd)) {
        AttachToListView(hWnd);
    }

    return hWnd;
}

////////////////////////////////////////////////////////////////////////////////
// Settings and entry points

void LoadSettings() {
    decltype(g_settings) s{};
    s.iconOpacity = std::clamp(Wh_GetIntSetting(L"iconOpacity"), 0, 100);
    s.fullOnHover = Wh_GetIntSetting(L"fullOpacityOnHover");
    s.fullOnSelect = Wh_GetIntSetting(L"fullOpacityOnSelect");
    g_settings = s;
}

BOOL Wh_ModInit() {
    Wh_Log(L"Init");

    LoadSettings();

    HMODULE comctl = FindComctl32v6();
    if (!comctl) {
        Wh_Log(L"comctl32 v6 not found");
        return FALSE;
    }

    g_getIconSize =
        (ImageList_GetIconSize_t)GetProcAddress(comctl, "ImageList_GetIconSize");

    bool hooked = false;

    auto drawIndirect = (ImageList_DrawIndirect_t)GetProcAddress(
        comctl, "ImageList_DrawIndirect");
    if (drawIndirect &&
        WindhawkUtils::SetFunctionHook(drawIndirect, ImageList_DrawIndirect_Hook,
                                       &ImageList_DrawIndirect_Original)) {
        hooked = true;
    } else {
        Wh_Log(L"Failed to hook ImageList_DrawIndirect");
    }

    if (auto draw = (IImageList_Draw_t)FindImageListDraw(comctl)) {
        if (WindhawkUtils::SetFunctionHook(draw, IImageList_Draw_Hook,
                                           &IImageList_Draw_Original)) {
            hooked = true;
        } else {
            Wh_Log(L"Failed to hook IImageList::Draw");
        }
    } else {
        Wh_Log(L"IImageList::Draw not found");
    }

    if (!hooked) {
        return FALSE;
    }

    if (!WindhawkUtils::SetFunctionHook(CreateWindowExW, CreateWindowExW_Hook,
                                        &CreateWindowExW_Original)) {
        Wh_Log(L"Failed to hook CreateWindowExW");
        return FALSE;
    }

    return TRUE;
}

void Wh_ModAfterInit() {
    if (g_lv.load(std::memory_order_relaxed)) {
        return;
    }

    if (HWND listView = GetExistingDesktopListView()) {
        AttachToListView(listView);
    }
}

void Wh_ModUninit() {
    Wh_Log(L"Uninit");

    if (HWND listView = g_lv.exchange(nullptr, std::memory_order_relaxed)) {
        WindhawkUtils::RemoveWindowSubclassFromAnyThread(listView, LvSubclassProc);
        if (IsWindow(listView)) {
            InvalidateRect(listView, nullptr, TRUE);
        }
    }
}

void Wh_ModSettingsChanged() {
    Wh_Log(L"SettingsChanged");

    LoadSettings();

    HWND listView = g_lv.load(std::memory_order_relaxed);
    if (!listView) {
        if (HWND found = GetExistingDesktopListView()) {
            AttachToListView(found);
            return;
        }
    }

    if (listView) {
        InvalidateRect(listView, nullptr, TRUE);
    }
}
