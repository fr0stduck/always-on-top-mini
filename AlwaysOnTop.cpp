// AlwaysOnTop Mini — 独立的"窗口置顶"小工具
// 核心逻辑剥离自 microsoft/PowerToys 的 Always On Top 模块（纯 Win32，无 PowerToys 依赖）
//   剥离掉：遥测/ETW、GPO 组策略、提权通知、游戏模式、集中热键服务(LLKH)、虚拟桌面判断
//   保留  ：热键置顶、置顶高亮边框、热键调透明度、托盘退出
//
// 热键：
//   Shift+Win+T       切换前台窗口置顶（被占用时自动回退 Alt+Win+T）
//   Win+Ctrl+上/下    增加/降低已置顶前台窗口的透明度
// 托盘右键菜单：
//   "选择窗口置顶"    全屏遮罩 + 十字光标，点击任意窗口即可置顶/取消（无需先激活该窗口）
//   "退出"
// 单置顶模式：全局只保留一个置顶窗口，钉新窗口时自动取消上一个。
//
// 编译（zig cc / MSVC 均可）:
//   zig c++ -O2 -target x86_64-windows-gnu -municode AlwaysOnTop.cpp -o AlwaysOnTop.exe \
//       -lgdi32 -ldwmapi -lshell32 -Wl,--subsystem,windows
//   MSVC: cl /O2 /EHsc /DUNICODE /D_UNICODE AlwaysOnTop.cpp user32.lib gdi32.lib \
//       dwmapi.lib shell32.lib /link /SUBSYSTEM:WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <unordered_map>

// ---------- 常量 ----------
static const wchar_t kHostClass[]   = L"AOTMini_Host";
static const wchar_t kBorderClass[] = L"AOTMini_Border";
static const wchar_t kPickClass[]   = L"AOTMini_Pick";
static const wchar_t kPinProp[]     = L"AOTMini_Pinned";
static const wchar_t kMutexName[]   = L"Local\\AOTMini_InstanceMutex";
static const UINT    WM_APP_TRAY    = WM_APP + 1;
static const int     ID_HOTKEY_PIN  = 1;
static const int     ID_HOTKEY_OP_UP = 2;
static const int     ID_HOTKEY_OP_DOWN = 3;
static const int     IDM_EXIT       = 100;
static const int     IDM_PICK       = 101;
static bool          g_hotkeyFallback = false; // 置顶热键被占用，已回退 Alt+Win+T
static const COLORREF BORDER_COLOR  = RGB(255, 128, 0); // 高亮边框颜色（橙）
static const int     TRANS_STEP     = 10;               // 每次透明度步进 (%)

// ---------- 全局 ----------
struct App {
    HINSTANCE hInst = nullptr;
    HWND host = nullptr;
    UINT sysDpi = 96;
    std::unordered_map<HWND, HWND> pinned;   // 目标窗口 -> 边框窗口（单置顶模式下至多一项）
    std::unordered_map<HWND, RECT> originalLayered; // 原始分层窗口状态
    NOTIFYICONDATAW nid = {};
    HWINEVENTHOOK hooks[6] = {};
};
static App g;

// ---------- 小工具 ----------
static int BorderThickness() { return MulDiv(3, (int)g.sysDpi, 96); }

static bool GetTrueRect(HWND hwnd, RECT* rc)
{
    // 用 DWM 扩展边界，排除 Win11/Win10 窗口阴影导致的偏移（同 PowerToys 做法）
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, rc, sizeof(RECT))))
        return true;
    return GetWindowRect(hwnd, rc) != FALSE;
}

static bool IsPinned(HWND hwnd)
{
    return GetPropW(hwnd, kPinProp) != nullptr;
}

// 过滤系统外壳窗口（桌面/任务栏），避免把它们钉起来
static bool IsShellWindow(HWND hwnd)
{
    wchar_t cls[64] = {};
    if (!GetClassNameW(hwnd, cls, 64)) return false;
    return !lstrcmpW(cls, L"Progman") || !lstrcmpW(cls, L"WorkerW") ||
           !lstrcmpW(cls, L"Shell_TrayWnd") || !lstrcmpW(cls, L"Shell_SecondaryTrayWnd") ||
           !lstrcmpW(cls, kBorderClass) || !lstrcmpW(cls, kPickClass) || !lstrcmpW(cls, kHostClass);
}

// ---------- 边框（区域裁剪方案：窗口=目标矩形，内部挖空只剩边框环，不可能遮挡窗口内容） ----------
static HRGN CreateRingRegion(int w, int h, int t)
{
    HRGN outer = CreateRectRgn(0, 0, w, h);
    HRGN inner = CreateRectRgn(t, t, w - t, h - t);
    HRGN ring = CreateRectRgn(0, 0, 0, 0);
    if (!outer || !inner || !ring || CombineRgn(ring, outer, inner, RGN_DIFF) == ERROR)
    {
        if (ring) { DeleteObject(ring); ring = nullptr; }
    }
    if (outer) DeleteObject(outer);
    if (inner) DeleteObject(inner);
    return ring;
}

