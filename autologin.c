/*
 * 开机自动登录设置工具
 * 功能:
 *   1. 设置 Windows 开机自动登录(Winlogon 注册表), 开机直接进入桌面
 *   2. 自动开启开机数字小键盘 NumLock(登录界面 + 当前用户)
 *   3. 取消开机自动登录, 恢复为手动输入密码
 *
 * 编译: 见 build.bat (gcc -mwindows, 清单通过 --manifest-file 嵌入)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <strsafe.h>
#include <wchar.h>

/* ---------- 控件 ID ---------- */
#define IDC_USER   1001
#define IDC_PASS   1002
#define IDC_NUM    1003
#define IDC_APPLY  1004
#define IDC_CANCEL 1005

/* ---------- 全局句柄/字体 ---------- */
static HINSTANCE g_hInst;
static HFONT   hFont, hFontTitle;
static HWND    hUser, hPass, hNum, hBtn, hBtnCancel;

/* ===================================================================
 * 管理员权限: 检测当前进程是否已提权, 否则以 runas 重新拉起自己
 * =================================================================== */
static BOOL IsProcessElevated(void)
{
    HANDLE hToken = NULL;
    BOOL   fElevated = FALSE;

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION Elevation;
        DWORD cbSize = sizeof(Elevation);
        if (GetTokenInformation(hToken, TokenElevation,
                                &Elevation, sizeof(Elevation), &cbSize)) {
            fElevated = Elevation.TokenIsElevated;
        }
        CloseHandle(hToken);
    }
    return fElevated;
}

static void RelaunchAsAdmin(void)
{
    WCHAR szPath[MAX_PATH];
    SHELLEXECUTEINFOW sei;

    GetModuleFileNameW(NULL, szPath, MAX_PATH);
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize       = sizeof(sei);
    sei.lpVerb       = L"runas";
    sei.lpFile       = szPath;
    sei.nShow        = SW_SHOWNORMAL;
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS;
    ShellExecuteExW(&sei);
}

/* ===================================================================
 * 注册表辅助函数
 * 注意: 编译目标为 32 位, 访问 HKLM\Software 必须带 KEY_WOW64_64KEY,
 *       否则会被重定向到 Wow6432Node, Winlogon 读取不到。
 * =================================================================== */
static LONG RegWriteSz(HKEY hRoot, const WCHAR *wszSubKey,
                       const WCHAR *wszName, const WCHAR *wszValue)
{
    HKEY  hKey = NULL;
    LONG  lRet;
    DWORD cbData;

    lRet = RegCreateKeyExW(hRoot, wszSubKey, 0, NULL, 0,
                           KEY_SET_VALUE | KEY_WOW64_64KEY,
                           NULL, &hKey, NULL);
    if (lRet != ERROR_SUCCESS) return lRet;

    cbData = (DWORD)((wcslen(wszValue) + 1) * sizeof(WCHAR));
    lRet = RegSetValueExW(hKey, wszName, 0, REG_SZ,
                          (const BYTE *)wszValue, cbData);
    RegCloseKey(hKey);
    return lRet;
}

static LONG RegWriteDword(HKEY hRoot, const WCHAR *wszSubKey,
                          const WCHAR *wszName, DWORD dwValue)
{
    HKEY  hKey = NULL;
    LONG  lRet;

    lRet = RegCreateKeyExW(hRoot, wszSubKey, 0, NULL, 0,
                           KEY_SET_VALUE | KEY_WOW64_64KEY,
                           NULL, &hKey, NULL);
    if (lRet != ERROR_SUCCESS) return lRet;

    lRet = RegSetValueExW(hKey, wszName, 0, REG_DWORD,
                          (const BYTE *)&dwValue, sizeof(DWORD));
    RegCloseKey(hKey);
    return lRet;
}

/*
 * 配置开机自动登录 + 小键盘
 * 成功返回 TRUE; 失败返回 FALSE 并填充 wszErr。
 */
