// OrbitMM2Macro.cpp – Full, final, working version (no warnings)
// Compile: g++ -O2 -std=c++11 -mwindows OrbitMM2Macro.cpp -o OrbitMM2Macro.exe -lwinmm -lgdi32 -luser32 -lkernel32 -lcomctl32 -lshell32 -static

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600
#define _WIN32_IE 0x0600
#include <winsock2.h>
#include <windows.h>
#include <mmsystem.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <urlmon.h>
#include <winhttp.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>
#include <vector>
#include <string>
#include <algorithm>
#include <set>

// Custom messages
#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_EXIT        1001
#define ID_TRAY_SETTINGS    1002

// WinDivert dynamic linkage (lag switch). We resolve these pointers at runtime
// from WinDivert.dll so the .exe stays self-contained; if the DLL/driver are
// missing, the lag-switch tab just reports "unavailable" rather than crashing.
typedef PVOID WINDIVERT_HANDLE;
#define WINDIVERT_FLAG_SNIFF    0x0001
#define WINDIVERT_FLAG_DROP     0x0002
#define WINDIVERT_FLAG_RECV_ONLY 0x0004
// WinDivert enum constants we use (raw values from windivert.h v2.2).
enum WINDIVERT_LAYER_LOCAL   { WD_LAYER_NETWORK = 0 };
enum WINDIVERT_IOCTL_LOCAL   { WD_IOCTL_QUEUE = 1, WD_IOCTL_SET_FLOWLIMIT = 2,
                                WD_IOCTL_GET_STATS = 3, WD_IOCTL_RESET_STATS = 4 };

// windivert address struct - must match windivert.h v2.2 ABI exactly (80 bytes).
typedef struct {
    INT64  Timestamp;
    UINT32 Layer:8;
    UINT32 Event:8;
    UINT32 Sniffed:1;
    UINT32 Outbound:1;
    UINT32 Loopback:1;
    UINT32 Impostor:1;
    UINT32 IPv6:1;
    UINT32 IPChecksum:1;
    UINT32 TCPChecksum:1;
    UINT32 UDPChecksum:1;
    UINT32 Reserved1:8;
    UINT32 Reserved2;
    union {
        struct { UINT32 IfIdx; UINT32 SubIfIdx; } Network;
        UINT8 Reserved3[64];
    };
} WINDIVERT_ADDRESS, *PWINDIVERT_ADDRESS;

// Function-pointer types matching the real exports the .dll ships with.
typedef WINDIVERT_HANDLE (*WinDivertOpenFunc)(LPCSTR, INT32, INT32, UINT64);
typedef BOOL            (*WinDivertRecvFunc)(WINDIVERT_HANDLE, PVOID, UINT, PUINT, PWINDIVERT_ADDRESS);
typedef BOOL            (*WinDivertSendFunc)(WINDIVERT_HANDLE, PVOID, UINT, PUINT, PWINDIVERT_ADDRESS);
typedef BOOL            (*WinDivertCloseFunc)(WINDIVERT_HANDLE);
typedef UINT64          (*WinDivertHelperHashFunc)(PVOID, UINT64, UINT64, UINT64);

// ==============================
//  Macro structures
// ==============================
struct MacroItem { int menu; int slot; };
struct Macro {
    std::string name;
    int menus = 3;
    UINT hotkey = 0;
    std::vector<MacroItem> items;
};

// ==============================
//  Global config path
// ==============================
static char g_configPath[MAX_PATH] = {0};

// ==============================
//  Global state
// ==============================
static HWND g_hwndMain = nullptr;
static HWND g_hwndSettings = nullptr;
static HWND g_hwndMacroEditor = nullptr;
static NOTIFYICONDATAW g_nid = {};
static HHOOK g_keyboardHook = NULL;
static HHOOK g_mouseHook = NULL;
static HANDLE g_singleInstanceMutex = NULL;
static bool g_exitRequested = false;
static bool g_recording = false;
static int g_recordingTarget = 0; // 0=glitch, 1=sit, 2=superjump, 3=lag

static Macro g_editingMacro;
static bool g_editingNew = true;
static std::map<std::string, Macro> g_macros;
static std::atomic<bool> g_glitchActive{false};
static std::atomic<bool> g_spamActive{false};

// Global stop/restart key state. The "was" flags are only touched from the
// hook thread, so plain bools are fine here.
static std::atomic<bool> g_keybindsLocked{false};

// Lag-switch dynamic module state. Kept together so EnableLagSwitch/DisableLagSwitch
// can operate against the resolved WinDivert pointers without touching globals elsewhere.
static HMODULE g_winDivertDll = NULL;
static WinDivertOpenFunc  g_fnWinDivertOpen  = NULL;
static WinDivertRecvFunc  g_fnWinDivertRecv  = NULL;
static WinDivertSendFunc  g_fnWinDivertSend  = NULL;
static WinDivertCloseFunc g_fnWinDivertClose = NULL;
static std::atomic<bool>  g_lagSwitchOn{false};
static std::atomic<bool>  g_lagAvailable{false};
static HWND g_statusLabelLag = nullptr;

// Status labels
static HWND g_statusLabelSit = nullptr;
static HWND g_statusLabelJump = nullptr;
static HWND g_statusLabelGlitch = nullptr;
static HWND g_statusLabelSpam = nullptr;
static HWND g_statusLabelUpdate = nullptr;

struct Config {
    double sensitivity = 0.294;
    int fps = 240;
    bool camFix = false;
    bool holdMode = false;
    bool glitchEnabled = false;
    UINT glitchKey = 0;       // no default key until the user records one

    bool sitEnabled = false;
    UINT sitKey = 0;          // no default key until the user records one
    int sitSlot = 1;

    bool superJumpEnabled = false;
    UINT superJumpKey = 0;    // no default key until the user records one
    bool spaceDuringJump = true;

    bool equipEnabled = false;
    bool startMinimized = false;

    // Lag switch. Per request: most settings are hardcoded to match the Spencer
    // screenshots; only the trigger key itself is personalisable.
    bool lagEnabled = false;
    UINT lagKey = 0;          // personalisable - no default key until the user records one

    // Spam Sign (Spencer "Item Clip"). Slot and trigger key are personalisable;
    // the 34ms cycle and the Roblox-focus requirement are hardcoded (defaults).
    bool spamEnabled = false;
    UINT spamKey = 0;         // personalisable - no default key until the user records one
    int spamSlot = 7;         // gear slot repeatedly equipped/released
    bool spamHoldMode = false; // checks "hold the trigger key while spam" like Spencer's toggle/hold

    // Global stop-all key: first press stops every running macro, second press
    // restarts exactly the ones that were running.
    UINT stopAllKey = 0;      // no default key until the user records one
} g_config;

// ==============================
//  Update detector (GitHub releases)
// ==============================
static const char* g_currentVersion = "1.6.0";
static std::string g_lastNotifiedVersion;

// Forward declarations
void LoadConfig();
void SaveConfig();
bool IsRobloxFocused();
int CalculatePixelValue();
void SendMouseMove(int dx, int dy);
void SendKey(WORD vk, bool down);
void HoldW(bool hold);
void FrameDelays(int fps, int& d1, int& d2);
void SleepMicro(int us);
void SpeedGlitchWorker();
void SpamSignWorker();
void DoSuperJump();
void OnHotkey(UINT vk, bool down);
void OnSpamHotkey(UINT vk, bool down);
void OnSitHotkey(UINT vk);
void OnSuperJumpHotkey(UINT vk);
void OnLagHotkey(UINT vk, bool down);
void RunMacro(const Macro& macro);
void LoadMacros();
void SaveMacros();
std::string KeyName(UINT vk);
void OnStopAllHotkey();
static void LagSwitchWorker();
static void EnsureLagAvailable();
static void UpdateCheckWorker();

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK MacroEditorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK HintWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ==============================
//  Theme: dark palette matched to the app logo (black + red accent)
// ==============================
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#define COL_BG        RGB(20,20,23)     // window background
#define COL_HEADER    RGB(27,10,13)     // header band background
#define COL_EDIT_BG   RGB(34,34,39)     // edit / listview background
#define COL_ACCENT    RGB(220,40,50)    // logo red
#define COL_ACCENT_DK RGB(172,28,36)    // pressed red
#define COL_ACCENT_LT RGB(240,64,74)    // highlight red / active status text
#define COL_TEXT      RGB(235,235,238)  // primary text
#define COL_TEXT_DIS  RGB(140,140,144)  // disabled text
#define COL_BORDER    RGB(58,58,64)

static HBRUSH g_hbrBg     = NULL;
static HBRUSH g_hbrEdit   = NULL;
static HBRUSH g_hbrHeader = NULL;
static HFONT  g_hFont     = NULL;
static HFONT  g_hFontBold = NULL;
static HICON  g_hAppIcon  = NULL;

void InitThemeResources() {
    g_hbrBg     = CreateSolidBrush(COL_BG);
    g_hbrEdit   = CreateSolidBrush(COL_EDIT_BG);
    g_hbrHeader = CreateSolidBrush(COL_HEADER);
    g_hFont     = CreateFontA(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    g_hFontBold = CreateFontA(-19, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
}

// Gives a window (and its title bar, on Win10 1809+/Win11) the dark chrome to match the theme.
void EnableDarkTitleBar(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

// Rounds the window corners: Windows 11 uses the DWM corner preference (keeps
// the drop shadow), older builds fall back to a classic round-rect region.
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#define DWMWCP_ROUND 2
void EnableRoundedCorners(HWND hwnd) {
    int pref = DWMWCP_ROUND;
    if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref)))) {
        RECT rc;
        GetWindowRect(hwnd, &rc);
        HRGN rgn = CreateRoundRectRgn(0, 0, rc.right - rc.left + 1, rc.bottom - rc.top + 1, 18, 18);
        if (rgn) SetWindowRgn(hwnd, rgn, TRUE); // the system takes ownership of the region
    }
}

// Applied to every child control after creation: sets the themed font and,
// for ListView/Tab controls, switches on dark-mode visuals.
static LRESULT CALLBACK DarkHeaderProc(HWND hdr, UINT msg, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
BOOL CALLBACK ThemeChildProc(HWND hwnd, LPARAM) {
    SendMessageA(hwnd, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    char cls[64] = {0};
    GetClassNameA(hwnd, cls, sizeof(cls));
    if (_stricmp(cls, WC_LISTVIEWA) == 0) {
        SetWindowTheme(hwnd, L"DarkMode_Explorer", NULL);
        HWND hdr = ListView_GetHeader(hwnd);
        if (hdr) {
            SetWindowTheme(hdr, L"DarkMode_ItemsView", NULL);
            SetWindowSubclass(hdr, DarkHeaderProc, 1, 0);
        }
        ListView_SetBkColor(hwnd, COL_EDIT_BG);
        ListView_SetTextColor(hwnd, COL_TEXT);
        ListView_SetTextBkColor(hwnd, COL_EDIT_BG);
        DWORD ex = ListView_GetExtendedListViewStyle(hwnd);
        ListView_SetExtendedListViewStyle(hwnd, ex | LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT);
    } else if (_stricmp(cls, WC_BUTTONA) == 0) {
        // v6 themed buttons (checkboxes) ignore WM_CTLCOLORBTN; force the dark
        // theme so the label text stays white on the dark background.
        SetWindowTheme(hwnd, L"DarkMode_Explorer", NULL);
    } else if (_stricmp(cls, WC_TABCONTROLA) == 0) {
        SetWindowTheme(hwnd, L"DarkMode_Explorer", NULL);
    }
    return TRUE;
}
void ApplyThemeToChildren(HWND hwnd) {
    EnumChildWindows(hwnd, ThemeChildProc, 0);
}

// Paints the ListView column headers manually (dark background + white text)
// so they never depend on OS theme support. Subclassed onto each header.
static LRESULT CALLBACK DarkHeaderProc(HWND hdr, UINT msg, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR uIdSubclass, DWORD_PTR) {
    switch (msg) {
        case WM_NCDESTROY:
            RemoveWindowSubclass(hdr, DarkHeaderProc, uIdSubclass);
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hdr, &ps);
            RECT rc; GetClientRect(hdr, &rc);
            FillRect(dc, &rc, g_hbrBg);
            int count = Header_GetItemCount(hdr);
            for (int i = 0; i < count; ++i) {
                RECT itemRc;
                if (!Header_GetItemRect(hdr, i, &itemRc)) continue;
                if (i > 0) {
                    HBRUSH br = CreateSolidBrush(COL_ACCENT);
                    RECT sep = {itemRc.left, itemRc.top, itemRc.left + 1, itemRc.bottom};
                    FillRect(dc, &sep, br);
                    DeleteObject(br);
                }
                char buf[128];
                HDITEM hdi = {};
                hdi.mask = HDI_TEXT;
                hdi.pszText = buf;
                hdi.cchTextMax = 128;
                Header_GetItem(hdr, i, &hdi);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, COL_TEXT);
                HFONT old = (HFONT)SelectObject(dc, g_hFont);
                RECT tr = itemRc;
                tr.left += 6;
                DrawTextA(dc, buf, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                SelectObject(dc, old);
            }
            EndPaint(hdr, &ps);
            return 0;
        }
    }
    return DefSubclassProc(hdr, msg, wParam, lParam);
}

// ==============================
//  Press feedback toast: a small topmost popup next to the cursor that shows
//  "Macros stopped" / "Macros started" when the global stop/start key is pressed.
// ==============================
static HWND g_hintPopup = nullptr;
static bool g_hintShown = false;
static std::string g_hintText;
static const UINT HINT_TIMER_ID = 1;
static const int HINT_TIMEOUT_MS = 1800;

static void ShowHint(const char* text) {
    if (!g_hintPopup) return;
    g_hintText = text;
    POINT pt; GetCursorPos(&pt);
    HDC dc = GetDC(g_hintPopup);
    HFONT oldFont = (HFONT)SelectObject(dc, g_hFont);
    RECT rc = {0, 0, 300, 0};
    DrawTextA(dc, text, -1, &rc, DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL);
    SelectObject(dc, oldFont);
    ReleaseDC(g_hintPopup, dc);
    int w = rc.right + 20, h = rc.bottom + 14;
    int x = pt.x + 14, y = pt.y + 22;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    if (x + w > sw - 8) x = pt.x - w - 14;
    if (y + h > sh - 8) y = pt.y - h - 22;
    if (x < 8) x = 8;
    if (y < 8) y = 8;
    SetWindowPos(g_hintPopup, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    ShowWindow(g_hintPopup, SW_SHOWNOACTIVATE);
    g_hintShown = true;
    InvalidateRect(g_hintPopup, NULL, TRUE);
    if (g_hwndMain) {
        KillTimer(g_hwndMain, HINT_TIMER_ID);
        SetTimer(g_hwndMain, HINT_TIMER_ID, HINT_TIMEOUT_MS, NULL);
    }
}

static void HideHint() {
    if (g_hintPopup && g_hintShown) {
        ShowWindow(g_hintPopup, SW_HIDE);
        g_hintShown = false;
    }
}

LRESULT CALLBACK HintWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);
            FillRect(dc, &rc, g_hbrBg);
            HBRUSH br = CreateSolidBrush(COL_ACCENT);
            FrameRect(dc, &rc, br);
            DeleteObject(br);
            RECT tr = {10, 7, rc.right - 10, rc.bottom - 7};
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, COL_TEXT);
            HFONT oldFont = (HFONT)SelectObject(dc, g_hFont);
            DrawTextA(dc, g_hintText.c_str(), -1, &tr, DT_WORDBREAK | DT_LEFT | DT_NOPREFIX);
            SelectObject(dc, oldFont);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Owner-draw renderer shared by every flat accent push-button in the app