static HWND CreateBorder(HWND target)
{
    RECT rc;
    if (!GetTrueRect(target, &rc)) return nullptr;
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return nullptr;

    HWND border = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kBorderClass, L"", WS_POPUP,
        rc.left, rc.top, w, h, nullptr, nullptr, g.hInst, nullptr);
    if (!border) return nullptr;

    SetLayeredWindowAttributes(border, 0, 255, LWA_ALPHA); // 不透明显示，鼠标穿透靠 WS_EX_TRANSPARENT
    if (HRGN ring = CreateRingRegion(w, h, BorderThickness()))
        SetWindowRgn(border, ring, FALSE);                 // 区域所有权移交系统
    INT32 cornerPref = DWMWCP_ROUND;
    DwmSetWindowAttribute(border, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref)); // Win11 圆角跟随
    ShowWindow(border, SW_SHOWNA);
    SetWindowPos(border, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    return border;
}

static void UpdateBorderPos(HWND target, HWND border)
{
    if (!IsWindow(target) || !IsWindowVisible(target))
    {
        ShowWindow(border, SW_HIDE);
        return;
    }
    RECT rc;
    if (!GetTrueRect(target, &rc)) return;
    int w = rc.right - rc.left, h = rc.bottom - rc.top;

    RECT cur;
    GetWindowRect(border, &cur);
    bool sizeChanged = (cur.right - cur.left) != w || (cur.bottom - cur.top) != h;

    SetWindowPos(border, HWND_TOPMOST, rc.left, rc.top, w, h, SWP_NOACTIVATE);

    if (sizeChanged) // 缩放后重挖内部区域，防止边框残留在旧尺寸
    {
        if (HRGN ring = CreateRingRegion(w, h, BorderThickness()))
            SetWindowRgn(border, ring, TRUE);
    }
    ShowWindow(border, SW_SHOWNA);
}

// ---------- 透明度 ----------
static void ApplyAlpha(HWND hwnd, int percentage)
{
    if (!hwnd || !IsWindow(hwnd)) return;
    percentage = percentage < TRANS_STEP ? TRANS_STEP : (percentage > 100 ? 100 : percentage);

    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    bool layered = (ex & WS_EX_LAYERED) != 0;

    // 首次修改前缓存原始状态，取消置顶时恢复
    if (g.originalLayered.find(hwnd) == g.originalLayered.end())
    {
        RECT state{ percentage, 0, layered ? 1 : 0, 0 }; // left 存"应用过的透明度"，right 存原本是否 layered
        BYTE alpha = 255; COLORREF key = 0; DWORD flags = 0;
        if (layered && GetLayeredWindowAttributes(hwnd, &key, &alpha, &flags))
        {
            state.top = alpha;               // 原始 alpha
            state.bottom = flags;            // 原始 flags
        }
        g.originalLayered[hwnd] = state;
    }

    BYTE alphaValue = (BYTE)(255 * percentage / 100);
    SetWindowLongW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    SetLayeredWindowAttributes(hwnd, 0, alphaValue, LWA_ALPHA);
}

static void RestoreAlpha(HWND hwnd)
{
    auto it = g.originalLayered.find(hwnd);
    if (it == g.originalLayered.end()) return;
    if (!IsWindow(hwnd)) { g.originalLayered.erase(it); return; }

    const RECT& st = it->second;
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (st.right) // 原本就是分层窗口
    {
        SetLayeredWindowAttributes(hwnd, (COLORREF)st.bottom, (BYTE)st.top, (DWORD)st.bottom | LWA_ALPHA);
    }
    else
    {
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
        SetWindowLongW(hwnd, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    g.originalLayered.erase(it);
}

// ---------- 置顶/取消（单置顶模式：钉新自动取消旧） ----------
static void UnpinWindow(HWND hwnd, bool windowDestroyed)
{
    auto it = g.pinned.find(hwnd);
    if (it == g.pinned.end()) return;

    if (!windowDestroyed && IsWindow(hwnd))
    {
        RemovePropW(hwnd, kPinProp);
        RestoreAlpha(hwnd);
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (it->second && IsWindow(it->second)) DestroyWindow(it->second);
    g.pinned.erase(it);
}

static void UnpinAll()
{
    std::unordered_map<HWND, HWND> copy = g.pinned;
    for (auto& kv : copy) UnpinWindow(kv.first, false);
}

static void TogglePinWindow(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd) || IsShellWindow(hwnd)) return;

    if (IsPinned(hwnd))
    {
        UnpinWindow(hwnd, false); // 已置顶的窗口：再选一次 = 取消
        return;
    }

    UnpinAll(); // 单置顶模式：先取消上一个，保证全局只有一个置顶窗口
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetPropW(hwnd, kPinProp, (HANDLE)1);
    g.pinned[hwnd] = CreateBorder(hwnd);
}

static void ToggleForeground()
{
    HWND hwnd = GetForegroundWindow();
    if (!hwnd || hwnd == g.host) return;
    TogglePinWindow(hwnd);
}

static void StepForegroundTransparency(int delta)
{
    HWND hwnd = GetForegroundWindow();
    if (!hwnd || !IsPinned(hwnd)) return; // 只对已置顶窗口生效（同 PowerToys）

    int cur = 100;
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_LAYERED)
    {
        BYTE alpha = 255;
        if (GetLayeredWindowAttributes(hwnd, nullptr, &alpha, nullptr))
            cur = alpha * 100 / 255;
    }
    int next = cur + delta;
    if (next < TRANS_STEP) next = TRANS_STEP;
    if (next > 100) next = 100;
    if (next != cur) ApplyAlpha(hwnd, next);
}

