// Phantom Fence - keeps windows and the mouse cursor off phantom displays.
// Copyright (C) 2026 Solemn Scribe
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the
// Free Software Foundation, either version 3 of the License, or (at your
// option) any later version.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
// Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program. If not, see <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: GPL-3.0-or-later

// PhantomFence — keep windows and the mouse cursor off phantom displays.
//
// Purpose: some AV receivers / HDMI audio devices register a display that
// doesn't physically exist. Disabling it in Windows kills the audio endpoint,
// but leaving it enabled lets windows get orphaned on it and the cursor get
// lost. PhantomFence lets you "fence" such displays: any window that appears
// there is moved back to a real monitor, and the mouse cannot cross onto it.
//
// Single-file Win32 app, no runtime dependencies.
// Build (MinGW-w64):
//   x86_64-w64-mingw32-g++ -O2 -municode -mwindows -static PhantomFence.cpp ...
//       -o PhantomFence.exe -lgdi32 -ldwmapi -lshell32 -luser32 -ladvapi32
//
// Config lives in HKCU\Software\PhantomFence. Fenced displays are stored by
// monitor device-interface ID so the fence survives display renumbering.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <wchar.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dwmapi.lib")

// ---------------------------------------------------------------- constants

static const wchar_t* APP_NAME      = L"PhantomFence";
static const wchar_t* WND_CLASS     = L"PhantomFenceHiddenWnd";
static const wchar_t* MUTEX_NAME    = L"Local\\PhantomFenceSingleton";
static const wchar_t* REG_KEY       = L"Software\\PhantomFence";
static const wchar_t* REG_FENCED    = L"FencedDisplays";   // REG_MULTI_SZ of device IDs
static const wchar_t* REG_MOUSE     = L"MouseFence";       // DWORD
static const wchar_t* REG_RESCUE    = L"WindowRescue";     // DWORD
static const wchar_t* REG_HOTKEY    = L"HotkeySkip";       // DWORD
static const wchar_t* REG_TBGUARD   = L"TaskbarAutoHideGuard"; // DWORD, default off
static const wchar_t* RUN_KEY       = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

enum {
    WMAPP_TRAY         = WM_APP + 1,
    WMAPP_HOTMOVE      = WM_APP + 2,  // wParam: 1=right 0=left, lParam: HWND
    WMAPP_FENCECONFIRM = WM_APP + 3,  // wParam: 1=keep 0=revert
    IDT_SWEEP         = 1,

    IDC_KEEP          = 1001,         // confirmation window buttons
    IDC_UNDO          = 1002,

    IDM_EXIT          = 1,
    IDM_AUTOSTART     = 2,
    IDM_RESCUE_NOW    = 3,
    IDM_TOGGLE_MOUSE  = 4,
    IDM_TOGGLE_RESCUE = 5,
    IDM_HEADER        = 6,
    IDM_TOGGLE_HOTKEY = 7,
    IDM_TOGGLE_TBGUARD = 8,
    IDM_DISPLAY_BASE  = 100,
};

static const UINT SWEEP_INTERVAL_MS = 2000;

// ---------------------------------------------------------------- state

struct MonitorEntry {
    HMONITOR     hmon;
    RECT         rc;          // full monitor rect (physical px)
    RECT         rcWork;      // work area
    std::wstring gdiName;     // \\.\DISPLAY1
    std::wstring deviceId;    // stable device-interface ID (or gdiName fallback)
    std::wstring friendly;    // "DENON-AVR" etc., may be empty
    bool         fenced;
    bool         primary;
};

static HINSTANCE                 g_hInst;
static HWND                      g_hwnd;
static NOTIFYICONDATAW           g_nid = {};
static HICON                     g_icon = nullptr;
static UINT                      g_msgTaskbarCreated;
static HHOOK                     g_mouseHook = nullptr;
static HHOOK                     g_kbHook = nullptr;
static HWINEVENTHOOK             g_evHookShow = nullptr;
static HWINEVENTHOOK             g_evHookMove = nullptr;
static HWINEVENTHOOK             g_evHookFg   = nullptr;

static std::vector<MonitorEntry> g_monitors;          // current topology
static std::vector<std::wstring> g_fencedIds;         // persisted fence list
static std::vector<RECT>         g_fencedRects;       // cache for the mouse hook
static bool                      g_mouseFenceOn  = true;
static bool                      g_windowRescueOn = true;
static bool                      g_hotkeySkipOn  = true;
static bool                      g_tbGuardOn     = false;  // keep taskbar auto-hide on
static bool                      g_isPackaged    = false;  // running from an MSIX package
static POINT                     g_lastGoodPt = { 0, 0 };
static bool                      g_haveLastGood = false;
static bool                      g_swallowUp[2] = { false, false }; // [0]=left [1]=right

// Fence-enable confirmation state: a newly fenced display takes effect
// immediately but is only persisted once the countdown dialog is confirmed.
static std::wstring              g_pendingFenceId;
static HWND                      g_confirmWnd = nullptr;
static HWND                      g_confirmText = nullptr;
static int                       g_confirmSecs = 0;

static void TrayBalloon(const wchar_t* title, const wchar_t* text); // fwd

// ---------------------------------------------------------------- registry

static HKEY OpenAppKey(bool write) {
    HKEY hk = nullptr;
    if (write) {
        RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, nullptr, 0,
                        KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &hk, nullptr);
    } else {
        RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_QUERY_VALUE, &hk);
    }
    return hk;
}

static DWORD RegReadDword(const wchar_t* name, DWORD def) {
    HKEY hk = OpenAppKey(false);
    if (!hk) return def;
    DWORD val = def, cb = sizeof(val), type = 0;
    if (RegQueryValueExW(hk, name, nullptr, &type, (BYTE*)&val, &cb) != ERROR_SUCCESS
        || type != REG_DWORD) val = def;
    RegCloseKey(hk);
    return val;
}