static BOOL ConfigureAutoLogin(const WCHAR *wszUser,
                               const WCHAR *wszPass,
                               BOOL bEnableNumLock,
                               WCHAR *wszErr, DWORD cchErr)
{
    static const WCHAR *WINLOGON =
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";
    static const WCHAR *PWDLESS =
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\PasswordLess\\Device";

    LONG  lRet;
    WCHAR szDomain[256];
    DWORD cchDomain = 256;

    /* ---- 1. Winlogon 自动登录四项 ---- */
    lRet = RegWriteSz(HKEY_LOCAL_MACHINE, WINLOGON,
                      L"AutoAdminLogon", L"1");
    if (lRet != ERROR_SUCCESS) {
        StringCchPrintfW(wszErr, cchErr,
            L"写入 AutoAdminLogon 失败 (错误码 %ld)", (long)lRet);
        return FALSE;
    }

    lRet = RegWriteSz(HKEY_LOCAL_MACHINE, WINLOGON,
                      L"DefaultUserName", wszUser);
    if (lRet != ERROR_SUCCESS) {
        StringCchPrintfW(wszErr, cchErr,
            L"写入 DefaultUserName 失败 (错误码 %ld)", (long)lRet);
        return FALSE;
    }

    lRet = RegWriteSz(HKEY_LOCAL_MACHINE, WINLOGON,
                      L"DefaultPassword", wszPass);
    if (lRet != ERROR_SUCCESS) {
        StringCchPrintfW(wszErr, cchErr,
            L"写入 DefaultPassword 失败 (错误码 %ld)", (long)lRet);
        return FALSE;
    }

    /* 本地账户写计算机名作为域; 微软账户(邮箱)不写, 避免域不匹配 */
    if (wcschr(wszUser, L'@') == NULL &&
        GetComputerNameW(szDomain, &cchDomain)) {
        RegWriteSz(HKEY_LOCAL_MACHINE, WINLOGON,
                   L"DefaultDomainName", szDomain);
    }

    /* ---- 2. 关闭 Win10/11 "仅允许 Windows Hello 登录",
              否则新版系统会强制要求输入密码 ---- */
    RegWriteDword(HKEY_LOCAL_MACHINE, PWDLESS,
                  L"DevicePasswordLessBuildVersion", 0);

    /* ---- 3. 开机自动启用数字小键盘 NumLock ---- */
    if (bEnableNumLock) {
        /* 登录前(登录界面)的小键盘状态, 2147483650 = 0x80000002 强制点亮 */
        RegWriteSz(HKEY_USERS,
                   L".DEFAULT\\Control Panel\\Keyboard",
                   L"InitialKeyboardIndicators", L"2147483650");
        /* 登录后当前用户的小键盘状态, 2 = 开启 */
        RegWriteSz(HKEY_CURRENT_USER,
                   L"Control Panel\\Keyboard",
                   L"InitialKeyboardIndicators", L"2");
    }

    return TRUE;
}

/*
 * 删除注册表值(值不存在时静默忽略)
 */
static void RegRemoveValue(HKEY hRoot, const WCHAR *wszSubKey,
                           const WCHAR *wszName)
{
    HKEY hKey = NULL;

    if (RegOpenKeyExW(hRoot, wszSubKey, 0,
                      KEY_SET_VALUE | KEY_WOW64_64KEY,
                      &hKey) == ERROR_SUCCESS) {
        RegDeleteValueW(hKey, wszName);
        RegCloseKey(hKey);
    }
}

/*
 * 取消开机自动登录
 *   AutoAdminLogon 置 0, 并删除明文保存的 DefaultPassword,
 *   使下次开机恢复为手动输入密码。
 *   bDisableNumLock 为真时同时撤销小键盘设置。
 * 成功返回 TRUE。
 */
static BOOL DisableAutoLogin(BOOL bDisableNumLock)
{
    static const WCHAR *WINLOGON =
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";
    static const WCHAR *DEFKBD =
        L".DEFAULT\\Control Panel\\Keyboard";
    static const WCHAR *CURKBD =
        L"Control Panel\\Keyboard";

    HKEY  hKey = NULL;
    LONG  lRet;
    const WCHAR szZero[] = L"0";

    lRet = RegOpenKeyExW(HKEY_LOCAL_MACHINE, WINLOGON, 0,
                         KEY_SET_VALUE | KEY_WOW64_64KEY, &hKey);
    if (lRet != ERROR_SUCCESS) return FALSE;

    RegSetValueExW(hKey, L"AutoAdminLogon", 0, REG_SZ,
                   (const BYTE *)szZero, sizeof(szZero));
    RegDeleteValueW(hKey, L"DefaultPassword");
    RegCloseKey(hKey);

    if (bDisableNumLock) {
        /* 删除即恢复系统默认的小键盘状态 */
        RegRemoveValue(HKEY_USERS, DEFKBD, L"InitialKeyboardIndicators");
        RegRemoveValue(HKEY_CURRENT_USER, CURKBD,
                       L"InitialKeyboardIndicators");
    }
    return TRUE;
}