// ---------- WinEvent 回调 ----------

static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                  LONG idObject, LONG idChild, DWORD, DWORD)
{
    if (!hwnd || idObject != OBJID_WINDOW || idChild != 0) return;
    if (IsShellWindow(hwnd)) return; // 跳过自己的边框/遮罩/宿主及系统外壳窗口

    switch (event)
    {
    case EVENT_OBJECT_LOCATIONCHANGE:
    {
        auto it = g.pinned.find(hwnd);
        if (it != g.pinned.end() && it->second) UpdateBorderPos(it->first, it->second);
        break;
    }
    case EVENT_OBJECT_DESTROY:
        UnpinWindow(hwnd, true);
        break;
    case EVENT_SYSTEM_MINIMIZESTART:
    {
        auto it = g.pinned.find(hwnd);
        if (it != g.pinned.end() && it->second) ShowWindow(it->second, SW_HIDE);
        break;
    }
    case EVENT_SYSTEM_MINIMIZEEND:
    {
        auto it = g.pinned.find(hwnd);
        if (it != g.pinned.end() && it->second)
        {
            // 部分应用恢复后 TOPMOST 标志会丢失，重新钉上（同 PowerToys #17332 修复）
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            UpdateBorderPos(hwnd, it->second);
        }
        break;
    }
    case EVENT_OBJECT_FOCUS:
    {
        // 应用自己重置了 TOPMOST 时补回来（同 PowerToys #19168 修复）
        for (auto& kv : g.pinned)
        {
            if (IsWindow(kv.first) &&
                !(GetWindowLongW(kv.first, GWL_EXSTYLE) & WS_EX_TOPMOST))
            {
                SetWindowPos(kv.first, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }
        break;
    }
    default:
        break;
    }
}

// ---------- 点击选窗模式 ----------
static HWND g_pickOverlay = nullptr;

static LRESULT CALLBACK PickWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_LBUTTONDOWN:
    {
        POINT pt;
        GetCursorPos(&pt);
        ShowWindow(hwnd, SW_HIDE); // 先隐藏遮罩，否则点到自己
        HWND target = WindowFromPoint(pt);
        if (target) target = GetAncestor(target, GA_ROOT);
        if (target && GetWindowTextLengthW(target) > 0)
            TogglePinWindow(target);
        return 0;
    }
    case WM_RBUTTONDOWN: // 右键取消
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_CROSS));
        return TRUE;
    case WM_DESTROY:
        g_pickOverlay = nullptr;
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

static void EnterPickMode()
{
    if (!g_pickOverlay)
    {
        WNDCLASSEXW pc = {};
        pc.cbSize = sizeof(pc);
        pc.lpfnWndProc = PickWndProc;
        pc.hInstance = g.hInst;
        pc.lpszClassName = kPickClass;
        pc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        RegisterClassExW(&pc);

        g_pickOverlay = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            kPickClass, L"", WS_POPUP,
            GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
            GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
            nullptr, nullptr, g.hInst, nullptr);
        if (!g_pickOverlay) return;
        SetLayeredWindowAttributes(g_pickOverlay, 0, 40, LWA_ALPHA); // 屏幕轻微变暗提示选窗中
    }

    SetWindowPos(g_pickOverlay, HWND_TOPMOST,
                 GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                 GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    SetForegroundWindow(g_pickOverlay); // 让 Esc 生效
    SetFocus(g_pickOverlay);
}