static void RegWriteDword(const wchar_t* name, DWORD val) {
    HKEY hk = OpenAppKey(true);
    if (!hk) return;
    RegSetValueExW(hk, name, 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
    RegCloseKey(hk);
}

static std::vector<std::wstring> RegReadMultiSz(const wchar_t* name) {
    std::vector<std::wstring> out;
    HKEY hk = OpenAppKey(false);
    if (!hk) return out;
    DWORD cb = 0, type = 0;
    if (RegQueryValueExW(hk, name, nullptr, &type, nullptr, &cb) == ERROR_SUCCESS
        && type == REG_MULTI_SZ && cb >= sizeof(wchar_t)) {
        std::vector<wchar_t> buf(cb / sizeof(wchar_t) + 2, L'\0');
        if (RegQueryValueExW(hk, name, nullptr, &type, (BYTE*)buf.data(), &cb) == ERROR_SUCCESS) {
            const wchar_t* p = buf.data();
            while (*p) {
                out.push_back(p);
                p += wcslen(p) + 1;
            }
        }
    }
    RegCloseKey(hk);
    return out;
}

static void RegWriteMultiSz(const wchar_t* name, const std::vector<std::wstring>& items) {
    std::vector<wchar_t> blob;
    for (const auto& s : items) {
        blob.insert(blob.end(), s.begin(), s.end());
        blob.push_back(L'\0');
    }
    blob.push_back(L'\0');            // double-null terminator
    HKEY hk = OpenAppKey(true);
    if (!hk) return;
    RegSetValueExW(hk, name, 0, REG_MULTI_SZ,
                   (const BYTE*)blob.data(), (DWORD)(blob.size() * sizeof(wchar_t)));
    RegCloseKey(hk);
}

// When installed from the Microsoft Store (MSIX), HKCU\...\Run writes are
// virtualized into the package hive and never read at login. Autostart is
// declared in the package manifest instead (windows.startupTask), and users
// manage it in Settings > Apps > Startup. Detect packaged mode so the tray
// menu reflects reality.
#ifndef APPMODEL_ERROR_NO_PACKAGE
#define APPMODEL_ERROR_NO_PACKAGE 15700L
#endif

static bool DetectPackaged() {
    typedef LONG (WINAPI *GetPkgFn)(UINT32*, wchar_t*);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) return false;
    auto fn = (GetPkgFn)(void*)GetProcAddress(k32, "GetCurrentPackageFullName");
    if (!fn) return false;
    UINT32 len = 0;
    return fn(&len, nullptr) != APPMODEL_ERROR_NO_PACKAGE;
}

static bool AutostartEnabled() {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS)
        return false;
    DWORD cb = 0, type = 0;
    bool on = RegQueryValueExW(hk, APP_NAME, nullptr, &type, nullptr, &cb) == ERROR_SUCCESS
              && type == REG_SZ;
    RegCloseKey(hk);
    return on;
}

static void SetAutostart(bool enable) {
    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &hk, nullptr) != ERROR_SUCCESS)
        return;
    if (enable) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring quoted = L"\"" + std::wstring(path) + L"\"";
        RegSetValueExW(hk, APP_NAME, 0, REG_SZ,
                       (const BYTE*)quoted.c_str(),
                       (DWORD)((quoted.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(hk, APP_NAME);
    }
    RegCloseKey(hk);
}

// ------------------------------------------------------- monitor enumeration

static std::wstring FriendlyNameForGdiDevice(const wchar_t* gdiDevice) {
    UINT32 nPath = 0, nMode = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPath, &nMode) != ERROR_SUCCESS)
        return L"";
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPath);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nMode);
    if (nPath == 0 ||
        QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPath, paths.data(),
                           &nMode, modes.data(), nullptr) != ERROR_SUCCESS)
        return L"";
    for (UINT32 i = 0; i < nPath; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
        src.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size      = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id        = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        if (_wcsicmp(src.viewGdiDeviceName, gdiDevice) != 0) continue;

        DISPLAYCONFIG_TARGET_DEVICE_NAME tgt = {};
        tgt.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tgt.header.size      = sizeof(tgt);
        tgt.header.adapterId = paths[i].targetInfo.adapterId;
        tgt.header.id        = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tgt.header) != ERROR_SUCCESS) continue;
        return tgt.monitorFriendlyDeviceName;
    }
    return L"";
}

static BOOL CALLBACK MonEnumProc(HMONITOR hmon, HDC, LPRECT, LPARAM lp) {
    auto* list = (std::vector<MonitorEntry>*)lp;
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hmon, &mi)) return TRUE;

    MonitorEntry e;
    e.hmon    = hmon;
    e.rc      = mi.rcMonitor;
    e.rcWork  = mi.rcWork;
    e.gdiName = mi.szDevice;
    e.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    e.fenced  = false;

    DISPLAY_DEVICEW dd = {};
    dd.cb = sizeof(dd);
    if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, EDD_GET_DEVICE_INTERFACE_NAME)
        && dd.DeviceID[0]) {
        e.deviceId = dd.DeviceID;
    } else {
        e.deviceId = e.gdiName;   // fallback: positional name
    }
    e.friendly = FriendlyNameForGdiDevice(mi.szDevice);
    list->push_back(e);
    return TRUE;
}

static void RefreshMonitors() {
    g_monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, MonEnumProc, (LPARAM)&g_monitors);

    // Stable ordering: by GDI name (DISPLAY1, DISPLAY2, ...)
    std::sort(g_monitors.begin(), g_monitors.end(),
              [](const MonitorEntry& a, const MonitorEntry& b) {
                  return a.gdiName < b.gdiName;
              });

    // The primary display can never be fenced: the taskbar and this app's
    // tray icon live there, and fencing it would lock the user out of the
    // very menu that could undo it. If a fenced display has BECOME the
    // primary (the user changed it in Settings), unfence it for good.
    bool removedPrimary = false;
    for (const auto& m : g_monitors) {
        if (!m.primary) continue;
        auto it = std::find(g_fencedIds.begin(), g_fencedIds.end(), m.deviceId);
        if (it != g_fencedIds.end()) {
            g_fencedIds.erase(it);
            removedPrimary = true;
        }
        if (!g_pendingFenceId.empty() && g_pendingFenceId == m.deviceId)
            g_pendingFenceId.clear();     // a pending fence is voided too
    }
    if (removedPrimary) {
        RegWriteMultiSz(REG_FENCED, g_fencedIds);
        TrayBalloon(APP_NAME,
                    L"A fenced display became the primary display and was unfenced.");
    }

    // Mark fenced from the persisted list plus any pending (unconfirmed) fence.
    for (auto& m : g_monitors) {
        m.fenced = !m.primary &&
                   (std::find(g_fencedIds.begin(), g_fencedIds.end(), m.deviceId)
                        != g_fencedIds.end()
                    || (!g_pendingFenceId.empty() && m.deviceId == g_pendingFenceId));
    }

    // Rebuild the rect cache the mouse hook reads.
    g_fencedRects.clear();
    for (const auto& m : g_monitors)
        if (m.fenced) g_fencedRects.push_back(m.rc);
}