/* ===================================================================
 * UI 辅助
 * =================================================================== */
static HWND CreateLabel(HWND hParent, const WCHAR *text,
                        int x, int y, int w, int h, HFONT font)
{
    HWND hw = CreateWindowExW(0, L"STATIC", text,
                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                    x, y, w, h, hParent, NULL, g_hInst, NULL);
    SendMessageW(hw, WM_SETFONT, (WPARAM)font, TRUE);
    return hw;
}

static void OnApply(HWND hWnd)
{
    WCHAR wszUser[256]   = {0};
    WCHAR wszPass[256]   = {0};
    WCHAR wszErr[512]    = {0};
    WCHAR wszMsg[640];
    BOOL  bNumLock;
    HCURSOR hOld;

    GetWindowTextW(hUser, wszUser, 256);
    GetWindowTextW(hPass, wszPass, 256);

    if (wcslen(wszUser) == 0) {
        MessageBoxW(hWnd, L"请输入用户名。", L"提示",
                    MB_OK | MB_ICONWARNING);
        SetFocus(hUser);
        return;
    }

    bNumLock = (BOOL)SendMessageW(hNum, BM_GETCHECK, 0, 0);

    hOld = SetCursor(LoadCursor(NULL, IDC_WAIT));

    if (!ConfigureAutoLogin(wszUser, wszPass, bNumLock, wszErr, 512)) {
        SetCursor(hOld);
        StringCchPrintfW(wszMsg, 640,
            L"设置失败:\n%s\n\n请确认以管理员身份运行本程序。", wszErr);
        MessageBoxW(hWnd, wszMsg, L"错误", MB_OK | MB_ICONERROR);
        return;
    }

    SetCursor(hOld);

    if (bNumLock) {
        StringCchPrintfW(wszMsg, 640,
            L"已设置开机自动登录:\n\n用户: %s\n\n"
            L"下次开机将自动输入密码并直接进入桌面,\n"
            L"同时会自动开启数字小键盘(NumLock)。",
            wszUser);
    } else {
        StringCchPrintfW(wszMsg, 640,
            L"已设置开机自动登录:\n\n用户: %s\n\n"
            L"下次开机将自动输入密码并直接进入桌面。",
            wszUser);
    }
    MessageBoxW(hWnd, wszMsg, L"设置成功", MB_OK | MB_ICONINFORMATION);
}