// (Save/Cancel/Record/New/Edit/Delete/Add/Del etc.)
void DrawThemedPushButton(LPDRAWITEMSTRUCT dis) {
    char text[128] = {0};
    GetWindowTextA(dis->hwndItem, text, sizeof(text));
    bool pressed  = (dis->itemState & ODS_SELECTED) != 0;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    bool focused  = (dis->itemState & ODS_FOCUS) != 0;

    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;

    COLORREF face = disabled ? RGB(52,52,57) : (pressed ? COL_ACCENT_DK : COL_ACCENT);
    HBRUSH br = CreateSolidBrush(face);
    HPEN pen = CreatePen(PS_SOLID, 1, focused && !disabled ? COL_ACCENT_LT : face);
    HGDIOBJ oldBr = SelectObject(dc, br);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, 6, 6);
    SelectObject(dc, oldBr);
    SelectObject(dc, oldPen);
    DeleteObject(br);
    DeleteObject(pen);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? COL_TEXT_DIS : RGB(255,255,255));
    HFONT oldFont = (HFONT)SelectObject(dc, g_hFont);
    DrawTextA(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);
}

// Shared header band drawn at the top of the Settings and Macro Editor windows.
void DrawThemedHeader(HWND hwnd, HDC dc, const char* title) {
    RECT rc; GetClientRect(hwnd, &rc);
    FillRect(dc, &rc, g_hbrBg);
    RECT header = rc; header.bottom = 54;
    FillRect(dc, &header, g_hbrHeader);
    RECT line = {0, 52, rc.right, 54};
    HBRUSH hbrLine = CreateSolidBrush(COL_ACCENT);
    FillRect(dc, &line, hbrLine);
    DeleteObject(hbrLine);
    if (g_hAppIcon) DrawIconEx(dc, 14, 10, g_hAppIcon, 32, 32, 0, NULL, DI_NORMAL);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, COL_TEXT);
    HFONT old = (HFONT)SelectObject(dc, g_hFontBold);
    TextOutA(dc, 58, 15, title, (int)strlen(title));
    SelectObject(dc, old);

    // macOS-style title bar dots: red = close, yellow = minimize
    HBRUSH hbrRed = CreateSolidBrush(RGB(255, 95, 86));
    HBRUSH hbrYellow = CreateSolidBrush(RGB(255, 189, 46));
    SelectObject(dc, hbrRed);   Ellipse(dc, rc.right - 46, 12, rc.right - 28, 30);
    SelectObject(dc, hbrYellow);Ellipse(dc, rc.right - 70, 12, rc.right - 52, 30);
    DeleteObject(hbrRed);
    DeleteObject(hbrYellow);
}

// Custom title bar hit testing for the macOS-style dots. The dots themselves
// must NOT claim HTCLOSE/HTMINBUTTON, otherwise Windows paints its own white
// caption glyphs over them. They report HTCLIENT instead (client clicks we
// handle in WM_LBUTTONDOWN). Everything else in the header band is a drag
// region (HTCAPTION).
static LRESULT HeaderHitTest(HWND hwnd, LPARAM lParam) {
    POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
    ScreenToClient(hwnd, &pt);
    RECT rc; GetClientRect(hwnd, &rc);
    if (pt.y < 54) {
        bool overDots = (pt.x >= rc.right - 70 && pt.x <= rc.right - 28 &&
                         pt.y >= 12 && pt.y <= 30);
        return overDots ? HTCLIENT : HTCAPTION;
    }
    return DefWindowProc(hwnd, WM_NCHITTEST, 0, lParam);
}

// Returns 1 = red (close) dot, 2 = yellow (minimize) dot, 0 = elsewhere.
// Expects CLIENT coordinates (WM_LBUTTONDOWN lParam is already client-relative).
static int HeaderDotAt(HWND hwnd, int x, int y) {
    RECT rc; GetClientRect(hwnd, &rc);
    if (x >= rc.right - 46 && x <= rc.right - 28 && y >= 12 && y <= 30)
        return 1;
    if (x >= rc.right - 70 && x <= rc.right - 52 && y >= 12 && y <= 30)
        return 2;
    return 0;
}

// Process a header-dot click (WM_LBUTTONDOWN lParam = client coords).
// Returns true if the click was consumed.
static bool HandleHeaderDotClick(HWND hwnd, LPARAM lParam) {
    int dot = HeaderDotAt(hwnd, (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam));
    if (dot == 1) { SendMessage(hwnd, WM_CLOSE, 0, 0); return true; }      // quit
    if (dot == 2) { SendMessage(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0); return true; }
    return false;
}

// ==============================
//  Helper: Get config path
// ==============================
void InitConfigPath() {
    char appdata[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appdata))) {
        sprintf_s(g_configPath, "%s\\OrbitMM2Macro\\config.ini", appdata);
        char dir[MAX_PATH];
        sprintf_s(dir, "%s\\OrbitMM2Macro", appdata);
        CreateDirectoryA(dir, NULL);
    } else {
        GetModuleFileNameA(NULL, g_configPath, MAX_PATH);
        char* p = strrchr(g_configPath, '\\');
        if (p) *p = '\0';
        sprintf_s(g_configPath + strlen(g_configPath), MAX_PATH - strlen(g_configPath), "\\config.ini");
    }
}

// ==============================
//  INI helpers
// ==============================
static double readDouble(const char* section, const char* key, double def) {
    char buf[64] = {0};
    GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), g_configPath);
    if (strlen(buf) == 0) return def;
    return atof(buf);
}
static int readInt(const char* section, const char* key, int def) {
    char buf[32] = {0};
    GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), g_configPath);
    if (strlen(buf) == 0) return def;
    return atoi(buf);
}
static bool readBool(const char* section, const char* key, bool def) {
    char buf[8] = {0};
    GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), g_configPath);
    if (strlen(buf) == 0) return def;
    return (buf[0] == '1' || buf[0] == 't' || buf[0] == 'T');
}
static void writeBool(const char* section, const char* key, bool val) {
    WritePrivateProfileStringA(section, key, val ? "1" : "0", g_configPath);
}
static void writeDouble(const char* section, const char* key, double val) {
    char buf[32];
    sprintf_s(buf, sizeof(buf), "%f", val);
    WritePrivateProfileStringA(section, key, buf, g_configPath);
}
static void writeInt(const char* section, const char* key, int val) {
    char buf[16];
    sprintf_s(buf, sizeof(buf), "%d", val);
    WritePrivateProfileStringA(section, key, buf, g_configPath);
}
static void writeString(const char* section, const char* key, const char* val) {
    WritePrivateProfileStringA(section, key, val, g_configPath);
}
static void readString(const char* section, const char* key, char* out, int outSize, const char* def) {
    GetPrivateProfileStringA(section, key, def, out, outSize, g_configPath);
}

// ==============================
//  Load / Save Config
// ==============================
void LoadConfig() {
    g_config.sensitivity = readDouble("Main", "Sensitivity", 0.294);
    g_config.fps = readInt("Main", "FPS", 240);
    g_config.camFix = readBool("Main", "CamFix", false);
    g_config.holdMode = readBool("SpeedGlitch", "HoldMode", false);
    g_config.glitchEnabled = readBool("SpeedGlitch", "Enabled", false);
    g_config.glitchKey = (UINT)readInt("SpeedGlitch", "TriggerKey", 0);

    g_config.sitEnabled = readBool("SitMacro", "Enabled", false);
    g_config.sitKey = (UINT)readInt("SitMacro", "TriggerKey", 0);
    g_config.sitSlot = readInt("SitMacro", "Slot", 1);

    g_config.superJumpEnabled = readBool("SuperJump", "Enabled", false);
    g_config.superJumpKey = (UINT)readInt("SuperJump", "TriggerKey", 0);
    g_config.spaceDuringJump = readBool("SuperJump", "SpaceDuringJump", true);

    g_config.equipEnabled = readBool("Equip", "Enabled", false);
    g_config.startMinimized = readBool("General", "StartMinimized", false);

    g_config.lagEnabled = readBool("LagSwitch", "Enabled", false);
    g_config.lagKey = (UINT)readInt("LagSwitch", "TriggerKey", 0);

    g_config.spamEnabled = readBool("SpamSign", "Enabled", false);
    g_config.spamKey = (UINT)readInt("SpamSign", "TriggerKey", 0);
    g_config.spamSlot = readInt("SpamSign", "Slot", 7);
    g_config.spamHoldMode = readBool("SpamSign", "HoldMode", false);

    g_config.stopAllKey = (UINT)readInt("General", "StopAllKey", 0);
    char verBuf[64] = {0};
    readString("Update", "LastNotifiedVersion", verBuf, sizeof(verBuf), "");
    g_lastNotifiedVersion = verBuf;
}

void SaveConfig() {
    writeDouble("Main", "Sensitivity", g_config.sensitivity);
    writeInt("Main", "FPS", g_config.fps);
    writeBool("Main", "CamFix", g_config.camFix);
    writeBool("SpeedGlitch", "HoldMode", g_config.holdMode);
    writeBool("SpeedGlitch", "Enabled", g_config.glitchEnabled);
    writeInt("SpeedGlitch", "TriggerKey", g_config.glitchKey);

    writeBool("SitMacro", "Enabled", g_config.sitEnabled);
    writeInt("SitMacro", "TriggerKey", g_config.sitKey);
    writeInt("SitMacro", "Slot", g_config.sitSlot);

    writeBool("SuperJump", "Enabled", g_config.superJumpEnabled);
    writeInt("SuperJump", "TriggerKey", g_config.superJumpKey);
    writeBool("SuperJump", "SpaceDuringJump", g_config.spaceDuringJump);

    writeBool("Equip", "Enabled", g_config.equipEnabled);
    writeBool("General", "StartMinimized", g_config.startMinimized);

    writeBool("LagSwitch", "Enabled", g_config.lagEnabled);
    writeInt("LagSwitch", "TriggerKey", g_config.lagKey);

    writeBool("SpamSign", "Enabled", g_config.spamEnabled);
    writeInt("SpamSign", "TriggerKey", g_config.spamKey);
    writeInt("SpamSign", "Slot", g_config.spamSlot);
    writeBool("SpamSign", "HoldMode", g_config.spamHoldMode);

    writeInt("General", "StopAllKey", g_config.stopAllKey);
}

// ==============================
//  Load / Save Macros
// ==============================
void LoadMacros() {
    g_macros.clear();
    char sections[8192] = {0};
    GetPrivateProfileSectionNamesA(sections, sizeof(sections), g_configPath);
    char* p = sections;
    while (*p) {
        std::string section(p);
        if (section.rfind("Macro_", 0) == 0) {
            std::string name = section.substr(6);
            Macro macro;
            macro.name = name;
            macro.menus = readInt(section.c_str(), "Menus", 3);
            macro.hotkey = (UINT)readInt(section.c_str(), "Hotkey", 0);
            char itemsStr[1024] = {0};
            readString(section.c_str(), "Items", itemsStr, sizeof(itemsStr), "");
            if (strlen(itemsStr) > 0) {
                char* token = strtok(itemsStr, ",");
                while (token) {
                    int menu, slot;
                    if (sscanf_s(token, "%d:%d", &menu, &slot) == 2)
                        macro.items.push_back({menu, slot});
                    token = strtok(NULL, ",");
                }
            }
            g_macros[name] = macro;
        }
        p += strlen(p) + 1;
    }
}

void SaveMacros() {
    char sections[8192] = {0};
    GetPrivateProfileSectionNamesA(sections, sizeof(sections), g_configPath);
    char* p = sections;
    while (*p) {
        std::string section(p);
        if (section.rfind("Macro_", 0) == 0)
            WritePrivateProfileStringA(section.c_str(), NULL, NULL, g_configPath);
        p += strlen(p) + 1;
    }
    for (auto& pair : g_macros) {
        const Macro& m = pair.second;
        std::string section = "Macro_" + m.name;
        writeInt(section.c_str(), "Menus", m.menus);
        writeInt(section.c_str(), "Hotkey", m.hotkey);
        std::string itemsStr;
        for (size_t i = 0; i < m.items.size(); ++i) {
            if (i > 0) itemsStr += ",";
            itemsStr += std::to_string(m.items[i].menu) + ":" + std::to_string(m.items[i].slot);
        }
        writeString(section.c_str(), "Items", itemsStr.c_str());
    }
}

// ==============================
//  Roblox focus check
// ==============================
bool IsRobloxFocused() {
    // This is called synchronously on the hook thread, BEFORE the sit/glitch/
    // jump/equip action thread is even spawned - so its cost sits directly in
    // front of every trigger key press. OpenProcess + QueryFullProcessImageNameA
    // is a real syscall round-trip (can be a few ms, more under load/AV hooking),
    // and for something as short as the sit macro (2 keystrokes) that overhead is
    // very noticeable as "lag between press and action" even though it's fixed
    // cost paid by every macro. Cache the result per foreground window so holding
    // focus on Roblox and spamming a trigger key only pays the syscall cost once,
    // on the first press after switching windows, instead of on every press.
    static std::atomic<HWND> cachedHwnd{nullptr};
    static std::atomic<bool> cachedResult{false};

    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return false;
    if (hwnd == cachedHwnd.load(std::memory_order_relaxed)) {
        return cachedResult.load(std::memory_order_relaxed);
    }

    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    bool result = false;
    if (hProc) {
        char exePath[MAX_PATH];
        DWORD size = sizeof(exePath);
        if (QueryFullProcessImageNameA(hProc, 0, exePath, &size)) {
            result = strstr(exePath, "Roblox") != nullptr;
        }
        CloseHandle(hProc);
    }

    cachedHwnd.store(hwnd, std::memory_order_relaxed);
    cachedResult.store(result, std::memory_order_relaxed);
    return result;
}

// ==============================
//  Pixel calculation (for Speed Glitch)
// ==============================
int CalculatePixelValue() {
    double base = g_config.camFix ? 500.0 : 360.0;
    double sens = g_config.sensitivity;
    if (sens <= 0.001) sens = 0.001;
    double val = (base / sens) * (359.0 / 360.0) * (359.0 / 360.0);
    return (int)round(val);
}