static bool IsMonitorFenced(HMONITOR hmon) {
    for (const auto& m : g_monitors)
        if (m.hmon == hmon) return m.fenced;
    return false;
}

static const MonitorEntry* NearestRealMonitor(const RECT& from) {
    const MonitorEntry* best = nullptr;
    long long bestDist = -1;
    long fx = (from.left + from.right) / 2;
    long fy = (from.top + from.bottom) / 2;
    for (const auto& m : g_monitors) {
        if (m.fenced) continue;
        long mx = (m.rc.left + m.rc.right) / 2;
        long my = (m.rc.top + m.rc.bottom) / 2;
        long long dx = mx - fx, dy = my - fy;
        long long d = dx * dx + dy * dy;
        if (!best || d < bestDist ||
            (d == bestDist && m.primary)) {
            best = &m;
            bestDist = d;
        }
    }
    if (!best) {                       // shouldn't happen (safety valve above)
        for (const auto& m : g_monitors)
            if (m.primary) return &m;
        return g_monitors.empty() ? nullptr : &g_monitors[0];
    }
    return best;
}

// Monitors in left-to-right (then top-to-bottom) order — the order Windows
// effectively cycles through for Win+Shift+Left/Right.
static std::vector<size_t> MonitorOrderByX() {
    std::vector<size_t> idx(g_monitors.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [](size_t a, size_t b) {
        const RECT& ra = g_monitors[a].rc;
        const RECT& rb = g_monitors[b].rc;
        if (ra.left != rb.left) return ra.left < rb.left;
        return ra.top < rb.top;
    });
    return idx;
}

static int OrderPos(const std::vector<size_t>& order, HMONITOR hmon) {
    for (size_t i = 0; i < order.size(); ++i)
        if (g_monitors[order[i]].hmon == hmon) return (int)i;
    return -1;
}

// Next non-fenced monitor in the cyclic direction, skipping fenced ones.
static const MonitorEntry* NextRealInDirection(HMONITOR cur, bool right) {
    std::vector<size_t> order = MonitorOrderByX();
    int n = (int)order.size();
    int pos = OrderPos(order, cur);
    if (pos < 0 || n < 2) return nullptr;
    for (int step = 1; step < n; ++step) {
        int i = right ? (pos + step) % n : (pos - step + n * 2) % n;
        const MonitorEntry& m = g_monitors[order[i]];
        if (!m.fenced) return &m;
    }
    return nullptr;
}

// ------------------------------------------------------------ window rescue

static bool IsWindowCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED,
                                        &cloaked, sizeof(cloaked))))
        return cloaked != 0;
    return false;
}

static bool WindowBeingDragged(HWND hwnd) {
    // Don't yank a window out from under an active user drag/resize.
    DWORD tid = GetWindowThreadProcessId(hwnd, nullptr);
    GUITHREADINFO gti = {};
    gti.cbSize = sizeof(gti);
    if (tid && GetGUIThreadInfo(tid, &gti))
        return (gti.flags & GUI_INMOVESIZE) != 0;
    return false;
}

static bool ShouldSkipWindow(HWND hwnd) {
    if (hwnd == g_hwnd)                    return true;
    if (!IsWindowVisible(hwnd))            return true;
    if (IsIconic(hwnd))                    return true;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return true;
    if (IsWindowCloaked(hwnd))             return true;
    if (WindowBeingDragged(hwnd))          return true;

    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 63);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") ||
        !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd") ||
        !wcscmp(cls, L"NotifyIconOverflowWindow"))
        return true;

    RECT rc;
    if (!GetWindowRect(hwnd, &rc)) return true;
    if (rc.right - rc.left < 8 || rc.bottom - rc.top < 8) return true;
    return false;
}

// Map a window rect from one monitor's work area to another's. A window that
// was "fitted" to the source display (fills >=60% of it in either axis — the
// usual result of landing on a phantom display) is rescaled per-axis so it
// occupies the same fraction of the destination, converting portrait-fitted
// windows into landscape-fitted ones. Small dialogs keep their size. Position
// maps proportionally by center; the result is clamped into the work area.
static void RemapRect(const RECT& srcWork, const RECT& dstWork, RECT& rc) {
    const double sw = (double)(srcWork.right - srcWork.left);
    const double sh = (double)(srcWork.bottom - srcWork.top);
    const double dw = (double)(dstWork.right - dstWork.left);
    const double dh = (double)(dstWork.bottom - dstWork.top);
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;

    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    bool fitted = (w >= 0.6 * sw) || (h >= 0.6 * sh);
    int nw = fitted ? (int)(w * dw / sw + 0.5) : w;
    int nh = fitted ? (int)(h * dh / sh + 0.5) : h;
    if (nw > (int)dw) nw = (int)dw;
    if (nh > (int)dh) nh = (int)dh;

    double cx = ((rc.left + rc.right) / 2.0 - srcWork.left) / sw;
    double cy = ((rc.top + rc.bottom) / 2.0 - srcWork.top)  / sh;
    int nx = dstWork.left + (int)(cx * dw + 0.5) - nw / 2;
    int ny = dstWork.top  + (int)(cy * dh + 0.5) - nh / 2;
    if (nx + nw > dstWork.right)  nx = dstWork.right - nw;
    if (ny + nh > dstWork.bottom) ny = dstWork.bottom - nh;
    if (nx < dstWork.left) nx = dstWork.left;
    if (ny < dstWork.top)  ny = dstWork.top;

    rc.left = nx;
    rc.top = ny;
    rc.right = nx + nw;
    rc.bottom = ny + nh;
}