// ---------- 托盘 ----------
static void ShowTrayMenu()
{
    HMENU menu = CreatePopupMenu();
    wchar_t pickText[64];
    _snwprintf(pickText, 64, L"选择窗口置顶 (点击目标窗口)%s",
               g_hotkeyFallback ? L"  [热键已回退为 Alt+Win+T]" : L"");
    AppendMenuW(menu, MF_STRING, IDM_PICK, pickText);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g.host); // 让菜单能正常收起
    int cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                             pt.x, pt.y, 0, g.host, nullptr);
    DestroyMenu(menu);

    if (cmd == IDM_EXIT) DestroyWindow(g.host);
    else if (cmd == IDM_PICK) EnterPickMode();
}

static void InitTray()
{
    g.nid.cbSize = sizeof(g.nid);
    g.nid.hWnd = g.host;
    g.nid.uID = 1;
    g.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g.nid.uCallbackMessage = WM_APP_TRAY;
    g.nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpyW(g.nid.szTip, L"AlwaysOnTop Mini - Shift+Win+T 置顶窗口");
    Shell_NotifyIconW(NIM_ADD, &g.nid);
}

// ---------- 初始化 ----------
static void EnableDpiAwareness()
{
    typedef BOOL(WINAPI * FnCtx)(DPI_AWARENESS_CONTEXT);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32)
    {
        auto fn = (FnCtx)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (fn && fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        {
            auto getDpi = (UINT(WINAPI*)())GetProcAddress(user32, "GetDpiForSystem");
            if (getDpi) { g.sysDpi = getDpi(); return; }
        }
    }
    SetProcessDPIAware();
}

static bool InitHooks()
{
    const DWORD events[] = {
        EVENT_OBJECT_LOCATIONCHANGE,
        EVENT_OBJECT_DESTROY,
        EVENT_OBJECT_FOCUS,
        EVENT_SYSTEM_MINIMIZESTART,
        EVENT_SYSTEM_MINIMIZEEND,
    };
    int i = 0;
    for (DWORD e : events)
    {
        g.hooks[i] = SetWinEventHook(e, e, nullptr, WinEventProc, 0, 0,
                                     WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (!g.hooks[i]) return false;
        i++;
    }
    return true;
}

static LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_HOTKEY:
        if (wp == ID_HOTKEY_PIN) ToggleForeground();
        else if (wp == ID_HOTKEY_OP_UP) StepForegroundTransparency(+TRANS_STEP);
        else if (wp == ID_HOTKEY_OP_DOWN) StepForegroundTransparency(-TRANS_STEP);
        return 0;
    case WM_APP_TRAY:
        if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK)
            ShowTrayMenu();
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp) PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g.nid);
        UnpinAll();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int)
{
    g.hInst = hInst;

    // 单实例
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    EnableDpiAwareness();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kHostClass;
    RegisterClassExW(&wc);

    // message-only 宿主窗口（不可见，仅接收热键/托盘消息）
    g.host = CreateWindowExW(0, kHostClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!g.host) return 0;

    WNDCLASSEXW bc = wc;
    bc.lpszClassName = kBorderClass;
    bc.lpfnWndProc = DefWindowProcW;                       // 边框窗口无需消息处理
    bc.hbrBackground = CreateSolidBrush(BORDER_COLOR);     // 背景即边框颜色，配合区域裁剪
    RegisterClassExW(&bc);

    InitTray();

    // 热键注册（带冲突回退）：首选 Shift+Win+T；被其他程序占用时自动回退 Alt+Win+T
    UINT pinMods = MOD_SHIFT | MOD_WIN | MOD_NOREPEAT;
    UINT pinKey = 'T';
    if (!RegisterHotKey(g.host, ID_HOTKEY_PIN, pinMods, pinKey))
    {
        pinMods = MOD_ALT | MOD_WIN | MOD_NOREPEAT;
        if (RegisterHotKey(g.host, ID_HOTKEY_PIN, pinMods, pinKey))
        {
            g_hotkeyFallback = true;
        }
        else
        {
            MessageBoxW(nullptr,
                        L"置顶热键注册失败：Shift+Win+T 和 Alt+Win+T 均被其他程序占用。\n"
                        L"仍可使用托盘菜单\"选择窗口置顶\"。",
                        L"AlwaysOnTop Mini", MB_OK | MB_ICONWARNING);
        }
    }
    RegisterHotKey(g.host, ID_HOTKEY_OP_UP, MOD_CONTROL | MOD_WIN | MOD_NOREPEAT, VK_UP);
    RegisterHotKey(g.host, ID_HOTKEY_OP_DOWN, MOD_CONTROL | MOD_WIN | MOD_NOREPEAT, VK_DOWN);

    if (!InitHooks()) return 0;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    for (auto& h : g.hooks)
        if (h) UnhookWinEvent(h);
    if (mutex) ReleaseMutex(mutex);
    return 0;
}