// ==============================
//  Input helpers – using mouse_event for better compatibility
// ==============================
void SendMouseMove(int dx, int dy) {
    mouse_event(MOUSEEVENTF_MOVE, dx, dy, 0, 0);
}

void SendKey(WORD vk, bool down) {
    WORD scan = MapVirtualKey(vk, MAPVK_VK_TO_VSC);
    if (scan == 0) {
        INPUT inp = {};
        inp.type = INPUT_KEYBOARD;
        inp.ki.wVk = vk;
        inp.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
        SendInput(1, &inp, sizeof(INPUT));
    } else {
        INPUT inp = {};
        inp.type = INPUT_KEYBOARD;
        inp.ki.wScan = scan;
        inp.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP) | KEYEVENTF_SCANCODE;
        SendInput(1, &inp, sizeof(INPUT));
    }
}

void HoldW(bool hold) {
    static bool currentlyHeld = false;
    if (hold && !currentlyHeld) {
        SendKey('W', true);
        currentlyHeld = true;
    } else if (!hold && currentlyHeld) {
        SendKey('W', false);
        currentlyHeld = false;
    }
}

// ==============================
//  Frame delay & high-res sleep
// ==============================
void FrameDelays(int fps, int& d1, int& d2) {
    if (fps < 1) fps = 1;
    double delayFloat = 1000.0 / fps;
    int delayFloor = (int)floor(delayFloat);
    if (delayFloor < 1) delayFloor = 1;
    int delayCeil = delayFloor + 1;
    double fractional = delayFloat - delayFloor;
    const double epsilon = 0.008;
    if (fractional < 0.33 - epsilon) {
        d1 = d2 = delayFloor;
    } else if (fractional > 0.66 + epsilon) {
        d1 = d2 = delayCeil;
    } else {
        d1 = delayFloor;
        d2 = delayCeil;
    }
}

void SleepMicro(int us) {
    if (us <= 0) return;
    static LARGE_INTEGER freq;
    static bool freqInit = false;
    if (!freqInit) { QueryPerformanceFrequency(&freq); freqInit = true; }
    LARGE_INTEGER start, now;
    QueryPerformanceCounter(&start);
    long long target = start.QuadPart + (us * freq.QuadPart) / 1000000;
    do {
        QueryPerformanceCounter(&now);
        if (now.QuadPart >= target) break;
        long long remaining = (target - now.QuadPart) * 1000000 / freq.QuadPart;
        if (remaining > 1000) Sleep(0);
        else std::this_thread::yield();
    } while (true);
}

// ==============================
//  Speed glitch worker
// ==============================
void SpeedGlitchWorker() {
    int pixel = CalculatePixelValue();
    int d1, d2;
    FrameDelays(g_config.fps, d1, d2);
    bool phase = false;
    while (g_glitchActive) {
        int dx = phase ? -pixel : pixel;
        int delay = phase ? d2 : d1;
        SendMouseMove(dx, 0);
        phase = !phase;
        SleepMicro(delay * 1000);
    }
}

// ==============================
//  Super Jump (Pool Super Jump)
// ==============================
void DoSuperJump() {
    if (!IsRobloxFocused() || !g_config.superJumpEnabled) return;

    double spin = 5000.0;
    double base = 0.36;
    double sens = g_config.sensitivity;
    int pixel = (int)round((spin * base) / sens);

    const int burstMs = 300;
    const int half = burstMs / 2; // 150ms — space fires somewhere in [0, half)

    // Same alternating +pixel/-pixel jitter timing Speed Glitch uses.
    int d1, d2;
    FrameDelays(g_config.fps, d1, d2);

    // Fire the jump key partway between the start and the midpoint of the spin,
    // on its own thread so its Sleep(50) doesn't stall the tight spin timing below.
    if (g_config.spaceDuringJump) {
        int spaceDelayMs = half / 2; // roughly halfway between "beginning" and "middle"
        std::thread([spaceDelayMs]() {
            SleepMicro(spaceDelayMs * 1000);
            SendKey(VK_SPACE, true);
            Sleep(50);
            SendKey(VK_SPACE, false);
        }).detach();
    }

    auto start = std::chrono::steady_clock::now();
    bool phase = false;
    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= burstMs) break;

        int dx = phase ? -pixel : pixel;
        int delay = phase ? d2 : d1;
        SendMouseMove(dx, 0);
        phase = !phase;
        SleepMicro(delay * 1000);
    }
}

// ==============================
//  Run a macro (Macro Manager)
// ==============================
void RunMacro(const Macro& macro) {
    if (!IsRobloxFocused() || !g_config.equipEnabled) return;
    const int DELAY = 10;

    int currentMenu = 1;
    SendKey('T', true); Sleep(DELAY); SendKey('T', false); Sleep(DELAY);

    for (size_t i = 0; i < macro.items.size(); ++i) {
        const MacroItem& item = macro.items[i];
        while (currentMenu < item.menu) {
            SendKey('E', true); Sleep(DELAY); SendKey('E', false); Sleep(DELAY);
            currentMenu++;
        }
        while (currentMenu > item.menu) {
            SendKey('Q', true); Sleep(DELAY); SendKey('Q', false); Sleep(DELAY);
            currentMenu--;
        }
        char num = '0' + item.slot;
        SendKey(num, true); Sleep(DELAY); SendKey(num, false); Sleep(DELAY);
        if (i < macro.items.size() - 1) {
            SendKey('T', true); Sleep(DELAY); SendKey('T', false); Sleep(DELAY);
        }
    }

    SendKey('T', true); Sleep(DELAY); SendKey('T', false); Sleep(DELAY);
    while (currentMenu > 1) {
        SendKey('Q', true); Sleep(DELAY); SendKey('Q', false); Sleep(DELAY);
        currentMenu--;
    }
    SendKey('T', true); Sleep(DELAY); SendKey('T', false); Sleep(DELAY);
    Sleep(80);
    char finalNum = '0' + (int)(macro.items.size() + 1);
    SendKey(finalNum, true); Sleep(DELAY); SendKey(finalNum, false); Sleep(DELAY);
}

// ==============================
//  Hotkey handlers (no CloseChat)
// ==============================
void OnHotkey(UINT vk, bool down) {
    if (!g_config.glitchEnabled || !IsRobloxFocused() || g_config.glitchKey == 0) return;

    // Hold Mode: while the trigger is held the speed glitch runs; releasing it stops it.
    // Toggle Mode: each full press toggles between Running and Stopped.
    if (g_config.holdMode) {
        if (down && !g_glitchActive) {
            g_glitchActive = true;
            HoldW(true);
            std::thread(SpeedGlitchWorker).detach();
            ShowHint("Speed Glitch started");
            if (g_statusLabelGlitch) SetWindowTextA(g_statusLabelGlitch, "Glitch: Running");
        } else if (!down && g_glitchActive) {
            g_glitchActive = false;
            HoldW(false);
            if (g_statusLabelGlitch) SetWindowTextA(g_statusLabelGlitch, "Glitch: Stopped");
        }
    } else {
        // Edge-trigger on the key-down only - a full press+release toggles once.
        if (!down) return;
        if (g_glitchActive) {
            g_glitchActive = false;
            HoldW(false);
            if (g_statusLabelGlitch) SetWindowTextA(g_statusLabelGlitch, "Glitch: Stopped");
        } else {
            g_glitchActive = true;
            HoldW(true);
            std::thread(SpeedGlitchWorker).detach();
            ShowHint("Speed Glitch started");
            if (g_statusLabelGlitch) SetWindowTextA(g_statusLabelGlitch, "Glitch: Running");
        }
    }
}

// ==============================
//  Spam Sign worker (Spencer "Item Clip")
// ==============================
void SpamSignWorker() {
    // Spencer default cycle: 34ms total (17ms equipped, 17ms released); only
    // while the active loop runs and Roblox is focused.
    const int halfDelay = 17;
    int slot = g_config.spamSlot < 0 ? 0 : (g_config.spamSlot > 9 ? 9 : g_config.spamSlot);
    const char key = '0' + slot;
    while (g_spamActive) {
        if (!IsRobloxFocused()) { SleepMicro(10000); continue; }
        SendKey(key, true);  Sleep(halfDelay);
        SendKey(key, false); Sleep(halfDelay);
    }
}

void OnSpamHotkey(UINT vk, bool down) {
    if (!g_config.spamEnabled || !IsRobloxFocused() || g_config.spamKey == 0) return;

    // Hold Mode: spam only while the trigger is held (Spencer's checkbox says
    // "Switch from Toggle Key to Hold Key"). Toggle Mode: press to start, press
    // again to stop.
    if (g_config.spamHoldMode) {
        if (down && !g_spamActive) {
            g_spamActive = true;
            std::thread(SpamSignWorker).detach();
            ShowHint("Spam Sign started");
            if (g_statusLabelSpam) SetWindowTextA(g_statusLabelSpam, "Spam: Running");
        } else if (!down && g_spamActive) {
            g_spamActive = false;
            if (g_statusLabelSpam) SetWindowTextA(g_statusLabelSpam, "Spam: Stopped");
        }
    } else {
        if (!down) return;
        if (g_spamActive) {
            g_spamActive = false;
            if (g_statusLabelSpam) SetWindowTextA(g_statusLabelSpam, "Spam: Stopped");
        } else {
            g_spamActive = true;
            std::thread(SpamSignWorker).detach();
            ShowHint("Spam Sign started");
            if (g_statusLabelSpam) SetWindowTextA(g_statusLabelSpam, "Spam: Running");
        }
    }
}

void OnSitHotkey(UINT vk) {
    if (!g_config.sitEnabled || !IsRobloxFocused() || g_config.sitKey == 0) return;
    // Run on a background thread — this hook callback must return quickly or
    // Windows will time it out / truncate it mid-execution (blocking Send/Sleep
    // calls directly in a WH_KEYBOARD_LL / WH_MOUSE_LL hook is unsafe).
    std::thread([]() {
        const int DELAY = 10; // matches the equip-macro timing; 50ms per step was making Sit feel laggy
        SendKey('T', true); Sleep(DELAY); SendKey('T', false); Sleep(DELAY);
        char num = '0' + g_config.sitSlot;
        SendKey(num, true); Sleep(DELAY); SendKey(num, false);
        if (g_statusLabelSit) {
            char buf[64];
            sprintf_s(buf, "Sit: sent T + %c", num);
            SetWindowTextA(g_statusLabelSit, buf);
        }
    }).detach();
}

void OnSuperJumpHotkey(UINT vk) {
    if (!g_config.superJumpEnabled || !IsRobloxFocused() || g_config.superJumpKey == 0) return;
    // DoSuperJump() blocks for ~200-300ms; running it directly inside the hook
    // caused Windows to silently cut the spin-burst short partway through,
    // which is why it only ever turned ~180° instead of completing the full spin.
    std::thread([]() {
        DoSuperJump();
        if (g_statusLabelJump) SetWindowTextA(g_statusLabelJump, "Super Jump: triggered");
    }).detach();
}

void OnMacroHotkey(UINT vk) {
    if (!IsRobloxFocused() || !g_config.equipEnabled) return;
    for (auto& pair : g_macros) {
        if (pair.second.hotkey == vk && vk != 0) {
            Macro m = pair.second; // copy — the map may change on the GUI thread
            std::thread([m]() { RunMacro(m); }).detach();
            ShowHint((pair.second.name + " started").c_str());
            break;
        }
    }
}

// ==============================
//  Global keybind lock key
//  First press: locks every macro keybind (and stops whatever is currently
//  running: speed glitch, spam sign, lag switch). While locked, pressing the
//  macro keys does nothing. Second press: unlocks the keybinds again. Works
//  globally, regardless of whether Roblox is focused.
// ==============================
void OnStopAllHotkey() {
    if (!g_keybindsLocked.load(std::memory_order_relaxed)) {
        g_keybindsLocked = true;
        bool glitchOn = g_glitchActive.load(std::memory_order_relaxed);
        bool spamOn   = g_spamActive.load(std::memory_order_relaxed);
        bool lagOn    = g_lagSwitchOn.load(std::memory_order_relaxed);
        if (glitchOn) { g_glitchActive = false; HoldW(false); }
        if (spamOn)   { g_spamActive = false; }
        if (lagOn)    { g_lagSwitchOn = false; }
        ShowHint("Macros stopped");
        if (g_statusLabelGlitch) SetWindowTextA(g_statusLabelGlitch, "Glitch: Stopped");
        if (g_statusLabelSpam)   SetWindowTextA(g_statusLabelSpam, "Spam: Stopped");
        if (g_statusLabelLag)    SetWindowTextA(g_statusLabelLag, "Lag: Off");
    } else {
        g_keybindsLocked = false;
        ShowHint("Macros started");
    }
}