// ---- snap detection ---------------------------------------------------
// Windows Snap has no public "is this window snapped" API, but snapped
// windows occupy exact grid fractions (halves, thirds, quarters) of the work
// area, measured on the VISIBLE bounds — DWMWA_EXTENDED_FRAME_BOUNDS.
// (GetWindowRect includes the invisible resize borders, which is why a
// proportional remap of a snapped window leaves small gaps.) Detect the
// fractions on the source display and re-apply them exactly on the
// destination, so a snapped window arrives re-snapped.

static bool VisibleBounds(HWND hwnd, RECT& vis, RECT& margins) {
    RECT wr;
    if (!GetWindowRect(hwnd, &wr)) return false;
    RECT fb = wr;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                     &fb, sizeof(fb))))
        fb = wr;
    vis = fb;
    margins.left   = fb.left - wr.left;
    margins.top    = fb.top  - wr.top;
    margins.right  = wr.right  - fb.right;
    margins.bottom = wr.bottom - fb.bottom;
    LONG* m[4] = { &margins.left, &margins.top, &margins.right, &margins.bottom };
    for (int i = 0; i < 4; ++i) {           // sanity: frame insets are small
        if (*m[i] < 0)  *m[i] = 0;
        if (*m[i] > 64) *m[i] = 64;
    }
    return true;
}

// Is `frac` within 16 px (over a span of `spanPx`) of a snap-grid anchor?
static bool SnapAnchor(double frac, double spanPx, double* out) {
    static const double anchors[] = { 0.0, 1.0 / 3.0, 0.5, 2.0 / 3.0, 1.0 };
    for (double a : anchors) {
        double distPx = (frac - a) * spanPx;
        if (distPx <= 16.0 && distPx >= -16.0) { *out = a; return true; }
    }
    return false;
}

static bool TrySnapPlace(HWND hwnd, const RECT& srcWork, const RECT& dstWork) {
    RECT vis, m;
    if (!VisibleBounds(hwnd, vis, m)) return false;
    const double sw = (double)(srcWork.right - srcWork.left);
    const double sh = (double)(srcWork.bottom - srcWork.top);
    if (sw <= 0 || sh <= 0) return false;

    double fx0, fx1, fy0, fy1;
    if (!SnapAnchor((vis.left   - srcWork.left) / sw, sw, &fx0)) return false;
    if (!SnapAnchor((vis.right  - srcWork.left) / sw, sw, &fx1)) return false;
    if (!SnapAnchor((vis.top    - srcWork.top)  / sh, sh, &fy0)) return false;
    if (!SnapAnchor((vis.bottom - srcWork.top)  / sh, sh, &fy1)) return false;
    if (fx1 <= fx0 || fy1 <= fy0) return false;
    if (fx0 == 0.0 && fx1 == 1.0 && fy0 == 0.0 && fy1 == 1.0)
        return false;   // fills the work area: generic path handles that fine

    const double dw = (double)(dstWork.right - dstWork.left);
    const double dh = (double)(dstWork.bottom - dstWork.top);
    int vl = dstWork.left + (int)(fx0 * dw + 0.5);
    int vr = dstWork.left + (int)(fx1 * dw + 0.5);
    int vt = dstWork.top  + (int)(fy0 * dh + 0.5);
    int vb = dstWork.top  + (int)(fy1 * dh + 0.5);

    SetWindowPos(hwnd, nullptr,
                 vl - m.left, vt - m.top,
                 (vr - vl) + m.left + m.right,
                 (vb - vt) + m.top + m.bottom,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    return true;
}

static void PlaceWindowOnMonitor(HWND hwnd, const RECT& srcWork,
                                 const MonitorEntry* dst) {
    if (!dst) return;

    WINDOWPLACEMENT wp = {};
    wp.length = sizeof(wp);
    GetWindowPlacement(hwnd, &wp);

    if (wp.showCmd == SW_SHOWMAXIMIZED) {
        // Re-anchor the restored rect on the target monitor, then re-maximize
        // there. Brief flicker, but reliable across app frameworks.
        RECT rc;
        GetWindowRect(hwnd, &rc);
        RECT nr = rc;
        RemapRect(srcWork, dst->rcWork, nr);
        wp.rcNormalPosition = nr;
        wp.showCmd = SW_RESTORE;
        SetWindowPlacement(hwnd, &wp);
        ShowWindow(hwnd, SW_MAXIMIZE);
        return;
    }

    if (TrySnapPlace(hwnd, srcWork, dst->rcWork))
        return;                             // re-snapped exactly

    RECT rc;
    GetWindowRect(hwnd, &rc);
    RECT nr = rc;
    RemapRect(srcWork, dst->rcWork, nr);
    SetWindowPos(hwnd, nullptr, nr.left, nr.top,
                 nr.right - nr.left, nr.bottom - nr.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

static void RescueWindow(HWND hwnd) {
    if (ShouldSkipWindow(hwnd)) return;

    HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!IsMonitorFenced(hmon)) return;

    RECT rc;
    GetWindowRect(hwnd, &rc);
    const MonitorEntry* dst = NearestRealMonitor(rc);
    if (!dst) return;

    RECT srcWork = rc;                 // fallback if entry lookup fails
    for (const auto& m : g_monitors)
        if (m.hmon == hmon) { srcWork = m.rcWork; break; }

    PlaceWindowOnMonitor(hwnd, srcWork, dst);
}

static BOOL CALLBACK SweepEnumProc(HWND hwnd, LPARAM) {
    RescueWindow(hwnd);
    return TRUE;
}

static void RescueCursor() {
    POINT pt;
    if (!GetCursorPos(&pt)) return;
    HMONITOR hmon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (!IsMonitorFenced(hmon)) return;
    RECT r = { pt.x, pt.y, pt.x + 1, pt.y + 1 };
    const MonitorEntry* dst = NearestRealMonitor(r);
    if (dst) {
        SetCursorPos((dst->rc.left + dst->rc.right) / 2,
                     (dst->rc.top + dst->rc.bottom) / 2);
    }
}

// --------------------------------------------------- taskbar auto-hide guard
//
// Display topology changes are notorious for silently turning off
// "Automatically hide the taskbar". When the guard is enabled, re-assert the
// auto-hide bit whenever it is found off. ABM_SETSTATE replaces the whole
// appbar state, so OR the bit into the current state instead of clobbering
// other bits (e.g. the legacy always-on-top bit).

static void GuardTaskbarAutoHide() {
    if (!g_tbGuardOn) return;
    APPBARDATA abd = {};
    abd.cbSize = sizeof(abd);
    UINT_PTR state = SHAppBarMessage(ABM_GETSTATE, &abd);
    if (!(state & ABS_AUTOHIDE)) {
        abd.lParam = (LPARAM)(state | ABS_AUTOHIDE);
        SHAppBarMessage(ABM_SETSTATE, &abd);
    }
}

static void SweepAll() {
    GuardTaskbarAutoHide();
    if (g_fencedRects.empty()) return;
    if (g_windowRescueOn)
        EnumWindows(SweepEnumProc, 0);
    RescueCursor();
}

// -------------------------------------------------------------- mouse fence

static bool PtInFence(POINT pt) {
    for (const auto& r : g_fencedRects) {
        if (pt.x >= r.left && pt.x < r.right &&
            pt.y >= r.top  && pt.y < r.bottom)
            return true;
    }
    return false;
}

static bool PtOnRealMonitor(POINT pt) {
    HMONITOR hmon = MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
    if (!hmon) return false;
    return !IsMonitorFenced(hmon);
}

// Deflect a point that landed in a fenced rect to the nearest legal point,
// preferring a slide along the fence edge so diagonal motion still works.
static POINT DeflectPoint(POINT pt) {
    const RECT* r = nullptr;
    for (const auto& fr : g_fencedRects) {
        if (pt.x >= fr.left && pt.x < fr.right &&
            pt.y >= fr.top  && pt.y < fr.bottom) { r = &fr; break; }
    }
    if (!r) return pt;

    POINT cands[4] = {
        { r->left - 1, pt.y },   // pushed out left
        { r->right,    pt.y },   // pushed out right
        { pt.x, r->top - 1 },    // pushed out above
        { pt.x, r->bottom },     // pushed out below
    };
    const POINT* best = nullptr;
    long long bestDist = -1;
    for (const auto& c : cands) {
        if (!PtOnRealMonitor(c)) continue;
        long long dx = c.x - pt.x, dy = c.y - pt.y;
        long long d = dx * dx + dy * dy;
        if (!best || d < bestDist) { best = &c; bestDist = d; }
    }
    if (best) return *best;
    if (g_haveLastGood) return g_lastGoodPt;

    // Last resort: center of nearest real monitor.
    RECT tiny = { pt.x, pt.y, pt.x + 1, pt.y + 1 };
    const MonitorEntry* dst = NearestRealMonitor(tiny);
    if (dst) {
        POINT c = { (dst->rc.left + dst->rc.right) / 2,
                    (dst->rc.top + dst->rc.bottom) / 2 };
        return c;
    }
    return pt;
}

static LRESULT CALLBACK MouseHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_mouseFenceOn && !g_fencedRects.empty()) {
        const MSLLHOOKSTRUCT* ms = (const MSLLHOOKSTRUCT*)lParam;
        // Let injected events (incl. our own SetCursorPos) pass untouched.
        if (!(ms->flags & LLMHF_INJECTED)) {
            POINT pt = ms->pt;
            if (PtInFence(pt)) {
                POINT safe = DeflectPoint(pt);
                if (safe.x != pt.x || safe.y != pt.y)
                    SetCursorPos(safe.x, safe.y);
                return 1;   // swallow the move onto the fenced display
            }
            if (wParam == WM_MOUSEMOVE) {
                g_lastGoodPt  = pt;
                g_haveLastGood = true;
            }
        }
    }
    return CallNextHookEx(g_mouseHook, code, wParam, lParam);
}