static void OnCancel(HWND hWnd)
{
    WCHAR   wszMsg[512];
    BOOL    bNumLock;
    HCURSOR hOld;

    bNumLock = (BOOL)SendMessageW(hNum, BM_GETCHECK, 0, 0);

    if (bNumLock) {
        StringCchCopyW(wszMsg, 512,
            L"确定要取消开机自动登录吗?\n\n"
            L"取消后开机将恢复为手动输入密码登录,\n"
            L"同时会撤销开机自动启用数字小键盘的设置。");
    } else {
        StringCchCopyW(wszMsg, 512,
            L"确定要取消开机自动登录吗?\n\n"
            L"取消后开机将恢复为手动输入密码登录。");
    }

    if (MessageBoxW(hWnd, wszMsg, L"确认取消",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }

    hOld = SetCursor(LoadCursor(NULL, IDC_WAIT));

    if (!DisableAutoLogin(bNumLock)) {
        SetCursor(hOld);
        MessageBoxW(hWnd,
            L"取消失败, 无法写入系统注册表。\n\n"
            L"请确认以管理员身份运行本程序。",
            L"错误", MB_OK | MB_ICONERROR);
        return;
    }

    SetCursor(hOld);
    MessageBoxW(hWnd,
        L"已取消开机自动登录。\n\n下次开机将需要手动输入密码。",
        L"取消成功", MB_OK | MB_ICONINFORMATION);
}

/* ===================================================================
 * 窗口过程
 * =================================================================== */
static LRESULT CALLBACK WndProc(HWND hWnd, UINT message,
                                WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE: {
        WCHAR szCurUser[256];
        DWORD cchCurUser = 256;

        hUser = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                    105, 55, 290, 25, hWnd, (HMENU)(INT_PTR)IDC_USER,
                    g_hInst, NULL);

        hPass = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP
                    | ES_AUTOHSCROLL | ES_PASSWORD,
                    105, 95, 290, 25, hWnd, (HMENU)(INT_PTR)IDC_PASS,
                    g_hInst, NULL);

        hNum = CreateWindowExW(0, L"BUTTON",
                    L"同时开机自动启用数字小键盘 (NumLock)",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                    105, 135, 290, 22, hWnd, (HMENU)(INT_PTR)IDC_NUM,
                    g_hInst, NULL);
        SendMessageW(hNum, BM_SETCHECK, BST_CHECKED, 0);

        hBtn = CreateWindowExW(0, L"BUTTON", L"设置开机自动登录",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                    105, 172, 140, 34, hWnd, (HMENU)(INT_PTR)IDC_APPLY,
                    g_hInst, NULL);

        hBtnCancel = CreateWindowExW(0, L"BUTTON", L"取消开机自动登录",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                    255, 172, 140, 34, hWnd, (HMENU)(INT_PTR)IDC_CANCEL,
                    g_hInst, NULL);

        SendMessageW(hUser, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hPass, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hNum,  WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hBtn,  WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hBtnCancel, WM_SETFONT, (WPARAM)hFont, TRUE);

        CreateLabel(hWnd, L"开机自动登录设置", 20, 14, 380, 26, hFontTitle);
        CreateLabel(hWnd, L"用户名(&U):", 25, 57, 75, 22, hFont);
        CreateLabel(hWnd, L"密  码(&P):", 25, 97, 75, 22, hFont);
        CreateLabel(hWnd,
            L"说明: 设置后开机将自动登录并直接进入桌面, 无需手动输入密码。",
            20, 222, 390, 18, hFont);
        CreateLabel(hWnd,
            L"密码由 Windows 自动登录机制保存在注册表中, 请在可信电脑上使用。",
            20, 242, 390, 18, hFont);
        CreateLabel(hWnd,
            L"点\"取消开机自动登录\"可恢复为每次开机手动输入密码。",
            20, 264, 390, 18, hFont);

        /* 默认填入当前登录用户名, 方便修改 */
        if (GetUserNameW(szCurUser, &cchCurUser)) {
            SetWindowTextW(hUser, szCurUser);
        }
        SetFocus(hPass);
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_APPLY && HIWORD(wParam) == BN_CLICKED) {
            OnApply(hWnd);
            return 0;
        }
        if (LOWORD(wParam) == IDC_CANCEL && HIWORD(wParam) == BN_CLICKED) {
            OnCancel(hWnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

/* ===================================================================
 * 程序入口
 * ================================================================= */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    WNDCLASSW wc;
    HWND      hWnd;
    MSG       msg;
    HDC       hdc;
    int       dpiY;
    int       iFontH, iTitleH;
    RECT      rc;

    (void)hPrevInstance;
    (void)lpCmdLine;

    /* 写 HKLM 必须管理员: 未提权则弹 UAC 重新启动 */
#ifndef SKIP_ELEVATION
    if (!IsProcessElevated()) {
        RelaunchAsAdmin();
        return 0;
    }
#endif

    g_hInst = hInstance;

    /* 创建字体(微软雅黑, 按系统 DPI 换算字号) */
    hdc = GetDC(NULL);
    dpiY = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(NULL, hdc);
    iFontH  = -MulDiv(9,  dpiY, 72);
    iTitleH = -MulDiv(13, dpiY, 72);

    hFont = CreateFontW(iFontH, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
    hFontTitle = CreateFontW(iTitleH, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"AutoLoginConfigWnd";
    wc.hIcon         = LoadIcon(NULL, IDI_SHIELD);
    if (!RegisterClassW(&wc)) return 0;

    hWnd = CreateWindowExW(0, wc.lpszClassName,
                    L"开机自动登录设置工具",
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                    CW_USEDEFAULT, CW_USEDEFAULT, 436, 336,
                    NULL, NULL, hInstance, NULL);
    if (!hWnd) return 0;

    /* 屏幕居中 */
    GetWindowRect(hWnd, &rc);
    SetWindowPos(hWnd, NULL,
        (GetSystemMetrics(SM_CXSCREEN) - (rc.right - rc.left)) / 2,
        (GetSystemMetrics(SM_CYSCREEN) - (rc.bottom - rc.top)) / 2,
        0, 0, SWP_NOSIZE | SWP_NOZORDER);

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    /* IsDialogMessage 让 Tab/回车/Alt 快捷键在普通窗口上可用 */
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (!IsDialogMessageW(hWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    DeleteObject(hFont);
    DeleteObject(hFontTitle);
    return (int)msg.wParam;
}