// ==============================
//  Update detector (GitHub releases)
//  Polls https://api.github.com/repos/orbitthegreatest/Orbit-MM2-Macro/releases/latest
//  and notifies when a tag newer than the current build is published. Each new
//  version is only announced once (persisted in config.ini).
// ==============================
static int CompareVersions(const std::string& a, const std::string& b) {
    auto split = [](const std::string& s) {
        std::vector<int> v;
        std::string cur;
        for (size_t i = 0; i < s.size(); ++i) {
            if (isdigit((unsigned char)s[i])) cur += s[i];
            else if (!cur.empty()) { v.push_back(atoi(cur.c_str())); cur.clear(); }
        }
        if (!cur.empty()) v.push_back(atoi(cur.c_str()));
        return v;
    };
    std::vector<int> av = split(a), bv = split(b);
    size_t n = std::max(av.size(), bv.size());
    for (size_t i = 0; i < n; ++i) {
        int x = i < av.size() ? av[i] : 0;
        int y = i < bv.size() ? bv[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

static std::string ExtractTagName(const std::string& json) {
    const char* key = "\"tag_name\"";
    size_t pos = json.find(key);
    if (pos == std::string::npos) return "";
    pos += strlen(key);
    size_t q1 = json.find('"', pos);
    if (q1 == std::string::npos) return "";
    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return json.substr(q1 + 1, q2 - q1 - 1);
}

// Fetches a HTTPS URL into a string via WinHTTP. A proper User-Agent is set
// because the GitHub API rejects requests without one (HTTP 403).
static bool HttpGetString(const char* url, std::string& out) {
    out.clear();
    HINTERNET hSession = WinHttpOpen(L"OrbitMM2Macro-Updater/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    std::string host, path = "/";
    const char* p = strstr(url, "://");
    if (p) {
        p += 3;
        const char* slash = strchr(p, '/');
        if (slash) { host.assign(p, slash - p); path.assign(slash); }
        else host.assign(p);
    } else {
        WinHttpCloseHandle(hSession);
        return false;
    }

    std::wstring wHost(host.begin(), host.end());
    std::wstring wPath(path.begin(), path.end());

    bool ok = false;
    HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (hConnect) {
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wPath.c_str(), NULL,
                                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                WINHTTP_FLAG_SECURE);
        if (hRequest) {
            if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hRequest, NULL)) {
                char buf[4096];
                DWORD read = 0;
                do {
                    if (!WinHttpReadData(hRequest, buf, sizeof(buf), &read)) { read = 0; break; }
                    if (read > 0) out.append(buf, read);
                } while (read > 0);
                ok = true;
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return ok;
}

static void SetUpdateStatusText(const std::string& text) {
    if (g_hwndSettings && g_statusLabelUpdate)
        SetWindowTextA(g_statusLabelUpdate, text.c_str());
}

static void NotifyUpdateAvailable(const std::string& tag) {
    // Tray balloon (shown as a toast on Win10/11); clicking it opens the release page.
    wcscpy_s(g_nid.szInfoTitle, L"Orbit MM2 Macro - Update Available");
    std::wstring msg = L"Version " + std::wstring(tag.begin(), tag.end()) +
                       L" is now available. Click to download.";
    wcscpy_s(g_nid.szInfo, msg.c_str());
    g_nid.uFlags = NIF_INFO;
    g_nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;

    // Inline prompt to grab the new release right away.
    if (MessageBoxA(NULL, ("A new version (" + tag + ") of Orbit MM2 Macro is available!\n\n"
                           "Download it now?").c_str(), "Orbit MM2 Macro - Update",
                    MB_YESNO | MB_ICONINFORMATION) == IDYES) {
        ShellExecuteA(NULL, "open",
                      "https://github.com/orbitthegreatest/Orbit-MM2-Macro/releases/latest",
                      NULL, NULL, SW_SHOWNORMAL);
    }
    SetUpdateStatusText("Update: " + tag + " available!");
}

static void UpdateCheckWorker() {
    const char* apiUrl = "https://api.github.com/repos/orbitthegreatest/Orbit-MM2-Macro/releases/latest";
    Sleep(3000); // give the network stack a moment after launch

    while (!g_exitRequested) {
        std::string json;
        if (HttpGetString(apiUrl, json)) {
            std::string tag = ExtractTagName(json);
            if (!tag.empty()) {
                std::string latest = tag;
                if (latest[0] == 'v' || latest[0] == 'V') latest = latest.substr(1);
                std::string cur = g_currentVersion;
                if (cur[0] == 'v' || cur[0] == 'V') cur = cur.substr(1);
                std::string lastNotified = g_lastNotifiedVersion;
                if (lastNotified[0] == 'v' || lastNotified[0] == 'V') lastNotified = lastNotified.substr(1);

                if (CompareVersions(latest, cur) > 0 && CompareVersions(latest, lastNotified) > 0) {
                    g_lastNotifiedVersion = tag;
                    writeString("Update", "LastNotifiedVersion", tag.c_str());
                    NotifyUpdateAvailable(tag);
                } else {
                    SetUpdateStatusText("Update: up to date (v" + cur + ")");
                }
            } else {
                SetUpdateStatusText("Update: no release found");
            }
        } else {
            SetUpdateStatusText("Update: check failed (offline?)");
        }

        // Re-check every 30 minutes.
        for (int i = 0; i < 30 * 60 && !g_exitRequested; ++i) Sleep(1000);
    }
}

// Lag switch uses hold-mode (while the key is held Roblox UDP is blocked; release
// to resume normal traffic). This matches the Spencer screenshots where "Switch
// from Hold Key to Toggle Key" was checked — but re-reading that checkbox name,
// checking it means: change from HOLD-key behaviour to TOGGLE-key behaviour.
// We honour that interpretation: a press toggles lag on or off; key-up does nothing.
void OnLagHotkey(UINT vk, bool down) {
    if (!g_config.lagEnabled || g_config.lagKey == 0 || !IsRobloxFocused()) return;
    if (!down) return; // toggle mode - only fire on the press edge

    // Toggle is racy if pressed twice in quick succession; spawn the work so the
    // hook returns immediately and lag worker thread flips its own state safely.
    std::thread([]() {
        bool expected = g_lagSwitchOn.load(std::memory_order_relaxed);
        if (expected) {
            g_lagSwitchOn = false; // worker thread observes this and unblocks
            if (g_statusLabelLag) SetWindowTextA(g_statusLabelLag, "Lag: Off");
        } else {
            // Lazy-load WinDivert on the first toggle (extract files + register
            // driver if the exe folder is fresh). If it still isn't usable, tell
            // the user instead of silently doing nothing.
            if (!g_lagAvailable.load(std::memory_order_relaxed)) {
                EnsureLagAvailable();
            }
            if (!g_lagAvailable.load(std::memory_order_relaxed)) {
                if (g_statusLabelLag) SetWindowTextA(g_statusLabelLag, "Lag: unavailable (no WinDivert)");
                return;
            }
            g_lagSwitchOn = true;
            std::thread(LagSwitchWorker).detach();
            ShowHint("Lag Switch on");
            if (g_statusLabelLag) SetWindowTextA(g_statusLabelLag, "Lag: On");
        }
    }).detach();
}

// ==============================
//  Low-level hooks
// ==============================
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && (wParam == WM_KEYDOWN || wParam == WM_KEYUP ||
                       wParam == WM_SYSKEYDOWN || wParam == WM_SYSKEYUP)) {
        KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;
        bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) &&
                    !(p->flags & LLKHF_INJECTED) && !(p->flags & LLKHF_UP);
        bool up   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP)   &&
                    !(p->flags & LLKHF_INJECTED);
        if (down || up) {
            UINT vk = p->vkCode;
            // Normalize left/right modifier codes so recorded generic codes
            // (VK_SHIFT / VK_CONTROL / VK_MENU / VK_LWIN) match physical presses.
            if (vk == VK_LSHIFT || vk == VK_RSHIFT) vk = VK_SHIFT;
            else if (vk == VK_LCONTROL || vk == VK_RCONTROL) vk = VK_CONTROL;
            else if (vk == VK_LMENU || vk == VK_RMENU) vk = VK_MENU;
            else if (vk == VK_LWIN || vk == VK_RWIN) vk = VK_LWIN;
            // Fire only on press/release transitions (auto-repeat of a held key
            // must not re-trigger a toggle).
            static bool keyDown[256] = {false};
            if (down) {
                if (!keyDown[vk]) {
                    keyDown[vk] = true;
                    if (vk == g_config.stopAllKey && g_config.stopAllKey) OnStopAllHotkey();
                    else if (g_keybindsLocked.load(std::memory_order_relaxed)) { /* keybinds locked */ }
                    else if (vk == g_config.glitchKey && g_config.glitchKey) OnHotkey(vk, true);
                    else if (vk == g_config.lagKey && g_config.lagKey) OnLagHotkey(vk, true);
                    else if (vk == g_config.spamKey && g_config.spamKey) OnSpamHotkey(vk, true);
                    else if (vk == g_config.sitKey && g_config.sitKey) OnSitHotkey(vk);
                    else if (vk == g_config.superJumpKey && g_config.superJumpKey) OnSuperJumpHotkey(vk);
                    else {
                        for (auto& pair : g_macros)
                            if (pair.second.hotkey == vk) { OnMacroHotkey(vk); break; }
                    }
                }
            } else {
                keyDown[vk] = false;
                if (g_keybindsLocked.load(std::memory_order_relaxed)) { /* keybinds locked */ }
                else if (vk == g_config.glitchKey && g_config.glitchKey) OnHotkey(vk, false);
                else if (vk == g_config.lagKey && g_config.lagKey) OnLagHotkey(vk, false);
                else if (vk == g_config.spamKey && g_config.spamKey) OnSpamHotkey(vk, false);
            }
        }
    }
    return CallNextHookEx(g_keyboardHook, nCode, wParam, lParam);
}

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        MSLLHOOKSTRUCT* p = (MSLLHOOKSTRUCT*)lParam;
        UINT vk = 0; bool down = false; bool up = false;
        switch (wParam) {
        case WM_LBUTTONDOWN: vk = VK_LBUTTON; down = true; break;
            case WM_LBUTTONUP:   vk = VK_LBUTTON; up = true; break;
            case WM_RBUTTONDOWN: vk = VK_RBUTTON; down = true; break;
            case WM_RBUTTONUP:   vk = VK_RBUTTON; up = true; break;
            case WM_MBUTTONDOWN: vk = VK_MBUTTON; down = true; break;
            case WM_MBUTTONUP:   vk = VK_MBUTTON; up = true; break;
            case WM_XBUTTONDOWN: vk = (HIWORD(p->mouseData) & XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2; down = true; break;
            case WM_XBUTTONUP:   vk = (HIWORD(p->mouseData) & XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2; up = true; break;
        }
        if (vk) {
            if (down && vk == g_config.stopAllKey && g_config.stopAllKey) {
                OnStopAllHotkey();
            } else if (g_keybindsLocked.load(std::memory_order_relaxed)) { /* keybinds locked */ }
            else if (vk == g_config.glitchKey && g_config.glitchKey) {
                if (down || up) OnHotkey(vk, down);
            } else if (vk == g_config.lagKey && g_config.lagKey) {
                if (down || up) OnLagHotkey(vk, down);
            } else if (vk == g_config.spamKey && g_config.spamKey) {
                if (down || up) OnSpamHotkey(vk, down);
            } else if (down) {
                if (vk == g_config.sitKey && g_config.sitKey)          OnSitHotkey(vk);
                else if (vk == g_config.superJumpKey && g_config.superJumpKey) OnSuperJumpHotkey(vk);
                else {
                    for (auto& pair : g_macros)
                        if (pair.second.hotkey == vk) { OnMacroHotkey(vk); break; }
                }
            }
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

// ==============================
//  Lag switch (WinDivert-based, ports/process targeting)
//
//  Settings are hardcoded to match the Spencer macro screenshots and are NOT
//  user-changeable in the UI. Only the trigger key is personalisable:
//    toggler mode (checked "Switch from Hold to Toggle")
//    prevent Roblox disconnection (pulse every ~20s)
//    only lag switch Roblox (UDP sockets of the Roblox process)
//    block both directions (inbound + outbound)
//    fake lag: OFF, TCP: OFF, auto-unlag: OFF
//
//  WinDivert.dll + WinDivert64.sys are embedded here; they are extracted to
//  %LOCALAPPDATA%\OrbitMM2Macro on first use and the kernel driver is registered
//  via UAC once.
// ==============================

#define WINDIVERT_MTU_MAX 65535

static bool IsElevated() {
    HANDLE hToken = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) return false;
    TOKEN_ELEVATION te = {};
    DWORD size = 0;
    bool elevated = false;
    if (GetTokenInformation(hToken, TokenElevation, &te, sizeof(te), &size))
        elevated = te.TokenIsElevated != 0;
    CloseHandle(hToken);
    return elevated;
}

static void GetExeDir(char* out, int outSize)
{
    GetModuleFileNameA(NULL, out, (DWORD)outSize);
    char* p = strrchr(out, '\\');
    if (p) *p = '\0';
}

static bool ExtractResourceToFile(const char* resName, const char* resType, const char* path)
{
    HMODULE hMod = GetModuleHandle(NULL);
    HRSRC hRes = FindResourceExA(hMod, resType, resName, MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL));
    if (!hRes) return false;
    HGLOBAL hMem = LoadResource(hMod, hRes);
    if (!hMem) return false;
    DWORD size = SizeofResource(hMod, hRes);
    const char* data = (const char*)LockResource(hMem);
    if (!data || size == 0) return false;
    HANDLE hFile = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(hFile, data, size, &written, NULL);
    CloseHandle(hFile);
    return written == size;
}

// Paths derived from %LOCALAPPDATA%\OrbitMM2Macro (same folder as config.ini)
// so the exe folder stays clean. The folder is created on demand by
// EnsureWinDivertFiles.
static void GetWinDivertPaths(char* dllPath, int dllSize, char* sysPath, int sysSize)
{
    char dir[MAX_PATH];
    if (!SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, dir)) {
        sprintf_s(dir + strlen(dir), MAX_PATH - (DWORD)strlen(dir), "\\OrbitMM2Macro");
    } else {
        GetExeDir(dir, sizeof(dir));
    }
    sprintf_s(dllPath, dllSize, "%s\\WinDivert.dll", dir);
    sprintf_s(sysPath, sysSize, "%s\\WinDivert64.sys", dir);
}

// Embeds the two official WinDivert packages into the .exe and writes them out
// to %LOCALAPPDATA%\OrbitMM2Macro the first time the lag switch is used. The
// .sys must stay on disk at runtime - a kernel driver cannot be loaded straight
// from memory.
static bool EnsureWinDivertFiles()
{
    char dllPath[MAX_PATH], sysPath[MAX_PATH];
    GetWinDivertPaths(dllPath, sizeof(dllPath), sysPath, sizeof(sysPath));

    // Make sure the folder exists before writing into it.
    char dir[MAX_PATH];
    if (!SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, dir)) {
        sprintf_s(dir + strlen(dir), MAX_PATH - (DWORD)strlen(dir), "\\OrbitMM2Macro");
        CreateDirectoryA(dir, NULL);
    }

    if (GetFileAttributesA(dllPath) == INVALID_FILE_ATTRIBUTES) {
        if (!ExtractResourceToFile("WINDIVERT_DLL", "BINARY", dllPath)) return false;
    }
    if (GetFileAttributesA(sysPath) == INVALID_FILE_ATTRIBUTES) {
        if (!ExtractResourceToFile("WINDIVERT_SYS", "BINARY", sysPath)) return false;
    }
    return true;
}

// Register + start the WinDivert kernel service ("WinDivert64"). Needs admin,
// so the first activation of the lag switch prompts UAC exactly once. NOTE:
// DLLs/SYS must have been extracted already (they live in %LOCALAPPDATA%\OrbitMM2Macro).
static void RegisterWinDivertDriver()
{
    char sysPath[MAX_PATH];
    char dllDir[MAX_PATH];
    GetWinDivertPaths(dllDir, sizeof(dllDir), sysPath, sizeof(sysPath));
    static const char* svcName = "WinDivert64";

    // Build "sc create WinDivert64 type= kernel binPath=\??\C:\...\WinDivert64.sys"
    // then "sc start WinDivert64". If the service already exists, skip create.
    std::string fullCmd = "sc query \"" + std::string(svcName) + "\" >nul 2>&1 && " +
                          "sc start \"" + std::string(svcName) + "\" || " +
                          "sc create \"" + std::string(svcName) + "\" type= kernel " +
                          "start= auto binPath= \"\\??\\" + std::string(sysPath) + "\" && " +
                          "sc start \"" + std::string(svcName) + "\"";

    // The whole registration runs in one elevated cmd; UAC is shown only if the
    // current process is not already admin.
    ShellExecuteA(NULL, "runas", "cmd.exe",
                  ("/c " + fullCmd).c_str(), NULL, SW_HIDE);
}