// ------------------------------------------------------------ keyboard hook
//
// Win+Shift+Left/Right normally cycles the foreground window through ALL
// displays, phantom included — and Windows' choice of "next monitor" follows
// an undocumented ordering that cannot be reliably predicted across layouts
// (vertical stacks still cycle on Left/Right, so it is not purely spatial).
// So while fencing is active we take the shortcut over entirely: swallow the
// keystroke and move the foreground window ourselves, deterministically, to
// the next non-fenced display in left-to-right order (wrapping, skipping
// fenced ones). The window never touches the phantom display.
// The actual move is posted to the main window: SetWindowPos can block on the
// target app's message pump, which must never happen inside a LL hook.

static LRESULT CALLBACK KbHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_hotkeySkipOn && !g_fencedRects.empty()) {
        const KBDLLHOOKSTRUCT* kb = (const KBDLLHOOKSTRUCT*)lParam;
        if (!(kb->flags & LLKHF_INJECTED) &&
            (kb->vkCode == VK_LEFT || kb->vkCode == VK_RIGHT)) {
            const int dir = (kb->vkCode == VK_RIGHT) ? 1 : 0;
            const bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
            const bool isUp   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);

            if (isUp && g_swallowUp[dir]) {   // eat the keyup of a swallowed press
                g_swallowUp[dir] = false;
                return 1;
            }
            if (isDown) {
                const bool win   = (GetAsyncKeyState(VK_LWIN) & 0x8000) ||
                                   (GetAsyncKeyState(VK_RWIN) & 0x8000);
                const bool shift = (GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0;
                const bool ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                const bool alt   = (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;
                if (win && shift && !ctrl && !alt) {
                    HWND fg = GetForegroundWindow();
                    if (fg && !ShouldSkipWindow(fg)) {
                        // Always take over: letting Windows act could land the
                        // window on a fenced display. HotkeyMove no-ops if
                        // there is no valid destination.
                        PostMessageW(g_hwnd, WMAPP_HOTMOVE,
                                     (WPARAM)dir, (LPARAM)fg);
                        g_swallowUp[dir] = true;
                        return 1;             // Windows never sees this press
                    }
                }
            }
        }
    }
    return CallNextHookEx(g_kbHook, code, wParam, lParam);
}