static int UdpPortToHost(DWORD networkOrderPort)
{
    DWORD port = networkOrderPort & 0xFFFF;
    return (int)(((port & 0x00FF) << 8) | ((port & 0xFF00) >> 8));
}

// Enumerates the UDP sockets owned by the Roblox player process. The filter is
// built from the local ports, giving "only Lag Switch Roblox" without needing
// the static Roblox IANA-ish CIDR table the Spencer client keeps (its port list
// is refreshed live; we do the same by process-name discovery).
static bool IsRobloxProcess(DWORD pid)
{
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return false;
    char exePath[MAX_PATH];
    DWORD size = sizeof(exePath);
    bool ok = false;
    if (QueryFullProcessImageNameA(hProc, 0, exePath, &size)) {
        ok = (strstr(exePath, "Roblox") != NULL);
    }
    CloseHandle(hProc);
    return ok;
}

static std::set<int> QueryRobloxUdpPorts()
{
    std::set<int> ports;

    // 1) IPv4 UDP table
    ULONG bufferSize = 0;
    if (GetExtendedUdpTable(NULL, &bufferSize, TRUE, AF_INET, UDP_TABLE_OWNER_PID, 0) == ERROR_INSUFFICIENT_BUFFER &&
        bufferSize > 0) {
        std::vector<unsigned char> buffer(bufferSize);
        PMIB_UDPTABLE_OWNER_PID table = (PMIB_UDPTABLE_OWNER_PID)buffer.data();
        if (GetExtendedUdpTable(table, &bufferSize, TRUE, AF_INET, UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
            for (DWORD i = 0; i < table->dwNumEntries; ++i) {
                if (IsRobloxProcess(table->table[i].dwOwningPid))
                    ports.insert(UdpPortToHost(table->table[i].dwLocalPort));
            }
        }
    }

    // 2) IPv6 UDP table (same approach)
    ULONG bufferSize6 = 0;
    if (GetExtendedUdpTable(NULL, &bufferSize6, TRUE, AF_INET6, UDP_TABLE_OWNER_PID, 0) == ERROR_INSUFFICIENT_BUFFER &&
        bufferSize6 > 0) {
        std::vector<unsigned char> buffer6(bufferSize6);
        PMIB_UDP6TABLE_OWNER_PID table6 = (PMIB_UDP6TABLE_OWNER_PID)buffer6.data();
        if (GetExtendedUdpTable(table6, &bufferSize6, TRUE, AF_INET6, UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
            for (DWORD i = 0; i < table6->dwNumEntries; ++i) {
                DWORD pid = table6->table[i].dwOwningPid;
                if (IsRobloxProcess(pid))
                    ports.insert(UdpPortToHost(table6->table[i].dwLocalPort));
            }
        }
    }
    return ports;
}

// Worker: opens a WinDivert handle and drops matching Roblox UDP traffic while
// the toggle is ON. Every ~19.9s it sends a short safety burst (prevent-disconnect).
static void LagSwitchWorker()
{
    if (!g_fnWinDivertOpen || !g_fnWinDivertRecv || !g_fnWinDivertSend ||
        !g_fnWinDivertClose) return;

    const long long PULSE_INTERVAL_MS = 19900; // ~20s pulse to avoid Roblox kick
    const long long PULSE_WINDOW_MS   = 250;   // let packets through briefly each pulse

    WINDIVERT_ADDRESS addr;
    char* packet = new char[WINDIVERT_MTU_MAX];

    while (g_lagSwitchOn.load(std::memory_order_relaxed)) {
        std::set<int> ports = QueryRobloxUdpPorts();
        std::string filter = "udp";
        if (!ports.empty()) {
            std::string portFilter;
            bool first = true;
            for (int port : ports) {
                if (!first) portFilter += " or ";
                portFilter += "((outbound and udp.SrcPort == " + std::to_string(port) + ") or " +
                              "(inbound and udp.DstPort == " + std::to_string(port) + "))";
                first = false;
            }
            filter += " and (" + portFilter + ")";
        } else {
            filter += " and false"; // no Roblox UDP sockets found
        }

        WINDIVERT_HANDLE h = g_fnWinDivertOpen(filter.c_str(), 0, 0, 0);
        if (h == NULL || h == (WINDIVERT_HANDLE)-1) {
            Sleep(500);
            continue;
        }
        auto pulseStart = std::chrono::steady_clock::now();

        while (g_lagSwitchOn.load(std::memory_order_relaxed)) {
            UINT packetLength = 0;
            if (!g_fnWinDivertRecv(h, packet, WINDIVERT_MTU_MAX, &packetLength, &addr)) {
                break;
            }
            auto now = std::chrono::steady_clock::now();
            long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - pulseStart).count();
            bool inPulse = (elapsed >= PULSE_INTERVAL_MS) &&
                           (elapsed < PULSE_INTERVAL_MS + PULSE_WINDOW_MS);
            // Once the pulse window finishes, start counting the next 20s.
            if (elapsed >= PULSE_INTERVAL_MS + PULSE_WINDOW_MS) {
                pulseStart = now;
            }
            if (inPulse) {
                // Prevent-disconnect: briefly let traffic pass.
                g_fnWinDivertSend(h, packet, packetLength, NULL, &addr);
            }
            // Otherwise: hard block - the packet is simply not re-injected.
        }
        g_fnWinDivertClose(h);
    }

    delete[] packet;
}

// Prompts for elevation once, then registers the driver + reloads the lib. If we
// already are elevated this just loads and returns whether we can open a handle.
static void EnsureLagAvailable()
{
    if (g_lagAvailable.load(std::memory_order_relaxed)) return;

    if (!EnsureWinDivertFiles()) {
        if (g_statusLabelLag) SetWindowTextA(g_statusLabelLag, "Lag: WinDivert files missing");
        return;
    }

    char dllPath[MAX_PATH], sysPath[MAX_PATH];
    GetWinDivertPaths(dllPath, sizeof(dllPath), sysPath, sizeof(sysPath));
    g_winDivertDll = LoadLibraryA(dllPath);
    if (!g_winDivertDll) {
        if (g_statusLabelLag) SetWindowTextA(g_statusLabelLag, "Lag: DLL load failed");
        return;
    }
    g_fnWinDivertOpen  = (WinDivertOpenFunc) GetProcAddress(g_winDivertDll, "WinDivertOpen");
    g_fnWinDivertRecv  = (WinDivertRecvFunc) GetProcAddress(g_winDivertDll, "WinDivertRecv");
    g_fnWinDivertSend  = (WinDivertSendFunc) GetProcAddress(g_winDivertDll, "WinDivertSend");
    g_fnWinDivertClose = (WinDivertCloseFunc)GetProcAddress(g_winDivertDll, "WinDivertClose");
    if (!g_fnWinDivertOpen || !g_fnWinDivertRecv || !g_fnWinDivertSend || !g_fnWinDivertClose) {
        SetWindowTextA(g_statusLabelLag, "Lag: DLL exports missing");
        return;
    }

    // Try opening a handle - if this returns access denied we must be elevated.
    WINDIVERT_HANDLE test = g_fnWinDivertOpen("false", 0, 0, 0);
    if (test == NULL || test == (WINDIVERT_HANDLE)-1) {
        DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            SetWindowTextA(g_statusLabelLag, "Lag: elevation needed");
            // Registering the driver requires an admin shell; use UAC prompt.
            RegisterWinDivertDriver();
            Sleep(1500); // give the elevated cmd a moment to register + start
            test = g_fnWinDivertOpen("false", 0, 0, 0);
            if (test == NULL || test == (WINDIVERT_HANDLE)-1) {
                SetWindowTextA(g_statusLabelLag, "Lag: driver failed");
                return;
            }
        } else {
            SetWindowTextA(g_statusLabelLag, "Lag: driver failed");
            return;
        }
    }
    if (test) g_fnWinDivertClose(test);
    g_lagAvailable.store(true, std::memory_order_relaxed);
}

// ==============================
//  Key name mapping
// ==============================
std::string KeyName(UINT vk) {
    if (vk == 0) return "None";
    if (vk == VK_LBUTTON) return "LButton";
    if (vk == VK_RBUTTON) return "RButton";
    if (vk == VK_MBUTTON) return "MButton";
    if (vk == VK_XBUTTON1) return "XButton1";
    if (vk == VK_XBUTTON2) return "XButton2";
    if (vk >= 'A' && vk <= 'Z') return std::string(1, (char)vk);
    if (vk >= '0' && vk <= '9') return std::string(1, (char)vk);
    switch (vk) {
        case VK_SPACE:   return "Space";
        case VK_RETURN:  return "Enter";
        case VK_TAB:     return "Tab";
        case VK_BACK:    return "Backspace";
        case VK_ESCAPE:  return "Escape";
        case VK_UP:      return "Up";
        case VK_DOWN:    return "Down";
        case VK_LEFT:    return "Left";
        case VK_RIGHT:   return "Right";
        case VK_INSERT:  return "Insert";
        case VK_DELETE:  return "Delete";
        case VK_HOME:    return "Home";
        case VK_END:     return "End";
        case VK_PRIOR:   return "PgUp";
        case VK_NEXT:    return "PgDn";
        case VK_F1:      return "F1";
        case VK_F2:      return "F2";
        case VK_F3:      return "F3";
        case VK_F4:      return "F4";
        case VK_F5:      return "F5";
        case VK_F6:      return "F6";
        case VK_F7:      return "F7";
        case VK_F8:      return "F8";
        case VK_F9:      return "F9";
        case VK_F10:     return "F10";
        case VK_F11:     return "F11";
        case VK_F12:     return "F12";
        case VK_LSHIFT:  return "LShift";
        case VK_RSHIFT:  return "RShift";
        case VK_LCONTROL:return "LCtrl";
        case VK_RCONTROL:return "RCtrl";
        case VK_LMENU:   return "LAlt";
        case VK_RMENU:   return "RAlt";
        case VK_LWIN:    return "LWin";
        case VK_RWIN:    return "RWin";
        case VK_NUMPAD0: return "Numpad0";
        case VK_NUMPAD1: return "Numpad1";
        case VK_NUMPAD2: return "Numpad2";
        case VK_NUMPAD3: return "Numpad3";
        case VK_NUMPAD4: return "Numpad4";
        case VK_NUMPAD5: return "Numpad5";
        case VK_NUMPAD6: return "Numpad6";
        case VK_NUMPAD7: return "Numpad7";
        case VK_NUMPAD8: return "Numpad8";
        case VK_NUMPAD9: return "Numpad9";
        case VK_MULTIPLY:return "Numpad*";
        case VK_ADD:     return "Numpad+";
        case VK_SUBTRACT:return "Numpad-";
        case VK_DIVIDE:  return "Numpad/";
        case VK_OEM_PLUS:return "+";
        case VK_OEM_MINUS:return "-";
        case VK_OEM_1:   return ";";
        case VK_OEM_2:   return "/";
        case VK_OEM_3:   return "`";
        case VK_OEM_4:   return "[";
        case VK_OEM_5:   return "\\";
        case VK_OEM_6:   return "]";
        case VK_OEM_7:   return "'";
        case VK_OEM_COMMA:return ",";
        case VK_OEM_PERIOD:return ".";
        default: {
            char buf[16];
            sprintf_s(buf, "0x%02X", vk);
            return std::string(buf);
        }
    }
}

// ==============================
//  Timer for key recording
// ==============================
void CALLBACK RecordTimerProc(HWND hwnd, UINT, UINT_PTR, DWORD) {
    if (!g_recording) return;
    for (int vk = 1; vk <= 255; ++vk) {
        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON ||
            vk == VK_XBUTTON1 || vk == VK_XBUTTON2) continue;
        if (GetAsyncKeyState(vk) & 0x8000) {
            g_recording = false; KillTimer(hwnd, 1);
            UINT key = (UINT)vk;
            std::string name = KeyName(key);
            if (g_recordingTarget == 0) {
                g_config.glitchKey = key;
                SetDlgItemTextA(hwnd, 113, name.c_str());
                EnableWindow(GetDlgItem(hwnd, 114), TRUE);
                SetDlgItemTextA(hwnd, 114, "Record");
            } else if (g_recordingTarget == 1) {
                g_config.sitKey = key;
                SetDlgItemTextA(hwnd, 202, name.c_str());
                EnableWindow(GetDlgItem(hwnd, 203), TRUE);
                SetDlgItemTextA(hwnd, 203, "Record");
            } else if (g_recordingTarget == 2) {
                g_config.superJumpKey = key;
                SetDlgItemTextA(hwnd, 302, name.c_str());
                EnableWindow(GetDlgItem(hwnd, 303), TRUE);
                SetDlgItemTextA(hwnd, 303, "Record");
            } else if (g_recordingTarget == 3) {
                g_config.lagKey = key;
                SetDlgItemTextA(hwnd, 413, name.c_str());
                EnableWindow(GetDlgItem(hwnd, 414), TRUE);
                SetDlgItemTextA(hwnd, 414, "Record");
            } else if (g_recordingTarget == 4) {
                g_config.spamKey = key;
                SetDlgItemTextA(hwnd, 513, name.c_str());
                EnableWindow(GetDlgItem(hwnd, 514), TRUE);
                SetDlgItemTextA(hwnd, 514, "Record");
            } else if (g_recordingTarget == 5) {
                g_config.stopAllKey = key;
                SetDlgItemTextA(hwnd, 601, name.c_str());
                EnableWindow(GetDlgItem(hwnd, 602), TRUE);
                SetDlgItemTextA(hwnd, 602, "Record");
            }
            return;
        }
    }
    UINT mouseKeys[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    const char* mouseNames[] = {"LButton","RButton","MButton","XButton1","XButton2"};
    for (int i = 0; i < 5; ++i) {
        if (GetAsyncKeyState(mouseKeys[i]) & 0x8000) {
            g_recording = false; KillTimer(hwnd, 1);
            UINT key = mouseKeys[i];
            const char* name = mouseNames[i];
            if (g_recordingTarget == 0) {
                g_config.glitchKey = key;
                SetDlgItemTextA(hwnd, 113, name);
                EnableWindow(GetDlgItem(hwnd, 114), TRUE);
                SetDlgItemTextA(hwnd, 114, "Record");
            } else if (g_recordingTarget == 1) {
                g_config.sitKey = key;
                SetDlgItemTextA(hwnd, 202, name);
                EnableWindow(GetDlgItem(hwnd, 203), TRUE);
                SetDlgItemTextA(hwnd, 203, "Record");
            } else if (g_recordingTarget == 2) {
                g_config.superJumpKey = key;
                SetDlgItemTextA(hwnd, 302, name);
                EnableWindow(GetDlgItem(hwnd, 303), TRUE);
                SetDlgItemTextA(hwnd, 303, "Record");
            } else if (g_recordingTarget == 3) {
                g_config.lagKey = key;
                SetDlgItemTextA(hwnd, 413, name);
                EnableWindow(GetDlgItem(hwnd, 414), TRUE);
                SetDlgItemTextA(hwnd, 414, "Record");
            } else if (g_recordingTarget == 4) {
                g_config.spamKey = key;
                SetDlgItemTextA(hwnd, 513, name);
                EnableWindow(GetDlgItem(hwnd, 514), TRUE);
                SetDlgItemTextA(hwnd, 514, "Record");
            } else if (g_recordingTarget == 5) {
                g_config.stopAllKey = key;
                SetDlgItemTextA(hwnd, 601, name);
                EnableWindow(GetDlgItem(hwnd, 602), TRUE);
                SetDlgItemTextA(hwnd, 602, "Record");
            }
            return;
        }
    }
}