static void HotkeyMove(HWND hwnd, bool right) {
    if (!IsWindow(hwnd) || ShouldSkipWindow(hwnd)) return;
    HMONITOR cur = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    const MonitorEntry* dst = NextRealInDirection(cur, right);
    if (!dst || dst->hmon == cur) return;
    RECT srcWork = {};
    bool found = false;
    for (const auto& m : g_monitors)
        if (m.hmon == cur) { srcWork = m.rcWork; found = true; break; }
    if (!found) return;
    PlaceWindowOnMonitor(hwnd, srcWork, dst);
}

// -------------------------------------------------------------- event hooks

static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD /*event*/, HWND hwnd,
                                  LONG idObject, LONG idChild, DWORD, DWORD) {
    if (!g_windowRescueOn || g_fencedRects.empty()) return;
    if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    RescueWindow(hwnd);
}

static void InstallHooks() {
    if (!g_mouseHook)
        g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, g_hInst, 0);
    if (!g_kbHook)
        g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, KbHookProc, g_hInst, 0);
    if (!g_evHookShow)
        g_evHookShow = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW,
                                       nullptr, WinEventProc, 0, 0,
                                       WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!g_evHookMove)
        g_evHookMove = SetWinEventHook(EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND,
                                       nullptr, WinEventProc, 0, 0,
                                       WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!g_evHookFg)
        g_evHookFg = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                     nullptr, WinEventProc, 0, 0,
                                     WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
}

static void RemoveHooks() {
    if (g_mouseHook)  { UnhookWindowsHookEx(g_mouseHook);  g_mouseHook  = nullptr; }
    if (g_kbHook)     { UnhookWindowsHookEx(g_kbHook);     g_kbHook     = nullptr; }
    if (g_evHookShow) { UnhookWinEvent(g_evHookShow);       g_evHookShow = nullptr; }
    if (g_evHookMove) { UnhookWinEvent(g_evHookMove);       g_evHookMove = nullptr; }
    if (g_evHookFg)   { UnhookWinEvent(g_evHookFg);         g_evHookFg   = nullptr; }
}

// ---------------------------------------------------------------- tray icon

static HICON CreateFenceIcon() {
    const int S = 32;
    HDC screen = GetDC(nullptr);
    HDC cdc = CreateCompatibleDC(screen);
    HDC mdc = CreateCompatibleDC(screen);
    HBITMAP color = CreateCompatibleBitmap(screen, S, S);
    HBITMAP mask  = CreateBitmap(S, S, 1, 1, nullptr);
    ReleaseDC(nullptr, screen);

    HGDIOBJ oc = SelectObject(cdc, color);
    HGDIOBJ om = SelectObject(mdc, mask);

    // Mask: white = transparent, black = opaque.
    RECT all = { 0, 0, S, S };
    FillRect(mdc, &all, (HBRUSH)GetStockObject(WHITE_BRUSH));
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    HGDIOBJ oldMB = SelectObject(mdc, black);
    SelectObject(mdc, GetStockObject(NULL_PEN));
    RoundRect(mdc, 2, 4, 30, 24, 6, 6);          // monitor body
    RECT stand = { 12, 24, 20, 28 };             // stand
    FillRect(mdc, &stand, black);
    RECT base = { 8, 27, 24, 30 };               // base
    FillRect(mdc, &base, black);
    SelectObject(mdc, oldMB);

    // Color: draw on black; masked area shows through.
    FillRect(cdc, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
    HBRUSH body   = CreateSolidBrush(RGB(70, 90, 120));
    HPEN   edge   = CreatePen(PS_SOLID, 2, RGB(180, 200, 225));
    HGDIOBJ ob = SelectObject(cdc, body);
    HGDIOBJ op = SelectObject(cdc, edge);
    RoundRect(cdc, 2, 4, 30, 24, 6, 6);
    HBRUSH gray = CreateSolidBrush(RGB(140, 150, 165));
    FillRect(cdc, &stand, gray);
    FillRect(cdc, &base, gray);
    // Red slash.
    HPEN slash = CreatePen(PS_SOLID, 4, RGB(225, 60, 60));
    SelectObject(cdc, slash);
    MoveToEx(cdc, 5, 26, nullptr);
    LineTo(cdc, 27, 2);
    SelectObject(cdc, op);
    SelectObject(cdc, ob);
    DeleteObject(slash);
    DeleteObject(gray);
    DeleteObject(edge);
    DeleteObject(body);

    SelectObject(cdc, oc);
    SelectObject(mdc, om);
    DeleteDC(cdc);
    DeleteDC(mdc);

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

static void TrayAdd() {
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WMAPP_TRAY;
    g_nid.hIcon = g_icon;
    wcscpy_s(g_nid.szTip, ARRAYSIZE(g_nid.szTip), APP_NAME);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

static void TrayBalloon(const wchar_t* title, const wchar_t* text) {
    NOTIFYICONDATAW nid = g_nid;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    wcscpy_s(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), title);
    wcscpy_s(nid.szInfo, ARRAYSIZE(nid.szInfo), text);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void TrayRemove() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

// ------------------------------------------------ fence confirmation window
//
// Fencing takes effect immediately but is only PERSISTED after the user
// confirms this countdown dialog — the same pattern as Windows' own "Keep
// these display settings?" prompt. The dialog is placed on the nearest
// NON-fenced display, which is the failsafe: if the user just fenced the
// only display they can actually see (e.g. the phantom is set as primary),
// the dialog lands on an invisible screen, cannot be clicked, and the fence
// reverts automatically. A panic reboot mid-countdown also comes back
// unfenced, because nothing was written to the registry yet.

static const int      CONFIRM_SECONDS = 15;
static const wchar_t* CONFIRM_CLASS   = L"PhantomFenceConfirmWnd";

static void ConfirmSetText() {
    if (!g_confirmText) return;
    wchar_t buf[256];
    swprintf(buf, 256,
             L"Phantom Fence: the display is now fenced.\r\n\r\n"
             L"If you can see this and everything looks right, click Keep.\r\n"
             L"Reverting automatically in %d second%s.",
             g_confirmSecs, g_confirmSecs == 1 ? L"" : L"s");
    SetWindowTextW(g_confirmText, buf);
}

static LRESULT CALLBACK ConfirmWndProc(HWND hwnd, UINT msg,
                                       WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TIMER:
        if (--g_confirmSecs <= 0) {
            PostMessageW(g_hwnd, WMAPP_FENCECONFIRM, 0, 0);
            DestroyWindow(hwnd);
        } else {
            ConfirmSetText();
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_KEEP) {
            PostMessageW(g_hwnd, WMAPP_FENCECONFIRM, 1, 0);
            DestroyWindow(hwnd);
        } else if (LOWORD(wParam) == IDC_UNDO) {
            PostMessageW(g_hwnd, WMAPP_FENCECONFIRM, 0, 0);
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:                       // Alt+F4 etc. = revert
        PostMessageW(g_hwnd, WMAPP_FENCECONFIRM, 0, 0);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        g_confirmWnd = nullptr;
        g_confirmText = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowFenceConfirm(const RECT& fencedRc) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc   = ConfirmWndProc;
        wc.hInstance     = g_hInst;
        wc.lpszClassName = CONFIRM_CLASS;
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }

    const MonitorEntry* dst = NearestRealMonitor(fencedRc);
    RECT wa = dst ? dst->rcWork : RECT{ 0, 0, 800, 600 };
    const int w = 480, h = 190;
    int x = wa.left + ((wa.right - wa.left) - w) / 2;
    int y = wa.top  + ((wa.bottom - wa.top) - h) / 2;

    g_confirmSecs = CONFIRM_SECONDS;
    g_confirmWnd = CreateWindowExW(WS_EX_TOPMOST, CONFIRM_CLASS,
                                   L"Phantom Fence", WS_POPUP | WS_BORDER,
                                   x, y, w, h, g_hwnd, nullptr, g_hInst, nullptr);
    if (!g_confirmWnd) {                 // fail safe: no dialog = no fence
        PostMessageW(g_hwnd, WMAPP_FENCECONFIRM, 0, 0);
        return;
    }

    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    g_confirmText = CreateWindowExW(0, L"STATIC", L"",
                                    WS_CHILD | WS_VISIBLE,
                                    16, 14, w - 32, 100,
                                    g_confirmWnd, nullptr, g_hInst, nullptr);
    SendMessageW(g_confirmText, WM_SETFONT, (WPARAM)font, TRUE);

    HWND keep = CreateWindowExW(0, L"BUTTON", L"Keep fenced",
                                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                w - 268, h - 52, 120, 32,
                                g_confirmWnd, (HMENU)(INT_PTR)IDC_KEEP,
                                g_hInst, nullptr);
    SendMessageW(keep, WM_SETFONT, (WPARAM)font, TRUE);

    HWND undo = CreateWindowExW(0, L"BUTTON", L"Undo now",
                                WS_CHILD | WS_VISIBLE,
                                w - 136, h - 52, 120, 32,
                                g_confirmWnd, (HMENU)(INT_PTR)IDC_UNDO,
                                g_hInst, nullptr);
    SendMessageW(undo, WM_SETFONT, (WPARAM)font, TRUE);

    ConfirmSetText();
    ShowWindow(g_confirmWnd, SW_SHOW);
    SetForegroundWindow(g_confirmWnd);
    SetFocus(keep);
    SetTimer(g_confirmWnd, 1, 1000, nullptr);
}

// --------------------------------------------------------------- tray menu

static std::wstring MonitorMenuLabel(const MonitorEntry& m, size_t index) {
    // "Display 1  —  1080×1920  —  DENON-AVR"
    std::wstring num;
    size_t pos = m.gdiName.find(L"DISPLAY");
    if (pos != std::wstring::npos) num = m.gdiName.substr(pos + 7);
    else num = std::to_wstring(index + 1);

    wchar_t res[64];
    swprintf(res, 64, L"%ld\u00D7%ld", m.rc.right - m.rc.left, m.rc.bottom - m.rc.top);

    std::wstring label = L"Display " + num + L"   " + res;
    if (!m.friendly.empty()) label += L"   " + m.friendly;
    if (m.primary) label += L"  (primary - can't be fenced)";
    return label;
}

static void ShowTrayMenu() {
    RefreshMonitors();   // pick up any topology change before showing

    HMENU menu = CreatePopupMenu();

    AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, IDM_HEADER,
                L"Fence a display to keep windows && mouse off it:");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    for (size_t i = 0; i < g_monitors.size(); ++i) {
        UINT flags = MF_STRING;
        if (g_monitors[i].fenced)  flags |= MF_CHECKED;
        if (g_monitors[i].primary) flags |= MF_DISABLED | MF_GRAYED;
        AppendMenuW(menu, flags, IDM_DISPLAY_BASE + (UINT)i,
                    MonitorMenuLabel(g_monitors[i], i).c_str());
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_RESCUE_NOW, L"Rescue stray windows now");
    AppendMenuW(menu, MF_STRING | (g_windowRescueOn ? MF_CHECKED : 0),
                IDM_TOGGLE_RESCUE, L"Auto-rescue windows");
    AppendMenuW(menu, MF_STRING | (g_mouseFenceOn ? MF_CHECKED : 0),
                IDM_TOGGLE_MOUSE, L"Block mouse on fenced displays");
    AppendMenuW(menu, MF_STRING | (g_hotkeySkipOn ? MF_CHECKED : 0),
                IDM_TOGGLE_HOTKEY, L"Win+Shift+Arrow skips fenced displays");
    AppendMenuW(menu, MF_STRING | (g_tbGuardOn ? MF_CHECKED : 0),
                IDM_TOGGLE_TBGUARD, L"Keep taskbar auto-hide enabled");
    if (g_isPackaged) {
        AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, IDM_AUTOSTART,
                    L"Startup: see Settings > Apps > Startup");
    } else {
        AppendMenuW(menu, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0),
                    IDM_AUTOSTART, L"Start with Windows");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);   // required so the menu dismisses properly
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                   pt.x, pt.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

static void ToggleDisplayFence(size_t index) {
    if (index >= g_monitors.size()) return;
    MonitorEntry& m = g_monitors[index];

    if (!m.fenced) {
        if (m.primary) {          // belt & braces: the item is greyed anyway
            TrayBalloon(APP_NAME,
                        L"The primary display can't be fenced - the taskbar "
                        L"and this tray icon live there.");
            return;
        }
        if (!g_pendingFenceId.empty()) {
            TrayBalloon(APP_NAME,
                        L"Confirm or undo the pending fence first.");
            return;
        }
        // Apply immediately; persist only after the countdown is confirmed.
        g_pendingFenceId = m.deviceId;
        RECT rc = m.rc;
        RefreshMonitors();
        SweepAll();               // show the effect right away
        ShowFenceConfirm(rc);
    } else {
        if (!g_pendingFenceId.empty() && m.deviceId == g_pendingFenceId) {
            // Unchecking the pending fence = undo the countdown.
            if (g_confirmWnd) DestroyWindow(g_confirmWnd);
            g_pendingFenceId.clear();
            RefreshMonitors();
            TrayBalloon(APP_NAME, L"Fence undone.");
            return;
        }
        g_fencedIds.erase(
            std::remove(g_fencedIds.begin(), g_fencedIds.end(), m.deviceId),
            g_fencedIds.end());
        RegWriteMultiSz(REG_FENCED, g_fencedIds);
        RefreshMonitors();
        SweepAll();
    }
}

// ------------------------------------------------------------- window proc

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_msgTaskbarCreated) {   // explorer restarted
        TrayAdd();
        return 0;
    }
    switch (msg) {
    case WMAPP_TRAY:
        switch (LOWORD(lParam)) {
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
        case NIN_KEYSELECT:
            ShowTrayMenu();
            return 0;
        case WM_LBUTTONDBLCLK:
            SweepAll();
            TrayBalloon(APP_NAME, L"Rescued stray windows.");
            return 0;
        }
        return 0;

    case WM_COMMAND: {
        UINT id = LOWORD(wParam);
        if (id >= IDM_DISPLAY_BASE && id < IDM_DISPLAY_BASE + 64) {
            ToggleDisplayFence(id - IDM_DISPLAY_BASE);
        } else if (id == IDM_EXIT) {
            DestroyWindow(hwnd);
        } else if (id == IDM_AUTOSTART) {
            if (!g_isPackaged) SetAutostart(!AutostartEnabled());
        } else if (id == IDM_RESCUE_NOW) {
            SweepAll();
        } else if (id == IDM_TOGGLE_MOUSE) {
            g_mouseFenceOn = !g_mouseFenceOn;
            RegWriteDword(REG_MOUSE, g_mouseFenceOn ? 1 : 0);
        } else if (id == IDM_TOGGLE_RESCUE) {
            g_windowRescueOn = !g_windowRescueOn;
            RegWriteDword(REG_RESCUE, g_windowRescueOn ? 1 : 0);
            if (g_windowRescueOn) SweepAll();
        } else if (id == IDM_TOGGLE_HOTKEY) {
            g_hotkeySkipOn = !g_hotkeySkipOn;
            RegWriteDword(REG_HOTKEY, g_hotkeySkipOn ? 1 : 0);
        } else if (id == IDM_TOGGLE_TBGUARD) {
            g_tbGuardOn = !g_tbGuardOn;
            RegWriteDword(REG_TBGUARD, g_tbGuardOn ? 1 : 0);
            GuardTaskbarAutoHide();       // enforce immediately on enable
        }
        return 0;
    }

    case WMAPP_HOTMOVE:
        HotkeyMove((HWND)lParam, wParam != 0);
        return 0;

    case WMAPP_FENCECONFIRM:
        if (!g_pendingFenceId.empty()) {
            if (wParam == 1) {
                g_fencedIds.push_back(g_pendingFenceId);
                RegWriteMultiSz(REG_FENCED, g_fencedIds);
                g_pendingFenceId.clear();
                TrayBalloon(APP_NAME, L"Display fenced.");
            } else {
                g_pendingFenceId.clear();
                TrayBalloon(APP_NAME, L"Fence reverted.");
            }
            RefreshMonitors();
            SweepAll();
        }
        return 0;

    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        RefreshMonitors();
        SweepAll();
        return 0;

    case WM_TIMER:
        if (wParam == IDT_SWEEP) SweepAll();
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, IDT_SWEEP);
        RemoveHooks();
        TrayRemove();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// -------------------------------------------------------------------- main

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    g_hInst = hInst;

    // Refuse a second instance.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, MUTEX_NAME);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;

    // Per-Monitor V2 DPI awareness so all coordinates are physical pixels.
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        typedef BOOL (WINAPI *SetCtxFn)(DPI_AWARENESS_CONTEXT);
        auto setCtx = (SetCtxFn)(void*)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (setCtx) setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }

    g_isPackaged     = DetectPackaged();
    g_mouseFenceOn   = RegReadDword(REG_MOUSE, 1) != 0;
    g_windowRescueOn = RegReadDword(REG_RESCUE, 1) != 0;
    g_hotkeySkipOn   = RegReadDword(REG_HOTKEY, 1) != 0;
    g_tbGuardOn      = RegReadDword(REG_TBGUARD, 0) != 0;   // opt-in
    g_fencedIds      = RegReadMultiSz(REG_FENCED);
    HKEY hkProbe     = OpenAppKey(false);
    bool firstRun    = (hkProbe == nullptr);
    if (hkProbe) RegCloseKey(hkProbe);

    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = WND_CLASS;
    RegisterClassW(&wc);

    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    // Hidden but real (not message-only) window: broadcasts like
    // WM_DISPLAYCHANGE and TaskbarCreated don't reach message-only windows.
    g_hwnd = CreateWindowExW(0, WND_CLASS, APP_NAME, WS_OVERLAPPED,
                             0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;

    g_icon = CreateFenceIcon();
    TrayAdd();
    RefreshMonitors();
    InstallHooks();
    SetTimer(g_hwnd, IDT_SWEEP, SWEEP_INTERVAL_MS, nullptr);
    SweepAll();

    if (firstRun || g_fencedIds.empty()) {
        TrayBalloon(APP_NAME,
                    L"Right-click the tray icon and pick the display(s) to fence.");
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_icon) DestroyIcon(g_icon);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}