// ==============================
//  Settings window
// ==============================
LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hTab;

    switch (msg) {
        case WM_NCHITTEST:
            return HeaderHitTest(hwnd, lParam);
        case WM_LBUTTONDOWN:
            // Red/yellow dots are client-area fake buttons: intercept the press
            // and act (close the whole macro / hide to tray) instead of letting
            // Windows paint its own caption glyphs over them.
            if (HandleHeaderDotClick(hwnd, lParam)) return 0;
            break;
        case WM_SHOWWINDOW: {
            RECT rc;
            GetWindowRect(hwnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;
            int screenW = GetSystemMetrics(SM_CXSCREEN);
            int screenH = GetSystemMetrics(SM_CYSCREEN);
            SetWindowPos(hwnd, NULL, (screenW - w) / 2, (screenH - h) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            break;
        }
        case WM_CREATE: {
            int w = 740, h = 470;
            int screenW = GetSystemMetrics(SM_CXSCREEN);
            int screenH = GetSystemMetrics(SM_CYSCREEN);
            SetWindowPos(hwnd, NULL, (screenW - w) / 2, (screenH - h) / 2, w, h, SWP_NOZORDER);
            EnableDarkTitleBar(hwnd);
            EnableRoundedCorners(hwnd);

            hTab = CreateWindowExA(0, WC_TABCONTROL, NULL,
                                   WS_CHILD | WS_VISIBLE,
                                   16, 64, 708, 310, hwnd, (HMENU)100, GetModuleHandle(NULL), NULL);

            TCITEMA tie = {};
            tie.mask = TCIF_TEXT;
            const char* tabLabels[] = { "Main", "Speed Glitch", "Sit Macro", "Pool Super Jump", "Equip Items", "Lag Switch", "Spam Sign" };
            for (int i = 0; i < 7; ++i) {
                tie.pszText = (char*)tabLabels[i];
                TabCtrl_InsertItem(hTab, i, &tie);
            }
            TabCtrl_SetCurSel(hTab, 0);

            // --- Tab 0: Main ---
            CreateWindowExA(0, "STATIC", "Roblox Sensitivity:", WS_CHILD | WS_VISIBLE, 36, 106, 160, 20, hwnd, (HMENU)1101, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER, 210, 103, 90, 24, hwnd, (HMENU)110, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "STATIC", "Roblox FPS:", WS_CHILD | WS_VISIBLE, 36, 146, 160, 20, hwnd, (HMENU)1102, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER, 210, 143, 90, 24, hwnd, (HMENU)111, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "BUTTON", "Start Minimized", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 36, 186, 200, 25, hwnd, (HMENU)501, GetModuleHandle(NULL), NULL);

            // Global stop/restart-all key + update status live on the Main tab too.
            CreateWindowExA(0, "STATIC", "Stop All Key:", WS_CHILD | WS_VISIBLE, 36, 226, 100, 20, hwnd, (HMENU)1103, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_UPPERCASE, 150, 223, 80, 24, hwnd, (HMENU)601, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "BUTTON", "Record", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 240, 223, 70, 24, hwnd, (HMENU)602, GetModuleHandle(NULL), NULL);
            g_statusLabelUpdate = CreateWindowExA(0, "STATIC", "Update: checking...", WS_CHILD | WS_VISIBLE, 36, 266, 400, 20, hwnd, (HMENU)603, GetModuleHandle(NULL), NULL);

            // --- Tab 1: Speed Glitch ---
            CreateWindowExA(0, "BUTTON", "Hold Mode", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 36, 106, 150, 25, hwnd, (HMENU)112, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "STATIC", "Glitch Key:", WS_CHILD | WS_VISIBLE, 36, 148, 100, 20, hwnd, (HMENU)1104, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_UPPERCASE, 150, 145, 80, 24, hwnd, (HMENU)113, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "BUTTON", "Record", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 240, 145, 70, 24, hwnd, (HMENU)114, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "BUTTON", "Enabled", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 36, 188, 100, 25, hwnd, (HMENU)115, GetModuleHandle(NULL), NULL);
            g_statusLabelGlitch = CreateWindowExA(0, "STATIC", "Glitch: idle", WS_CHILD | WS_VISIBLE, 36, 228, 400, 20, hwnd, (HMENU)116, GetModuleHandle(NULL), NULL);

            // --- Tab 2: Sit Macro ---
            CreateWindowExA(0, "STATIC", "Sit Key:", WS_CHILD | WS_VISIBLE, 36, 108, 100, 20, hwnd, (HMENU)1201, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_UPPERCASE, 150, 105, 80, 24, hwnd, (HMENU)202, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "BUTTON", "Record", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 240, 105, 70, 24, hwnd, (HMENU)203, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "STATIC", "Sit Slot:", WS_CHILD | WS_VISIBLE, 36, 150, 100, 20, hwnd, (HMENU)1202, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
                            150, 147, 80, 120, hwnd, (HMENU)204, GetModuleHandle(NULL), NULL);
            for (int i = 0; i <= 9; ++i) {
                char buf[8]; sprintf_s(buf, "%d", i);
                SendDlgItemMessageA(hwnd, 204, CB_ADDSTRING, 0, (LPARAM)buf);
            }
            CreateWindowExA(0, "BUTTON", "Enabled", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 36, 190, 100, 25, hwnd, (HMENU)205, GetModuleHandle(NULL), NULL);
            g_statusLabelSit = CreateWindowExA(0, "STATIC", "Sit: idle", WS_CHILD | WS_VISIBLE, 36, 228, 400, 20, hwnd, (HMENU)206, GetModuleHandle(NULL), NULL);

            // --- Tab 3: Pool Super Jump ---
            CreateWindowExA(0, "STATIC", "Trigger Key:", WS_CHILD | WS_VISIBLE, 36, 108, 100, 20, hwnd, (HMENU)1301, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_UPPERCASE, 150, 105, 80, 24, hwnd, (HMENU)302, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "BUTTON", "Record", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 240, 105, 70, 24, hwnd, (HMENU)303, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "BUTTON", "Enabled", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 36, 148, 100, 25, hwnd, (HMENU)306, GetModuleHandle(NULL), NULL);
            g_statusLabelJump = CreateWindowExA(0, "STATIC", "Super Jump: idle", WS_CHILD | WS_VISIBLE, 36, 188, 400, 20, hwnd, (HMENU)308, GetModuleHandle(NULL), NULL);

            // --- Tab 4: Equip Items ---
            HWND hLV = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEW, "",
                                       WS_CHILD | WS_VISIBLE | LVS_REPORT,
                                       36, 104, 668, 168, hwnd, (HMENU)401, GetModuleHandle(NULL), NULL);
            ListView_SetExtendedListViewStyle(hLV, LVS_EX_FULLROWSELECT);
            LVCOLUMNA col = {}; col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.pszText = (LPSTR)"Name"; col.cx = 260; ListView_InsertColumn(hLV, 0, &col);
            col.pszText = (LPSTR)"Hotkey"; col.cx = 180; ListView_InsertColumn(hLV, 1, &col);
            col.pszText = (LPSTR)"Items"; col.cx = 100; ListView_InsertColumn(hLV, 2, &col);
            CreateWindowExA(0, "BUTTON", "New", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 36, 284, 80, 28, hwnd, (HMENU)402, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "BUTTON", "Edit", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 126, 284, 80, 28, hwnd, (HMENU)403, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "BUTTON", "Delete", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 216, 284, 80, 28, hwnd, (HMENU)404, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "BUTTON", "Enabled", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 320, 288, 120, 25, hwnd, (HMENU)405, GetModuleHandle(NULL), NULL);

            // --- Tab 5: Lag Switch ---
            CreateWindowExA(0, "STATIC", "Trigger Key:", WS_CHILD | WS_VISIBLE, 36, 108, 100, 20, hwnd, (HMENU)1401, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_UPPERCASE, 150, 105, 80, 24, hwnd, (HMENU)413, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "BUTTON", "Record", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 240, 105, 70, 24, hwnd, (HMENU)414, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "BUTTON", "Enabled", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 36, 150, 100, 25, hwnd, (HMENU)415, GetModuleHandle(NULL), NULL);
            g_statusLabelLag = CreateWindowExA(0, "STATIC", "Lag: idle", WS_CHILD | WS_VISIBLE, 36, 190, 400, 20, hwnd, (HMENU)416, GetModuleHandle(NULL), NULL);

            // --- Tab 6: Spam Sign (Spencer "Item Clip") ---
            CreateWindowExA(0, "STATIC", "Trigger Key:", WS_CHILD | WS_VISIBLE, 36, 108, 100, 20, hwnd, (HMENU)1601, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_UPPERCASE, 150, 105, 80, 24, hwnd, (HMENU)513, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "BUTTON", "Record", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 240, 105, 70, 24, hwnd, (HMENU)514, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "BUTTON", "Hold Key", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 36, 150, 150, 25, hwnd, (HMENU)515, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "STATIC", "Item Slot:", WS_CHILD | WS_VISIBLE, 36, 195, 100, 20, hwnd, (HMENU)1602, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
                            150, 192, 80, 150, hwnd, (HMENU)516, GetModuleHandle(NULL), NULL);
            for (int i = 1; i <= 9; ++i) {
                char buf[8]; sprintf_s(buf, "%d", i);
                SendDlgItemMessageA(hwnd, 516, CB_ADDSTRING, 0, (LPARAM)buf);
            }
            CreateWindowExA(0, "BUTTON", "Enabled", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 36, 240, 100, 25, hwnd, (HMENU)517, GetModuleHandle(NULL), NULL);
            g_statusLabelSpam = CreateWindowExA(0, "STATIC", "Spam: idle", WS_CHILD | WS_VISIBLE, 36, 278, 400, 20, hwnd, (HMENU)518, GetModuleHandle(NULL), NULL);

            HWND hLV2 = GetDlgItem(hwnd, 401);
            int idx = 0;
            for (auto& pair : g_macros) {
                const Macro& m = pair.second;
                char itemsCount[8]; sprintf_s(itemsCount, "%d", (int)m.items.size());
                LVITEMA lv = {}; lv.mask = LVIF_TEXT; lv.iItem = idx; lv.iSubItem = 0;
                lv.pszText = (LPSTR)m.name.c_str();
                int item = ListView_InsertItem(hLV2, &lv);
                ListView_SetItemText(hLV2, item, 1, (LPSTR)KeyName(m.hotkey).c_str());
                ListView_SetItemText(hLV2, item, 2, itemsCount);
                idx++;
            }

            // ---- Save/Cancel ----
            CreateWindowExA(0, "BUTTON", "Save", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 500, 396, 110, 32, hwnd, (HMENU)IDOK, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "BUTTON", "Cancel", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 618, 396, 110, 32, hwnd, (HMENU)IDCANCEL, GetModuleHandle(NULL), NULL);

            // Load values
            char buf[64];
            sprintf_s(buf, "%f", g_config.sensitivity); SetDlgItemTextA(hwnd, 110, buf);
            sprintf_s(buf, "%d", g_config.fps); SetDlgItemTextA(hwnd, 111, buf);
            CheckDlgButton(hwnd, 501, g_config.startMinimized ? BST_CHECKED : BST_UNCHECKED);
            SetDlgItemTextA(hwnd, 601, KeyName(g_config.stopAllKey).c_str());

            CheckDlgButton(hwnd, 112, g_config.holdMode ? BST_CHECKED : BST_UNCHECKED);
            SetDlgItemTextA(hwnd, 113, KeyName(g_config.glitchKey).c_str());
            CheckDlgButton(hwnd, 115, g_config.glitchEnabled ? BST_CHECKED : BST_UNCHECKED);

            SetDlgItemTextA(hwnd, 202, KeyName(g_config.sitKey).c_str());
            SendDlgItemMessageA(hwnd, 204, CB_SETCURSEL, g_config.sitSlot, 0);
            CheckDlgButton(hwnd, 205, g_config.sitEnabled ? BST_CHECKED : BST_UNCHECKED);

            SetDlgItemTextA(hwnd, 302, KeyName(g_config.superJumpKey).c_str());
            CheckDlgButton(hwnd, 306, g_config.superJumpEnabled ? BST_CHECKED : BST_UNCHECKED);

            CheckDlgButton(hwnd, 405, g_config.equipEnabled ? BST_CHECKED : BST_UNCHECKED);

            SetDlgItemTextA(hwnd, 413, KeyName(g_config.lagKey).c_str());
            CheckDlgButton(hwnd, 415, g_config.lagEnabled ? BST_CHECKED : BST_UNCHECKED);

            SetDlgItemTextA(hwnd, 513, KeyName(g_config.spamKey).c_str());
            CheckDlgButton(hwnd, 515, g_config.spamHoldMode ? BST_CHECKED : BST_UNCHECKED);
            SendDlgItemMessageA(hwnd, 516, CB_SETCURSEL, g_config.spamSlot - 1, 0);
            CheckDlgButton(hwnd, 517, g_config.spamEnabled ? BST_CHECKED : BST_UNCHECKED);

            // Show only Main
int allIds[] = {110,111,501,601,602,603,1101,1102,1103,
                            112,113,114,115,116,1104,
                            202,203,204,205,206,1201,1202,
                            302,303,306,308,1301,
                                401,402,403,404,405,
                                413,414,415,416,1401,
                                513,514,515,516,517,518,1601,1602};
            for (int id : allIds) ShowWindow(GetDlgItem(hwnd, id), SW_HIDE);
            for (int id : {110,111,501,601,602,603,1101,1102,1103}) ShowWindow(GetDlgItem(hwnd, id), SW_SHOW);

            ApplyThemeToChildren(hwnd);
            SendMessageA(hwnd, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            break;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            DrawThemedHeader(hwnd, dc, "Orbit MM2 Macro - Settings");
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HWND ctl = (HWND)lParam;
            int id = GetDlgCtrlID(ctl);
            HDC dc = (HDC)wParam;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, (id == 116 || id == 206 || id == 308 || id == 416 || id == 518) ? COL_ACCENT_LT : COL_TEXT);
            return (LRESULT)g_hbrBg;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC dc = (HDC)wParam;
            SetTextColor(dc, COL_TEXT);
            SetBkColor(dc, COL_EDIT_BG);
            SetBkMode(dc, OPAQUE);
            return (LRESULT)g_hbrEdit;
        }
        case WM_CTLCOLORBTN: {
            HDC dc = (HDC)wParam;
            SetTextColor(dc, COL_TEXT);
            SetBkMode(dc, TRANSPARENT);
            return (LRESULT)g_hbrBg;
        }
        case WM_DRAWITEM: {
            LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lParam;
            if (dis->CtlType == ODT_BUTTON) { DrawThemedPushButton(dis); return TRUE; }
            break;
        }

        case WM_NOTIFY: {
            NMHDR* hdr = (NMHDR*)lParam;
            if (hdr->idFrom == 100 && hdr->code == TCN_SELCHANGE) {
                int tab = TabCtrl_GetCurSel(hTab);
int allIds[] = {110,111,501,601,602,603,1101,1102,1103,
                                112,113,114,115,116,1104,
                                202,203,204,205,206,1201,1202,
                                302,303,306,308,1301,
                                401,402,403,404,405,
                                413,414,415,416,1401,
                                513,514,515,516,517,518,1601,1602};
                for (int id : allIds) ShowWindow(GetDlgItem(hwnd, id), SW_HIDE);
                switch (tab) {
                    case 0: for (int id : {110,111,501,601,602,603,1101,1102,1103}) ShowWindow(GetDlgItem(hwnd,id), SW_SHOW); break;
                    case 1: for (int id : {112,113,114,115,116,1104}) ShowWindow(GetDlgItem(hwnd,id), SW_SHOW); break;
                    case 2: for (int id : {202,203,204,205,206,1201,1202}) ShowWindow(GetDlgItem(hwnd,id), SW_SHOW); break;
                    case 3: for (int id : {302,303,306,308,1301}) ShowWindow(GetDlgItem(hwnd,id), SW_SHOW); break;
                    case 4: for (int id : {401,402,403,404,405}) ShowWindow(GetDlgItem(hwnd,id), SW_SHOW); break;
                    case 5: for (int id : {413,414,415,416,1401}) ShowWindow(GetDlgItem(hwnd,id), SW_SHOW); break;
                    case 6: for (int id : {513,514,515,516,517,518,1601,1602}) ShowWindow(GetDlgItem(hwnd,id), SW_SHOW); break;
                }
                return 0;
            }
            break;
        }

        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case 114: // Record glitch
                    if (!g_recording) { g_recording = true; g_recordingTarget = 0;
                        SetDlgItemTextA(hwnd, 114, "..."); EnableWindow(GetDlgItem(hwnd, 114), FALSE);
                        SetTimer(hwnd, 1, 50, (TIMERPROC)RecordTimerProc);
                    }
                    break;
                case 203: // Record sit
                    if (!g_recording) { g_recording = true; g_recordingTarget = 1;
                        SetDlgItemTextA(hwnd, 203, "..."); EnableWindow(GetDlgItem(hwnd, 203), FALSE);
                        SetTimer(hwnd, 1, 50, (TIMERPROC)RecordTimerProc);
                    }
                    break;
                case 303: // Record super jump
                    if (!g_recording) { g_recording = true; g_recordingTarget = 2;
                        SetDlgItemTextA(hwnd, 303, "..."); EnableWindow(GetDlgItem(hwnd, 303), FALSE);
                        SetTimer(hwnd, 1, 50, (TIMERPROC)RecordTimerProc);
                    }
                    break;
                case 414: // Record lag switch key
                    if (!g_recording) { g_recording = true; g_recordingTarget = 3;
                        SetDlgItemTextA(hwnd, 414, "..."); EnableWindow(GetDlgItem(hwnd, 414), FALSE);
                        SetTimer(hwnd, 1, 50, (TIMERPROC)RecordTimerProc);
                    }
                    break;
                case 514: // Record spam key
                    if (!g_recording) { g_recording = true; g_recordingTarget = 4;
                        SetDlgItemTextA(hwnd, 514, "..."); EnableWindow(GetDlgItem(hwnd, 514), FALSE);
                        SetTimer(hwnd, 1, 50, (TIMERPROC)RecordTimerProc);
                    }
                    break;
                case 602: // Record stop-all key
                    if (!g_recording) { g_recording = true; g_recordingTarget = 5;
                        SetDlgItemTextA(hwnd, 602, "..."); EnableWindow(GetDlgItem(hwnd, 602), FALSE);
                        SetTimer(hwnd, 1, 50, (TIMERPROC)RecordTimerProc);
                    }
                    break;
                case 402: // New macro
                case 403: // Edit macro
                {
                    HWND hLV = GetDlgItem(hwnd, 401);
                    int sel = ListView_GetNextItem(hLV, -1, LVNI_SELECTED);
                    if (LOWORD(wParam) == 402) {
                        g_editingMacro = Macro();
                        g_editingNew = true;
                    } else {
                        if (sel == -1) { MessageBoxA(hwnd, "Select a macro", "Info", MB_OK); break; }
                        char name[128];
                        ListView_GetItemText(hLV, sel, 0, name, sizeof(name));
                        if (g_macros.find(name) == g_macros.end()) break;
                        g_editingMacro = g_macros[name];
                        g_editingNew = false;
                    }
                    if (g_hwndMacroEditor) DestroyWindow(g_hwndMacroEditor);
                    g_hwndMacroEditor = CreateWindowExA(0, "MacroEditorClass", "Edit Macro",
                                                        WS_POPUP | WS_BORDER | WS_SYSMENU | WS_MINIMIZEBOX,
                                                        CW_USEDEFAULT, CW_USEDEFAULT, 340, 410,
                                                        hwnd, NULL, GetModuleHandle(NULL), NULL);
                    ShowWindow(g_hwndMacroEditor, SW_SHOW);
                    break;
                }
                case 404: { // Delete macro
                    HWND hLV = GetDlgItem(hwnd, 401);
                    int sel = ListView_GetNextItem(hLV, -1, LVNI_SELECTED);
                    if (sel == -1) { MessageBoxA(hwnd, "Select a macro", "Info", MB_OK); break; }
                    char name[128];
                    ListView_GetItemText(hLV, sel, 0, name, sizeof(name));
                    if (MessageBoxA(hwnd, ("Delete '" + std::string(name) + "'?").c_str(), "Confirm", MB_YESNO) == IDYES) {
                        g_macros.erase(name);
                        SaveMacros();
                        ListView_DeleteItem(hLV, sel);
                        int count = ListView_GetItemCount(hLV);
                        for (int i = 0; i < count; ++i) {
                            char idx[8]; sprintf_s(idx, "%d", i+1);
                            ListView_SetItemText(hLV, i, 0, idx);
                        }
                    }
                    break;
                }
                case IDOK: {
                    char buf[64];
                    GetDlgItemTextA(hwnd, 110, buf, sizeof(buf)); double newSens = atof(buf);
                    GetDlgItemTextA(hwnd, 111, buf, sizeof(buf)); int newFps = atoi(buf);
                    bool newStartMin = (IsDlgButtonChecked(hwnd, 501) == BST_CHECKED);
                    bool newHoldMode = (IsDlgButtonChecked(hwnd, 112) == BST_CHECKED);
                    bool newGlitchEnabled = (IsDlgButtonChecked(hwnd, 115) == BST_CHECKED);
                    bool newSitEnabled = (IsDlgButtonChecked(hwnd, 205) == BST_CHECKED);
                    int newSitSlot = (int)SendDlgItemMessageA(hwnd, 204, CB_GETCURSEL, 0, 0);
                    if (newSitSlot < 0) newSitSlot = 0;
                    bool newSuperJumpEnabled = (IsDlgButtonChecked(hwnd, 306) == BST_CHECKED);
                    bool newEquipEnabled = (IsDlgButtonChecked(hwnd, 405) == BST_CHECKED);
                    bool newLagEnabled = (IsDlgButtonChecked(hwnd, 415) == BST_CHECKED);
                    bool newSpamEnabled = (IsDlgButtonChecked(hwnd, 517) == BST_CHECKED);
                    bool newSpamHoldMode = (IsDlgButtonChecked(hwnd, 515) == BST_CHECKED);
                    int newSpamSlot = (int)SendDlgItemMessageA(hwnd, 516, CB_GETCURSEL, 0, 0) + 1;
                    if (newSpamSlot < 0) newSpamSlot = 0;

                    // Guard against two enabled macros sharing the same trigger key —
                    // only the first match ever fires, which makes the second one look broken.
                    struct KeyOwner { UINT key; bool enabled; const char* label; };
                    KeyOwner owners[] = {
                        { g_config.glitchKey,     newGlitchEnabled,    "Speed Glitch" },
                        { g_config.sitKey,        newSitEnabled,       "Sit Macro" },
                        { g_config.superJumpKey,  newSuperJumpEnabled, "Pool Super Jump" },
                        { g_config.lagKey,        newLagEnabled,       "Lag Switch" },
                        { g_config.spamKey,       newSpamEnabled,      "Spam Sign" },
                        { g_config.stopAllKey,    g_config.stopAllKey != 0, "Stop All" },
                    };
                    for (int i = 0; i < 6; ++i) {
                        if (!owners[i].enabled) continue;
                        for (int j = i + 1; j < 6; ++j) {
                            if (owners[j].enabled && owners[i].key == owners[j].key) {
                                char msg[256];
                                sprintf_s(msg, "'%s' and '%s' are both bound to the same key (%s).\n"
                                               "Only one of them will ever fire. Please give them different keys.",
                                          owners[i].label, owners[j].label, KeyName(owners[i].key).c_str());
                                MessageBoxA(hwnd, msg, "Duplicate Trigger Key", MB_OK | MB_ICONWARNING);
                                return 0;
                            }
                        }
                    }
                    for (int i = 1; i < 6; ++i) {
                        if (!owners[i].enabled || owners[i].key == 0) continue;
                        for (auto& pair : g_macros) {
                            if (!newEquipEnabled) break;
                            if ((UINT)pair.second.hotkey == owners[i].key) {
                                char msg[256];
                                sprintf_s(msg, "Macro '%s' shares its key (%s) with %s.",
                                          pair.first.c_str(), KeyName(owners[i].key).c_str(), owners[i].label);
                                MessageBoxA(hwnd, msg, "Duplicate Trigger Key", MB_OK | MB_ICONWARNING);
                                return 0;
                            }
                        }
                    }

                    g_config.sensitivity = newSens;
                    g_config.fps = newFps;
                    g_config.startMinimized = newStartMin;
                    g_config.holdMode = newHoldMode;
                    g_config.glitchEnabled = newGlitchEnabled;
                    g_config.sitEnabled = newSitEnabled;
                    g_config.sitSlot = newSitSlot;
                    g_config.superJumpEnabled = newSuperJumpEnabled;
                    g_config.equipEnabled = newEquipEnabled;
                    g_config.lagEnabled = newLagEnabled;
                    g_config.spamEnabled = newSpamEnabled;
                    g_config.spamHoldMode = newSpamHoldMode;
                    g_config.spamSlot = newSpamSlot;
                    SaveConfig();
                    DestroyWindow(hwnd);
                    g_hwndSettings = nullptr;
                    break;
                }
                case IDCANCEL:
                    DestroyWindow(hwnd);
                    g_hwndSettings = nullptr;
                    break;
            }
            break;
        }
        case WM_SYSCOMMAND:
            // Minimize button → hide to tray instead of minimizing
            if ((wParam & 0xFFF0) == SC_MINIMIZE) {
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            }
            return DefWindowProc(hwnd, msg, wParam, lParam);

        case WM_CLOSE:
            // X button → completely exit the macro
            g_exitRequested = true;
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ==============================
//  Macro Editor
// ==============================
LRESULT CALLBACK MacroEditorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCHITTEST:
            return HeaderHitTest(hwnd, lParam);
        case WM_LBUTTONDOWN:
            if (HandleHeaderDotClick(hwnd, lParam)) return 0;
            break;
        case WM_CREATE: {
            EnableDarkTitleBar(hwnd);
            EnableRoundedCorners(hwnd);
            CreateWindowExA(0, "STATIC", "Name:", WS_CHILD | WS_VISIBLE, 16, 68, 50, 20, hwnd, NULL, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER, 70, 65, 254, 24, hwnd, (HMENU)201, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "STATIC", "Hotkey:", WS_CHILD | WS_VISIBLE, 16, 102, 50, 20, hwnd, NULL, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER, 70, 99, 90, 24, hwnd, (HMENU)202, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "BUTTON", "Rec", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 170, 99, 68, 24, hwnd, (HMENU)203, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "STATIC", "Menus:", WS_CHILD | WS_VISIBLE, 16, 136, 50, 20, hwnd, NULL, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER, 70, 133, 60, 24, hwnd, (HMENU)204, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "STATIC", "Items (Menu:Slot):", WS_CHILD | WS_VISIBLE, 16, 168, 150, 20, hwnd, NULL, GetModuleHandle(NULL), NULL);
            HWND hLV = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEW, "", WS_CHILD | WS_VISIBLE | LVS_REPORT,
                                       16, 190, 308, 108, hwnd, (HMENU)205, GetModuleHandle(NULL), NULL);
            ListView_SetExtendedListViewStyle(hLV, LVS_EX_FULLROWSELECT);
            LVCOLUMNA col = {}; col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.pszText = (LPSTR)"#"; col.cx = 40; ListView_InsertColumn(hLV, 0, &col);
            col.pszText = (LPSTR)"Menu"; col.cx = 130; ListView_InsertColumn(hLV, 1, &col);
            col.pszText = (LPSTR)"Slot"; col.cx = 130; ListView_InsertColumn(hLV, 2, &col);
            CreateWindowExA(0, "STATIC", "Menu:", WS_CHILD | WS_VISIBLE, 16, 308, 40, 20, hwnd, NULL, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER, 58, 305, 50, 24, hwnd, (HMENU)206, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "STATIC", "Slot:", WS_CHILD | WS_VISIBLE, 118, 308, 40, 20, hwnd, NULL, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER, 158, 305, 50, 24, hwnd, (HMENU)207, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "BUTTON", "Add", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 220, 305, 48, 24, hwnd, (HMENU)208, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "BUTTON", "Del", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 276, 305, 48, 24, hwnd, (HMENU)209, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "BUTTON", "Save", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 70, 348, 100, 32, hwnd, (HMENU)IDOK, GetModuleHandle(NULL), NULL);
            CreateWindowExA(0, "BUTTON", "Cancel", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 180, 348, 100, 32, hwnd, (HMENU)IDCANCEL, GetModuleHandle(NULL), NULL);
            ApplyThemeToChildren(hwnd);
            SendMessageA(hwnd, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            if (!g_editingNew) {
                SetDlgItemTextA(hwnd, 201, g_editingMacro.name.c_str());
                SetDlgItemTextA(hwnd, 202, KeyName(g_editingMacro.hotkey).c_str());
                char buf[16]; sprintf_s(buf, "%d", g_editingMacro.menus);
                SetDlgItemTextA(hwnd, 204, buf);
                HWND hLV2 = GetDlgItem(hwnd, 205);
                for (size_t i = 0; i < g_editingMacro.items.size(); ++i) {
                    char idx[8], menu[8], slot[8];
                    sprintf_s(idx, "%d", (int)i+1);
                    sprintf_s(menu, "%d", g_editingMacro.items[i].menu);
                    sprintf_s(slot, "%d", g_editingMacro.items[i].slot);
                    LVITEMA lv = {}; lv.mask = LVIF_TEXT; lv.iItem = (int)i; lv.iSubItem = 0; lv.pszText = idx;
                    int item = ListView_InsertItem(hLV2, &lv);
                    ListView_SetItemText(hLV2, item, 1, menu);
                    ListView_SetItemText(hLV2, item, 2, slot);
                }
            }
            break;
        }
        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case 203: {
                    if (!g_recording) {
                        g_recording = true;
                        SetDlgItemTextA(hwnd, 203, "...");
                        EnableWindow(GetDlgItem(hwnd, 203), FALSE);
                        SetTimer(hwnd, 2, 50, [](HWND h, UINT, UINT_PTR, DWORD) {
                            if (!g_recording) return;
                            for (int vk=1; vk<=255; ++vk) {
                                if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON ||
                                    vk == VK_XBUTTON1 || vk == VK_XBUTTON2) continue;
                                if (GetAsyncKeyState(vk) & 0x8000) {
                                    g_recording = false; KillTimer(h, 2);
                                    g_editingMacro.hotkey = (UINT)vk;
                                    SetDlgItemTextA(h, 202, KeyName(vk).c_str());
                                    EnableWindow(GetDlgItem(h, 203), TRUE);
                                    SetDlgItemTextA(h, 203, "Rec");
                                    return;
                                }
                            }
                            UINT mouseKeys[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
                            const char* names[] = {"LButton","RButton","MButton","XButton1","XButton2"};
                            for (int i=0; i<5; ++i) {
                                if (GetAsyncKeyState(mouseKeys[i]) & 0x8000) {
                                    g_recording = false; KillTimer(h, 2);
                                    g_editingMacro.hotkey = mouseKeys[i];
                                    SetDlgItemTextA(h, 202, names[i]);
                                    EnableWindow(GetDlgItem(h, 203), TRUE);
                                    SetDlgItemTextA(h, 203, "Rec");
                                    return;
                                }
                            }
                        });
                    }
                    break;
                }
                case 208: {
                    char menuBuf[8], slotBuf[8];
                    GetDlgItemTextA(hwnd, 206, menuBuf, sizeof(menuBuf));
                    GetDlgItemTextA(hwnd, 207, slotBuf, sizeof(slotBuf));
                    int menu = atoi(menuBuf), slot = atoi(slotBuf);
                    if (menu < 1 || slot < 1) { MessageBoxA(hwnd, "Menu and Slot must be >=1", "Error", MB_OK); break; }
                    g_editingMacro.items.push_back({menu, slot});
                    HWND hLV = GetDlgItem(hwnd, 205);
                    int count = (int)g_editingMacro.items.size();
                    char idx[8], menuS[8], slotS[8];
                    sprintf_s(idx, "%d", count);
                    sprintf_s(menuS, "%d", menu);
                    sprintf_s(slotS, "%d", slot);
                    LVITEMA lv = {}; lv.mask = LVIF_TEXT; lv.iItem = count-1; lv.iSubItem = 0; lv.pszText = idx;
                    int item = ListView_InsertItem(hLV, &lv);
                    ListView_SetItemText(hLV, item, 1, menuS);
                    ListView_SetItemText(hLV, item, 2, slotS);
                    SetDlgItemTextA(hwnd, 206, "");
                    SetDlgItemTextA(hwnd, 207, "");
                    break;
                }
                case 209: {
                    if (g_editingMacro.items.empty()) break;
                    g_editingMacro.items.pop_back();
                    HWND hLV = GetDlgItem(hwnd, 205);
                    int count = ListView_GetItemCount(hLV);
                    if (count > 0) ListView_DeleteItem(hLV, count-1);
                    break;
                }
                case IDOK: {
                    char nameBuf[128];
                    GetDlgItemTextA(hwnd, 201, nameBuf, sizeof(nameBuf));
                    if (strlen(nameBuf) == 0) { MessageBoxA(hwnd, "Enter a name", "Error", MB_OK); break; }
                    if (g_editingMacro.hotkey == 0) { MessageBoxA(hwnd, "Record a hotkey", "Error", MB_OK); break; }
                    char menusBuf[8];
                    GetDlgItemTextA(hwnd, 204, menusBuf, sizeof(menusBuf));
                    g_editingMacro.menus = std::max(1, atoi(menusBuf));
                    if (g_editingMacro.items.empty()) { MessageBoxA(hwnd, "Add at least one item", "Error", MB_OK); break; }

                    // Capture the ORIGINAL name before overwriting it, so renames
                    // erase the correct old entry instead of a no-op erase of the new name.
                    std::string oldName = g_editingMacro.name;
                    g_editingMacro.name = nameBuf;
                    if (!g_editingNew && oldName != nameBuf)
                        g_macros.erase(oldName);
                    g_macros[nameBuf] = g_editingMacro;
                    SaveMacros();
                    DestroyWindow(hwnd);
                    g_hwndMacroEditor = nullptr;

                    // Refresh the macro list back on the Settings window (control 401) —
                    // it was only ever populated once at creation, so without this,
                    // newly added/edited macros never appeared even though they DID save.
                    if (g_hwndSettings) {
                        HWND hLV2 = GetDlgItem(g_hwndSettings, 401);
                        if (hLV2) {
                            ListView_DeleteAllItems(hLV2);
                            int idx2 = 0;
                            for (auto& pair : g_macros) {
                                const Macro& m = pair.second;
                                char itemsCount[8]; sprintf_s(itemsCount, "%d", (int)m.items.size());
                                LVITEMA lv2 = {}; lv2.mask = LVIF_TEXT; lv2.iItem = idx2; lv2.iSubItem = 0;
                                lv2.pszText = (LPSTR)m.name.c_str();
                                int item2 = ListView_InsertItem(hLV2, &lv2);
                                ListView_SetItemText(hLV2, item2, 1, (LPSTR)KeyName(m.hotkey).c_str());
                                ListView_SetItemText(hLV2, item2, 2, itemsCount);
                                idx2++;
                            }
                        }
                    }
                    break;
                }
                case IDCANCEL:
                    DestroyWindow(hwnd);
                    g_hwndMacroEditor = nullptr;
                    break;
            }
            break;
        }
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            DrawThemedHeader(hwnd, dc, "Edit Macro");
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wParam;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, COL_TEXT);
            return (LRESULT)g_hbrBg;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC dc = (HDC)wParam;
            SetTextColor(dc, COL_TEXT);
            SetBkColor(dc, COL_EDIT_BG);
            SetBkMode(dc, OPAQUE);
            return (LRESULT)g_hbrEdit;
        }
        case WM_CTLCOLORBTN: {
            HDC dc = (HDC)wParam;
            SetTextColor(dc, COL_TEXT);
            SetBkMode(dc, TRANSPARENT);
            return (LRESULT)g_hbrBg;
        }
        case WM_DRAWITEM: {
            LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lParam;
            if (dis->CtlType == ODT_BUTTON) { DrawThemedPushButton(dis); return TRUE; }
            break;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            g_hwndMacroEditor = nullptr;
            break;
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ==============================
//  Main window
// ==============================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_TRAYICON: {
            if (lParam == NIN_BALLOONUSERCLICK) {
                // Update toast clicked -> open the release page.
                ShellExecuteA(NULL, "open",
                              "https://github.com/orbitthegreatest/Orbit-MM2-Macro/releases/latest",
                              NULL, NULL, SW_SHOWNORMAL);
            }
            if (lParam == WM_LBUTTONDBLCLK) {
                // Double-click tray icon → restore settings window
                if (!g_hwndSettings) {
                    g_hwndSettings = CreateWindowExA(0, "SettingsClass", "Orbit MM2 Settings",
                                                     WS_POPUP | WS_BORDER | WS_SYSMENU | WS_MINIMIZEBOX,
                                                     CW_USEDEFAULT, CW_USEDEFAULT, 740, 470,
                                                     hwnd, NULL, GetModuleHandle(NULL), NULL);
                }
                ShowWindow(g_hwndSettings, SW_SHOW);
                SetForegroundWindow(g_hwndSettings);
            }
            if (lParam == WM_RBUTTONUP) {
                POINT pt;
                GetCursorPos(&pt);
                HMENU hMenu = CreatePopupMenu();
                AppendMenuA(hMenu, MF_STRING, ID_TRAY_SETTINGS, "Settings");
                AppendMenuA(hMenu, MF_STRING, ID_TRAY_EXIT, "Exit");
                SetForegroundWindow(hwnd);
                TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(hMenu);
                PostMessage(hwnd, WM_NULL, 0, 0);
            }
            break;
        }
        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case ID_TRAY_SETTINGS: {
                    if (!g_hwndSettings) {
g_hwndSettings = CreateWindowExA(0, "SettingsClass", "Orbit MM2 Settings",
                                                     WS_POPUP | WS_BORDER | WS_SYSMENU | WS_MINIMIZEBOX,
                                                     CW_USEDEFAULT, CW_USEDEFAULT, 740, 470,
                                                     hwnd, NULL, GetModuleHandle(NULL), NULL);
                    }
                    ShowWindow(g_hwndSettings, SW_SHOW);
                    SetForegroundWindow(g_hwndSettings);
                    break;
                }
                case ID_TRAY_EXIT:
                    g_exitRequested = true;
                    PostQuitMessage(0);
                    break;
            }
            break;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            break;
        case WM_TIMER:
            if (wParam == HINT_TIMER_ID) HideHint();
            break;
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ==============================
//  Main entry
// ==============================
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // Single-instance guard (matches the original AHK script's #SingleInstance Force):
    // only one copy of the macro should ever be hooking input at once. If another
    // instance is already running, wake it up (reopen its Settings window) and quit.
    g_singleInstanceMutex = CreateMutexA(NULL, TRUE, "Local\\OrbitMM2Macro_SingleInstanceMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND hExisting = FindWindowA("OrbitMM2Class", "Orbit MM2 Macro");
        if (hExisting) {
            PostMessage(hExisting, WM_COMMAND, ID_TRAY_SETTINGS, 0);
            SetForegroundWindow(hExisting);
        }
        if (g_singleInstanceMutex) CloseHandle(g_singleInstanceMutex);
        return 0;
    }

    // Matches the original AHK script's ProcessSetPriority("High") + timeBeginPeriod(1).
    // Without this, Windows' default ~15ms Sleep() granularity turns the burst loops'
    // 4ms steps into much coarser, choppier ~15ms steps, making Super Jump's camera
    // spin (and Speed Glitch's jitter) feel nothing like the original macro.
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    timeBeginPeriod(1);

    InitConfigPath();
    LoadConfig();
    LoadMacros();
    InitThemeResources();

    // Register the common controls classes (tooltips) before creating windows.
    INITCOMMONCONTROLSEX icc = { sizeof(INITCOMMONCONTROLSEX), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);

    // Load icon from resource (used for the tray icon, all window title bars, and the header banner)
    HICON hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(1));
    if (!hIcon) {
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        char* p = strrchr(exePath, '\\');
        if (p) {
            strcpy_s(p+1, MAX_PATH - (p+1 - exePath), "mm2_macro_logo.ico");
        }
        hIcon = (HICON)LoadImageA(NULL, exePath, IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
        if (!hIcon) hIcon = LoadIcon(NULL, IDI_APPLICATION);
    }
    g_hAppIcon = hIcon;

    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "OrbitMM2Class";
    wc.hIcon = g_hAppIcon;
    wc.hbrBackground = g_hbrBg;
    RegisterClassA(&wc);

    WNDCLASSA wcSettings = {};
    wcSettings.lpfnWndProc = SettingsWndProc;
    wcSettings.hInstance = hInstance;
    wcSettings.lpszClassName = "SettingsClass";
    wcSettings.hIcon = g_hAppIcon;
    wcSettings.hbrBackground = g_hbrBg;
    wcSettings.style = CS_DROPSHADOW;
    RegisterClassA(&wcSettings);

    WNDCLASSA wcMacroEdit = {};
    wcMacroEdit.lpfnWndProc = MacroEditorWndProc;
    wcMacroEdit.hInstance = hInstance;
    wcMacroEdit.lpszClassName = "MacroEditorClass";
    wcMacroEdit.hIcon = g_hAppIcon;
    wcMacroEdit.hbrBackground = g_hbrBg;
    wcMacroEdit.style = CS_DROPSHADOW;
    RegisterClassA(&wcMacroEdit);

    WNDCLASSA wcHint = {};
    wcHint.lpfnWndProc = HintWndProc;
    wcHint.hInstance = hInstance;
    wcHint.lpszClassName = "HintClass";
    wcHint.hbrBackground = g_hbrBg;
    RegisterClassA(&wcHint);

    g_hwndMain = CreateWindowExA(0, "OrbitMM2Class", "Orbit MM2 Macro",
                                 WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                 400, 200, NULL, NULL, hInstance, NULL);
    if (!g_hwndMain) return 1;
    ShowWindow(g_hwndMain, SW_HIDE);

    // Press feedback toast popup (shown next to the cursor when the global
    // stop/start key is pressed).
    g_hintPopup = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                  "HintClass", NULL, WS_POPUP, 0, 0, 200, 60,
                                  NULL, NULL, hInstance, NULL);

    g_keyboardHook = SetWindowsHookExA(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInstance, 0);
    g_mouseHook = SetWindowsHookExA(WH_MOUSE_LL, LowLevelMouseProc, hInstance, 0);

    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = g_hwndMain;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = hIcon;
    wcscpy_s(g_nid.szTip, L"Orbit MM2 Macro");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    // Background update detector: polls the GitHub releases API and notifies
    // when a release newer than the current build is published.
    std::thread(UpdateCheckWorker).detach();

    if (!g_config.startMinimized)
        PostMessage(g_hwndMain, WM_COMMAND, ID_TRAY_SETTINGS, 0);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_keyboardHook) UnhookWindowsHookEx(g_keyboardHook);
    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_hintPopup) {
        KillTimer(g_hwndMain, HINT_TIMER_ID);
        DestroyWindow(g_hintPopup);
        g_hintPopup = nullptr;
    }
    timeEndPeriod(1);
    if (g_singleInstanceMutex) {
        ReleaseMutex(g_singleInstanceMutex);
        CloseHandle(g_singleInstanceMutex);
    }
    return 0;
}