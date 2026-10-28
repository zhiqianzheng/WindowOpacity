#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <algorithm>
#include <string>
#include <vector>
#include <deque>
#include "clear_region.h"

enum { WindowList = 101, Refresh, Pick, Slider, Percent, Apply, Restore, RestoreAll, Status };
struct SavedWindow {
    HWND hwnd;
    DWORD pid, tid;
    bool layered;
    COLORREF color;
    BYTE alpha;
    DWORD flags;
};
static HWND mainWindow, windowList, slider, percentEdit, statusText, pickButton;
static HFONT font, headingFont;
static HWND target;
static std::vector<SavedWindow> saved;
static std::vector<SavedWindow> temporaryOpacity;
static int countdown;
static double scale = 1;
static const wchar_t* propertyName = L"WindowOpacity.Owner.8CF3470F";
static HANDLE ownerToken;
enum { TrayCallback = WM_APP + 1, WheelMessage = WM_APP + 2, PanelCommand = 201, PauseCommand, ExitCommand, ShortcutCommand,
    RegionSelectCommand, RegionClearCommand, RegionClearAllCommand };
static constexpr int RegionSelectHotkey = 6901, RegionClearHotkey = 6902;
enum { ShortcutEnabled = 301, ShortcutShift, ShortcutCtrl, ShortcutAlt, ShortcutWin, ShortcutStep, ShortcutSave, ShortcutDefaults, ShortcutStatus, StartAtLogon };
struct ShortcutOptions { bool enabled = true; int modifiers = 1; int step = 5; };
struct WheelRequest { HWND hwnd; DWORD pid, tid; int delta; POINT point; ULONGLONG time; bool toggle = false; };
static ShortcutOptions shortcut;
static HWND settingsWindow;
static HHOOK mouseHook;
static bool suppressRightUp;
static std::deque<WheelRequest> wheelRequests;
static std::wstring settingsPath;
static ULONGLONG wheelFeedbackUntil;
static HWND wheelPrevious;
static DWORD wheelPid, wheelTid;
static ULONGLONG wheelTime;
static int wheelRemainder;
static constexpr const wchar_t* runKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static constexpr const wchar_t* runValue = L"WindowOpacity";
static ULONGLONG trayRetryAt;
static HWND popup, popupTarget, hoverTarget;
static DWORD popupPid, popupTid;
static HFONT popupFont, popupTitleFont, popupSmallFont;
static double popupScale = 1;
static bool dragging, hoverEnabled = true, trayAdded;
static bool popupInteractive = true;
static int popupValue = 100, anchorOffset;
static POINT hoverPoint;
static ULONGLONG hoverSince, lastInside;
static std::wstring popupHint, lastStatus;
static UINT taskbarCreated;
static constexpr UINT_PTR HoverTimer = 2;
static int P(int value) { return static_cast<int>(value * popupScale + .5); }
static void HidePopup();
static void HoverTick();
static void ShowPanel();
static void ExitApplication();
static bool AddTray();
static void ShowShortcutSettings();
static void PopupApplyAt(int x);
static void BeginRegionSelection(HWND source);
static HWND PointedSource();

static int S(int value) { return static_cast<int>(value * scale + .5); }
static void StatusMessage(const std::wstring& message) {
    lastStatus = message;
    SetWindowTextW(statusText, message.c_str());
}
static bool IdentityMatches(const SavedWindow& item) {
    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(item.hwnd, &pid);
    return IsWindow(item.hwnd) && pid == item.pid && tid == item.tid &&
        GetPropW(item.hwnd, propertyName) == ownerToken;
}
static void CancelTemporary(HWND hwnd) {
    temporaryOpacity.erase(std::remove_if(temporaryOpacity.begin(), temporaryOpacity.end(), [hwnd](const SavedWindow& item) {
        return item.hwnd == hwnd;
    }), temporaryOpacity.end());
}
static void Prune() {
    saved.erase(std::remove_if(saved.begin(), saved.end(), [](const SavedWindow& item) {
        return !IdentityMatches(item);
    }), saved.end());
    temporaryOpacity.erase(std::remove_if(temporaryOpacity.begin(), temporaryOpacity.end(), [](const SavedWindow& item) {
        return !IdentityMatches(item);
    }), temporaryOpacity.end());
}
static void ErrorMessage(const wchar_t* action, DWORD error) {
    std::wstring message = action;
    message += L"失败（错误 " + std::to_wstring(error) + L"）。";
    message += error == ERROR_ACCESS_DENIED ?
        L"请以管理员身份运行本工具后重试。" : L"目标可能已关闭或不支持此操作。";
    StatusMessage(message);
}
static bool SetStyle(HWND hwnd, LONG_PTR style) {
    SetLastError(0);
    LONG_PTR previous = SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style);
    return previous != 0 || GetLastError() == 0;
}
static bool RestoreAppearance(const SavedWindow& item) {
    if (!IdentityMatches(item)) return true;
    if (item.layered) {
        if (!SetLayeredWindowAttributes(item.hwnd, item.color, item.alpha, item.flags)) return false;
    } else {
        LONG_PTR style = GetWindowLongPtrW(item.hwnd, GWL_EXSTYLE);
        if (!SetStyle(item.hwnd, style & ~static_cast<LONG_PTR>(WS_EX_LAYERED))) return false;
        RedrawWindow(item.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    }
    return true;
}
static bool RestoreWindow(const SavedWindow& item) {
    if (!RestoreAppearance(item)) return false;
    if (IdentityMatches(item)) RemovePropW(item.hwnd, propertyName);
    CancelTemporary(item.hwnd);
    return true;
}
static void ShowPercent(int value) {
    SendMessageW(slider, TBM_SETPOS, TRUE, value);
    SetWindowTextW(percentEdit, std::to_wstring(value).c_str());
}
static bool Eligible(HWND hwnd) {
    if (!hwnd || hwnd == mainWindow || !IsWindowVisible(hwnd) ||
        hwnd == GetShellWindow() || hwnd == GetDesktopWindow() || !GetWindowTextLengthW(hwnd)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return false;
    DWORD cloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return !cloaked;
}
static void SelectWindow() {
    LRESULT index = SendMessageW(windowList, CB_GETCURSEL, 0, 0);
    target = index == CB_ERR ? nullptr : reinterpret_cast<HWND>(SendMessageW(windowList, CB_GETITEMDATA, index, 0));
    int value = 100;
    if (IsWindow(target) && (GetWindowLongPtrW(target, GWL_EXSTYLE) & WS_EX_LAYERED)) {
        BYTE alpha = 255;
        DWORD flags = 0;
        COLORREF color = 0;
        if (GetLayeredWindowAttributes(target, &color, &alpha, &flags) && (flags & LWA_ALPHA))
            value = std::max(1, static_cast<int>((alpha * 100 + 127) / 255));
    }
    ShowPercent(value);
    StatusMessage(target ? L"已选择窗口。拖动滑块即刻生效；100% 表示完全不透明。" : L"未找到可调整的窗口，请打开一个应用后刷新。 ");
}
static BOOL CALLBACK EnumCallback(HWND hwnd, LPARAM) {
    if (!Eligible(hwnd)) return TRUE;
    wchar_t title[512] = {};
    GetWindowTextW(hwnd, title, 512);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    std::wstring label = std::wstring(title) + L"  [" + std::to_wstring(pid) + L"]";
    LRESULT index = SendMessageW(windowList, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    if (index >= 0) SendMessageW(windowList, CB_SETITEMDATA, index, reinterpret_cast<LPARAM>(hwnd));
    return TRUE;
}
static void RefreshWindows(HWND preferred = nullptr) {
    Prune();
    HWND previous = preferred ? preferred : target;
    SendMessageW(windowList, CB_RESETCONTENT, 0, 0);
    EnumWindows(EnumCallback, 0);
    int count = static_cast<int>(SendMessageW(windowList, CB_GETCOUNT, 0, 0));
    int selected = count ? 0 : -1;
    for (int i = 0; i < count; ++i)
        if (reinterpret_cast<HWND>(SendMessageW(windowList, CB_GETITEMDATA, i, 0)) == previous) selected = i;
    SendMessageW(windowList, CB_SETCURSEL, selected, 0);
    SelectWindow();
}
static bool ApplyOpacity(int value, HWND hwnd = nullptr) {
    if (!hwnd) hwnd = target;
    if (value < 1 || value > 100 || !Eligible(hwnd)) {
        StatusMessage(L"目标窗口已关闭或不可用，请重新选择窗口。");
        return false;
    }
    Prune();
    auto existing = std::find_if(saved.begin(), saved.end(), [hwnd](const SavedWindow& item) { return item.hwnd == hwnd; });
    bool first = existing == saved.end();
    SavedWindow original{};
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (first) {
        original.hwnd = hwnd;
        original.tid = GetWindowThreadProcessId(hwnd, &original.pid);
        original.layered = (style & WS_EX_LAYERED) != 0;
        original.alpha = 255;
        original.flags = LWA_ALPHA;
        if (original.layered && !GetLayeredWindowAttributes(hwnd, &original.color, &original.alpha, &original.flags)) {
            StatusMessage(L"此窗口使用特殊的分层渲染，无法安全调整透明度。请选择其他窗口。");
            return false;
        }
        if (!SetPropW(hwnd, propertyName, ownerToken)) {
            ErrorMessage(L"访问窗口", GetLastError());
            return false;
        }
        saved.push_back(original);
        existing = saved.end() - 1;
    }
    if (!(style & WS_EX_LAYERED) && !SetStyle(hwnd, style | WS_EX_LAYERED)) {
        DWORD error = GetLastError();
        if (first) { RemovePropW(hwnd, propertyName); saved.pop_back(); }
        ErrorMessage(L"设置窗口属性", error);
        return false;
    }
    DWORD flags = LWA_ALPHA | (existing->flags & LWA_COLORKEY);
    BYTE alpha = static_cast<BYTE>((value * 255 + 50) / 100);
    if (!SetLayeredWindowAttributes(hwnd, existing->color, alpha, flags)) {
        DWORD error = GetLastError();
        if (first && RestoreWindow(*existing)) saved.pop_back();
        ErrorMessage(L"设置不透明度", error);
        return false;
    }
    CancelTemporary(hwnd);
    StatusMessage(L"已应用：不透明度 " + std::to_wstring(value) + L"%。退出本工具时自动恢复。");
    return true;
}
static int RestoreAllWindows() {
    ClearRegion::ClearAll();
    int failures = 0;
    for (auto it = saved.begin(); it != saved.end();) {
        if (RestoreWindow(*it)) it = saved.erase(it);
        else { ++failures; ++it; }
    }
    return failures;
}
static int OpacityOf(HWND hwnd) {
    COLORREF color = 0;
    BYTE alpha = 255;
    DWORD flags = 0;
    if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) &&
        GetLayeredWindowAttributes(hwnd, &color, &alpha, &flags) && (flags & LWA_ALPHA))
        return std::max(1, static_cast<int>((alpha * 100 + 127) / 255));
    return 100;
}
static bool PopupTargetValid() {
    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(popupTarget, &pid);
    return Eligible(popupTarget) && !IsIconic(popupTarget) && pid == popupPid && tid == popupTid;
}
static bool VisibleBounds(HWND hwnd, RECT& rect) {
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect))))
        return GetWindowRect(hwnd, &rect) != FALSE;
    return rect.right > rect.left && rect.bottom > rect.top;
}
static void HidePopup() {
    if (dragging) { dragging = false; ReleaseCapture(); }
    ShowWindow(popup, SW_HIDE);
    popupTarget = nullptr;
    hoverTarget = nullptr;
    hoverSince = 0;
}
static void PositionPopup() {
    RECT bounds{};
    if (!VisibleBounds(popupTarget, bounds)) return;
    MONITORINFO monitor{sizeof(monitor), {}, {}, 0};
    GetMonitorInfoW(MonitorFromWindow(popupTarget, MONITOR_DEFAULTTONEAREST), &monitor);
    int width = P(350), height = P(172);
    int x = bounds.left + anchorOffset - width / 2;
    int minX = std::max(static_cast<int>(bounds.left), static_cast<int>(monitor.rcWork.left));
    int maxX = std::min(static_cast<int>(bounds.right), static_cast<int>(monitor.rcWork.right)) - width;
    x = std::clamp(x, minX, std::max(minX, maxX));
    x = std::clamp(x, static_cast<int>(monitor.rcWork.left), std::max(static_cast<int>(monitor.rcWork.left), static_cast<int>(monitor.rcWork.right) - width));
    int y = std::clamp(static_cast<int>(bounds.top) + P(8), static_cast<int>(monitor.rcWork.top),
        std::max(static_cast<int>(monitor.rcWork.top), static_cast<int>(monitor.rcWork.bottom) - height));
    SetWindowPos(popup, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}
static void ConfigurePopupDpi(HWND hwnd) {
    using GetDpi = UINT(WINAPI*)(HWND);
    auto getDpi = reinterpret_cast<GetDpi>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    popupScale = getDpi ? getDpi(hwnd) / 96.0 : scale;
    if (popupFont) DeleteObject(popupFont);
    if (popupTitleFont) DeleteObject(popupTitleFont);
    if (popupSmallFont) DeleteObject(popupSmallFont);
    auto makeFont = [](int size, int weight) {
        return CreateFontW(-P(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    };
    popupFont = makeFont(14, FW_SEMIBOLD);
    popupTitleFont = makeFont(12, FW_NORMAL);
    popupSmallFont = makeFont(11, FW_NORMAL);
    SetWindowRgn(popup, CreateRoundRectRgn(0, 0, P(350)+1, P(172)+1, P(18), P(18)), TRUE);
}
static void OpenPopup(HWND hwnd, POINT cursor, bool interactive = true) {
    popupInteractive = interactive;
    LONG_PTR style = GetWindowLongPtrW(popup, GWL_EXSTYLE) | WS_EX_LAYERED;
    if (interactive) style &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
    else style |= WS_EX_TRANSPARENT;
    SetStyle(popup, style);
    SetLayeredWindowAttributes(popup, 0, 255, LWA_ALPHA);
    popupTarget = hwnd;
    popupTid = GetWindowThreadProcessId(hwnd, &popupPid);
    RECT bounds{};
    VisibleBounds(hwnd, bounds);
    anchorOffset = cursor.x - bounds.left;
    ConfigurePopupDpi(hwnd);
    popupValue = OpacityOf(hwnd);
    popupHint.clear();
    lastInside = GetTickCount64();
    PositionPopup();
    RECT shown{};
    GetWindowRect(popup, &shown);
    if (PtInRect(&shown, cursor)) {
        MONITORINFO monitor{sizeof(monitor), {}, {}, 0};
        GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
        int height = shown.bottom-shown.top;
        int y = cursor.y + P(14);
        if (y + height > monitor.rcWork.bottom) y = cursor.y - height - P(14);
        y = std::clamp(y, static_cast<int>(monitor.rcWork.top),
            std::max(static_cast<int>(monitor.rcWork.top), static_cast<int>(monitor.rcWork.bottom)-height));
        SetWindowPos(popup, HWND_TOPMOST, shown.left, y, shown.right-shown.left, height, SWP_NOACTIVATE);
    }
    InvalidateRect(popup, nullptr, FALSE);
}
static bool AtTopEdge(HWND hwnd, POINT point) {
    if (!Eligible(hwnd) || IsIconic(hwnd)) return false;
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (!(style & (WS_CAPTION | WS_THICKFRAME))) return false;
    if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return false;
    RECT bounds{};
    if (!VisibleBounds(hwnd, bounds)) return false;
    // Include the title bar's upper strip, while leaving normal clicks and drags alone.
    return point.x >= bounds.left && point.x < bounds.right &&
        point.y >= bounds.top && point.y < bounds.top + S(12);
}
static void HoverTick() {
    ClearRegion::Tick();
    if (!trayAdded && GetTickCount64() >= trayRetryAt) {
        trayRetryAt = GetTickCount64() + 5000;
        AddTray();
    }
    if (ClearRegion::selector || (!hoverEnabled && GetTickCount64() >= wheelFeedbackUntil) || countdown) { HidePopup(); return; }
    POINT point{};
    if (!GetCursorPos(&point)) { HidePopup(); return; }
    ULONGLONG now = GetTickCount64();
    if (IsWindowVisible(popup)) {
        if (!PopupTargetValid()) { HidePopup(); return; }
        if (!popupInteractive) {
            if (now >= wheelFeedbackUntil) HidePopup();
            return;
        }
        if (dragging) {
            POINT local = point;
            ScreenToClient(popup, &local);
            PopupApplyAt(local.x);
            // A non-activating background window may miss button-up outside its bounds.
            if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
                dragging = false;
                ReleaseCapture();
            }
            lastInside = now;
            return;
        }
        if (now < wheelFeedbackUntil) return;
        RECT rect{};
        GetWindowRect(popup, &rect);
        HWND under = GetAncestor(WindowFromPoint(point), GA_ROOT);
        if (PtInRect(&rect, point) && under == popup) {
            lastInside = now;
            PositionPopup();
            return;
        }
        if (under == popupTarget && AtTopEdge(popupTarget, point)) {
            lastInside = now;
            PositionPopup();
            return;
        }
        if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) || now - lastInside > 450) HidePopup();
        else return;
    }
    if ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000) {
        hoverTarget = nullptr;
        return;
    }
    HWND candidate = GetAncestor(WindowFromPoint(point), GA_ROOT);
    if (!AtTopEdge(candidate, point)) { hoverTarget = nullptr; return; }
    if (candidate != hoverTarget || abs(point.x - hoverPoint.x) > S(5) || abs(point.y - hoverPoint.y) > S(5)) {
        hoverTarget = candidate;
        hoverPoint = point;
        hoverSince = now;
        return;
    }
    if (now - hoverSince >= 320) OpenPopup(candidate, point);
}
static RECT PopupRestoreRect() { return RECT{P(268), P(61), P(332), P(91)}; }
static RECT PopupSelectRegionRect() { return RECT{P(18), P(134), P(218), P(162)}; }
static RECT PopupClearRegionRect() { return RECT{P(228), P(134), P(332), P(162)}; }
static void DrawPopup(HDC dc) {
    RECT client{};
    GetClientRect(popup, &client);
    HBRUSH background = CreateSolidBrush(RGB(28, 33, 44));
    FillRect(dc, &client, background);
    DeleteObject(background);
    SetBkMode(dc, TRANSPARENT);
    auto text = [&](const wchar_t* value, RECT rect, HFONT selectedFont, COLORREF color, UINT flags) {
        HGDIOBJ previous = SelectObject(dc, selectedFont);
        SetTextColor(dc, color);
        DrawTextW(dc, value, -1, &rect, flags | DT_NOPREFIX);
        SelectObject(dc, previous);
    };
    wchar_t title[256] = {};
    GetWindowTextW(popupTarget, title, 256);
    text(title, RECT{P(18), P(12), P(310), P(33)}, popupTitleFont, RGB(170, 183, 204), DT_SINGLELINE | DT_END_ELLIPSIS);
    text(L"×", RECT{P(318), P(9), P(341), P(34)}, popupFont, RGB(170, 183, 204), DT_CENTER | DT_SINGLELINE);
    text(L"不透明度", RECT{P(18), P(38), P(180), P(60)}, popupFont, RGB(243, 246, 255), DT_SINGLELINE);
    std::wstring value = std::to_wstring(popupValue) + L"%";
    text(value.c_str(), RECT{P(187), P(38), P(250), P(60)}, popupFont, RGB(110, 187, 255), DT_RIGHT | DT_SINGLELINE);
    int x = P(24) + (popupValue - 1) * (P(244) - P(24)) / 99;
    HPEN rail = CreatePen(PS_SOLID, P(5), RGB(67, 78, 97));
    HGDIOBJ oldPen = SelectObject(dc, rail);
    MoveToEx(dc, P(24), P(77), nullptr); LineTo(dc, P(244), P(77));
    HPEN filled = CreatePen(PS_SOLID, P(5), RGB(89, 165, 255));
    SelectObject(dc, filled);
    MoveToEx(dc, P(24), P(77), nullptr); LineTo(dc, x, P(77));
    SelectObject(dc, GetStockObject(NULL_PEN));
    HBRUSH thumb = CreateSolidBrush(RGB(229, 242, 255));
    HGDIOBJ oldBrush = SelectObject(dc, thumb);
    Ellipse(dc, x-P(7), P(70), x+P(7), P(84));
    HBRUSH button = CreateSolidBrush(RGB(49, 60, 78));
    SelectObject(dc, button);
    RECT restore = PopupRestoreRect();
    RoundRect(dc, restore.left, restore.top, restore.right, restore.bottom, P(10), P(10));
    RECT selectRegion = PopupSelectRegionRect(), clearRegion = PopupClearRegionRect();
    RoundRect(dc, selectRegion.left, selectRegion.top, selectRegion.right, selectRegion.bottom, P(10), P(10));
    RoundRect(dc, clearRegion.left, clearRegion.top, clearRegion.right, clearRegion.bottom, P(10), P(10));
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(rail); DeleteObject(filled); DeleteObject(thumb); DeleteObject(button);
    text(L"恢复", restore, popupTitleFont, RGB(225, 233, 249), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    text(L"框选清晰区域", selectRegion, popupTitleFont, RGB(225, 233, 249), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    text(L"取消清晰", clearRegion, popupTitleFont, RGB(225, 233, 249), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    text(popupHint.empty() ? L"拖动调整  ·  移开自动收起" : popupHint.c_str(),
        RECT{P(18), P(99), P(332), P(130)}, popupSmallFont,
        popupHint.empty() ? RGB(147, 163, 187) : RGB(255, 191, 135), DT_WORDBREAK);
}
static void PopupApplyAt(int x) {
    if (!PopupTargetValid()) { HidePopup(); return; }
    int value = 1 + (std::clamp(x, P(24), P(244)) - P(24)) * 99 / (P(244) - P(24));
    bool ok = ApplyOpacity(value, popupTarget);
    popupValue = OpacityOf(popupTarget);
    popupHint = ok ? L"" : lastStatus;
    if (target == popupTarget) ShowPercent(popupValue);
    InvalidateRect(popup, nullptr, FALSE);
}
static LRESULT CALLBACK PopupProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT rect{}; GetClientRect(hwnd, &rect);
        HDC buffer = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, rect.right, rect.bottom);
        HGDIOBJ old = SelectObject(buffer, bitmap);
        DrawPopup(buffer);
        BitBlt(dc, 0, 0, rect.right, rect.bottom, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, old); DeleteObject(bitmap); DeleteDC(buffer);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (point.x >= P(314) && point.y < P(36)) { HidePopup(); return 0; }
        RECT selectRegion = PopupSelectRegionRect(), clearRegion = PopupClearRegionRect();
        if (PtInRect(&selectRegion, point)) { BeginRegionSelection(popupTarget); return 0; }
        if (PtInRect(&clearRegion, point)) {
            popupHint = ClearRegion::Clear(popupTarget) ? L"已取消此窗口的清晰区域" : L"此窗口尚未设置清晰区域";
            InvalidateRect(hwnd, nullptr, FALSE); return 0;
        }
        RECT restore = PopupRestoreRect();
        if (PtInRect(&restore, point) && PopupTargetValid()) {
            Prune();
            auto it = std::find_if(saved.begin(), saved.end(), [](const SavedWindow& item) { return item.hwnd == popupTarget; });
            if (it != saved.end()) {
                if (RestoreWindow(*it)) { saved.erase(it); popupHint.clear(); }
                else { ErrorMessage(L"恢复窗口", GetLastError()); popupHint = lastStatus; }
            }
            popupValue = OpacityOf(popupTarget);
            if (target == popupTarget) ShowPercent(popupValue);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (point.x >= P(12) && point.x <= P(256) && point.y >= P(61) && point.y <= P(94)) {
            dragging = true;
            SetCapture(hwnd);
            PopupApplyAt(point.x);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (dragging) PopupApplyAt(GET_X_LPARAM(lp));
        return 0;
    case WM_LBUTTONUP:
        if (dragging) { PopupApplyAt(GET_X_LPARAM(lp)); dragging = false; ReleaseCapture(); lastInside = GetTickCount64(); }
        return 0;
    case WM_CAPTURECHANGED:
        dragging = false;
        lastInside = GetTickCount64();
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
static void LoadSettings() {
    wchar_t directory[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, directory))) return;
    std::wstring folder = std::wstring(directory) + L"\\WindowOpacity";
    CreateDirectoryW(folder.c_str(), nullptr);
    settingsPath = folder + L"\\settings.ini";
    shortcut.enabled = GetPrivateProfileIntW(L"Shortcut", L"Enabled", 1, settingsPath.c_str()) != 0;
    int modifiers = static_cast<int>(GetPrivateProfileIntW(L"Shortcut", L"Modifiers", 1, settingsPath.c_str()));
    int step = static_cast<int>(GetPrivateProfileIntW(L"Shortcut", L"Step", 5, settingsPath.c_str()));
    shortcut.modifiers = modifiers >= 1 && modifiers <= 15 ? modifiers : 1;
    shortcut.step = step >= 1 && step <= 100 ? step : 5;
}
static bool SaveSettings(const ShortcutOptions& options) {
    if (settingsPath.empty()) return false;
    std::wstring content = L"\xFEFF[Shortcut]\r\nEnabled=" + std::to_wstring(options.enabled ? 1 : 0) +
        L"\r\nModifiers=" + std::to_wstring(options.modifiers) + L"\r\nStep=" + std::to_wstring(options.step) + L"\r\n";
    std::wstring temporary = settingsPath + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD bytes = static_cast<DWORD>(content.size() * sizeof(wchar_t)), written = 0;
    bool ok = WriteFile(file, content.data(), bytes, &written, nullptr) && bytes == written;
    CloseHandle(file);
    if (ok) ok = MoveFileExW(temporary.c_str(), settingsPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok) DeleteFileW(temporary.c_str());
    return ok;
}
static std::wstring StartupCommand() {
    std::vector<wchar_t> path(32768);
    DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return L"";
    return L"\"" + std::wstring(path.data(), length) + L"\" --background";
}
static bool StartsAtLogon() {
    wchar_t value[32768] = {};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, runKey, runValue, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return false;
    std::wstring expected = StartupCommand();
    return !expected.empty() && expected == value;
}
static LSTATUS SetStartAtLogon(bool enabled) {
    if (!enabled) {
        HKEY key = nullptr;
        LSTATUS result = RegOpenKeyExW(HKEY_CURRENT_USER, runKey, 0, KEY_SET_VALUE, &key);
        if (result == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
        if (result != ERROR_SUCCESS) return result;
        result = RegDeleteValueW(key, runValue);
        RegCloseKey(key);
        return result == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : result;
    }
    std::wstring command = StartupCommand();
    if (command.empty() || command.size() >= 260) return ERROR_FILENAME_EXCED_RANGE;
    HKEY key = nullptr;
    LSTATUS result = RegCreateKeyExW(HKEY_CURRENT_USER, runKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (result != ERROR_SUCCESS) return result;
    result = RegSetValueExW(key, runValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return result;
}
static int PressedModifiers() {
    int modifiers = 0;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) modifiers |= 1;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) modifiers |= 2;
    if (GetAsyncKeyState(VK_MENU) & 0x8000) modifiers |= 4;
    if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) modifiers |= 8;
    return modifiers;
}
static LRESULT CALLBACK MouseHookProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && wp == WM_RBUTTONUP && suppressRightUp) {
        suppressRightUp = false;
        return 1;
    }
    if (code == HC_ACTION && (wp == WM_MOUSEWHEEL || wp == WM_RBUTTONDOWN)) {
        bool wheel = wp == WM_MOUSEWHEEL;
        if (dragging || (wheel && (!shortcut.enabled || PressedModifiers() != shortcut.modifiers))) {
            wheelPrevious = nullptr;
            wheelRemainder = 0;
            return CallNextHookEx(mouseHook, code, wp, lp);
        }
        const auto& mouse = *reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
        HWND hwnd = GetAncestor(WindowFromPoint(mouse.pt), GA_ROOT);
        if (hwnd == popup && IsWindowVisible(popup)) hwnd = popupTarget;
        DWORD pid = 0;
        DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
        if (!wheel) {
            bool active = std::any_of(temporaryOpacity.begin(), temporaryOpacity.end(), [hwnd](const SavedWindow& item) {
                return item.hwnd == hwnd && IdentityMatches(item);
            });
            bool pending = std::any_of(wheelRequests.begin(), wheelRequests.end(), [hwnd, pid, tid](const WheelRequest& request) {
                return request.toggle && request.hwnd == hwnd && request.pid == pid && request.tid == tid;
            });
            if (!active && !pending && PressedModifiers() != 1) return CallNextHookEx(mouseHook, code, wp, lp);
        }
        // Keep this callback short. Window mutation and painting run in the queued handler.
        if (hwnd && pid && pid != GetCurrentProcessId() && IsWindowVisible(hwnd) && !IsIconic(hwnd) &&
            hwnd != GetShellWindow() && hwnd != GetDesktopWindow() && GetWindowTextLengthW(hwnd) &&
            !(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) {
            if (wheelRequests.size() < 64) {
                wheelRequests.push_back(WheelRequest{hwnd, pid, tid, static_cast<short>(HIWORD(mouse.mouseData)), mouse.pt, GetTickCount64(), !wheel});
                if (!PostMessageW(mainWindow, WheelMessage, 0, 0)) {
                    wheelRequests.pop_back();
                    return CallNextHookEx(mouseHook, code, wp, lp);
                }
                if (!wheel) suppressRightUp = true;
                return 1;
            }
        }
    }
    return CallNextHookEx(mouseHook, code, wp, lp);
}
static void ProcessToggle(const WheelRequest& request) {
    Prune();
    auto it = std::find_if(temporaryOpacity.begin(), temporaryOpacity.end(), [&request](const SavedWindow& item) {
        return item.hwnd == request.hwnd;
    });
    bool restoring = it != temporaryOpacity.end();
    bool ok;
    if (restoring) {
        ok = RestoreAppearance(*it);
        if (ok) temporaryOpacity.erase(it);
        else ErrorMessage(L"恢复临时透明度", GetLastError());
    } else {
        SavedWindow state{};
        state.hwnd = request.hwnd; state.pid = request.pid; state.tid = request.tid;
        state.layered = (GetWindowLongPtrW(request.hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) != 0;
        state.alpha = 255; state.flags = LWA_ALPHA;
        if (state.layered && !GetLayeredWindowAttributes(request.hwnd, &state.color, &state.alpha, &state.flags)) {
            StatusMessage(L"此窗口使用特殊分层渲染，无法安全切换透明度。");
            ok = false;
        } else {
            ok = ApplyOpacity(100, request.hwnd);
            if (ok) temporaryOpacity.push_back(state);
        }
    }
    std::wstring error = lastStatus;
    if (!IsWindowVisible(popup) || popupTarget != request.hwnd || popupInteractive) OpenPopup(request.hwnd, request.point, false);
    popupValue = OpacityOf(request.hwnd);
    popupHint = ok ? (restoring ? L"已恢复切换前的透明度" : L"临时清晰模式  ·  再右键此窗口恢复") : error;
    wheelFeedbackUntil = GetTickCount64() + 1200;
    if (target == request.hwnd) ShowPercent(popupValue);
    InvalidateRect(popup, nullptr, FALSE);
}
static void ProcessWheel() {
    if (wheelRequests.empty()) return;
    WheelRequest request = wheelRequests.front();
    wheelRequests.pop_front();
    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(request.hwnd, &pid);
    if (pid != request.pid || tid != request.tid || !Eligible(request.hwnd) || IsIconic(request.hwnd)) return;
    if (request.toggle) { ProcessToggle(request); return; }
    if (!shortcut.enabled) return;
    if (wheelPrevious != request.hwnd || wheelPid != pid || wheelTid != tid || request.time - wheelTime > 1000) wheelRemainder = 0;
    wheelPrevious = request.hwnd; wheelPid = pid; wheelTid = tid; wheelTime = request.time;
    wheelRemainder += request.delta;
    int ticks = wheelRemainder / WHEEL_DELTA;
    wheelRemainder %= WHEEL_DELTA;
    if (!ticks) return;
    int before = OpacityOf(request.hwnd);
    int value = std::clamp(before + ticks * shortcut.step, 1, 100);
    bool ok = value == before || ApplyOpacity(value, request.hwnd);
    std::wstring error = lastStatus;
    if (!IsWindowVisible(popup) || popupTarget != request.hwnd || popupInteractive) OpenPopup(request.hwnd, request.point, false);
    popupValue = OpacityOf(request.hwnd);
    popupHint = ok ? L"上滚更清晰  ·  下滚更透明" : error;
    wheelFeedbackUntil = GetTickCount64() + 1000;
    if (target == request.hwnd) ShowPercent(popupValue);
    InvalidateRect(popup, nullptr, FALSE);
}
static void FillShortcutSettings(const ShortcutOptions& options) {
    CheckDlgButton(settingsWindow, ShortcutEnabled, options.enabled ? BST_CHECKED : BST_UNCHECKED);
    for (int i = 0; i < 4; ++i)
        CheckDlgButton(settingsWindow, ShortcutShift + i, (options.modifiers & (1 << i)) ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemInt(settingsWindow, ShortcutStep, options.step, FALSE);
}
static LRESULT CALLBACK SettingsProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CTLCOLORSTATIC:
        SetBkColor(reinterpret_cast<HDC>(wp), GetSysColor(COLOR_WINDOW));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    case WM_CREATE: {
        settingsWindow = hwnd;
        auto control = [hwnd](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int width, int height, int id = 0) {
            HWND child = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(width), S(height),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            return child;
        };
        control(L"STATIC", L"鼠标指向窗口，按住组合键并滚动", 0, 24, 20, 392, 26);
        control(L"BUTTON", L"启用滚轮调节透明度", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 60, 380, 28, ShortcutEnabled);
        control(L"STATIC", L"按住的组合键（可多选，至少一项）", 0, 24, 107, 390, 24);
        const wchar_t* names[] = {L"Shift", L"Ctrl", L"Alt", L"Win"};
        for (int i = 0; i < 4; ++i)
            control(L"BUTTON", names[i], BS_AUTOCHECKBOX | WS_TABSTOP, 24 + 96*i, 140, 90, 28, ShortcutShift+i);
        control(L"STATIC", L"每格透明度变化", 0, 24, 193, 155, 26);
        HWND edit = control(L"EDIT", L"", WS_BORDER | ES_NUMBER | ES_CENTER | WS_TABSTOP, 182, 190, 60, 30, ShortcutStep);
        SendMessageW(edit, EM_SETLIMITTEXT, 3, 0);
        control(L"STATIC", L"%（1–100）", 0, 254, 193, 155, 26);
        control(L"STATIC", L"可直接输入整数，例如 1%、3%、10%；保存后立即生效。", 0, 24, 232, 392, 44);
        control(L"BUTTON", L"开机自启动（登录后在托盘后台运行）", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 291, 392, 30, StartAtLogon);
        control(L"STATIC", L"不弹出控制面板；取消勾选并保存即可关闭自启动。", 0, 24, 330, 392, 46);
        control(L"BUTTON", L"恢复默认", WS_TABSTOP, 24, 393, 128, 36, ShortcutDefaults);
        control(L"BUTTON", L"保存设置", WS_TABSTOP | BS_DEFPUSHBUTTON, 270, 393, 145, 36, ShortcutSave);
        control(L"STATIC", L"", 0, 24, 445, 392, 50, ShortcutStatus);
        FillShortcutSettings(shortcut);
        CheckDlgButton(hwnd, StartAtLogon, StartsAtLogon() ? BST_CHECKED : BST_UNCHECKED);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ShortcutDefaults) {
            FillShortcutSettings(ShortcutOptions{});
            SetDlgItemTextW(hwnd, ShortcutStatus, L"已填入默认值，点击保存后生效。");
        } else if (LOWORD(wp) == ShortcutSave) {
            ShortcutOptions options;
            options.enabled = IsDlgButtonChecked(hwnd, ShortcutEnabled) == BST_CHECKED;
            options.modifiers = 0;
            for (int i = 0; i < 4; ++i)
                if (IsDlgButtonChecked(hwnd, ShortcutShift+i) == BST_CHECKED) options.modifiers |= 1 << i;
            BOOL valid = FALSE;
            options.step = static_cast<int>(GetDlgItemInt(hwnd, ShortcutStep, &valid, FALSE));
            if (!options.modifiers || !valid || options.step < 1 || options.step > 100) {
                SetDlgItemTextW(hwnd, ShortcutStatus, L"请至少选择一个组合键，并输入 1–100 的调整幅度。");
            } else if (!SaveSettings(options)) {
                SetDlgItemTextW(hwnd, ShortcutStatus, L"保存失败，请检查本地配置目录是否可写。");
            } else {
                shortcut = options;
                wheelRequests.clear(); wheelPrevious = nullptr; wheelRemainder = 0;
                bool autoStart = IsDlgButtonChecked(hwnd, StartAtLogon) == BST_CHECKED;
                LSTATUS result = SetStartAtLogon(autoStart);
                if (result == ERROR_SUCCESS) {
                    SetDlgItemTextW(hwnd, ShortcutStatus, autoStart ?
                        L"设置已保存。下次登录 Windows 后自动在托盘运行。" : L"设置已保存并立即生效，重启后仍然保留。");
                } else {
                    std::wstring error = L"滚轮设置已保存；自启动设置失败（错误 " + std::to_wstring(result) + L"）。";
                    SetDlgItemTextW(hwnd, ShortcutStatus, error.c_str());
                }
                CheckDlgButton(hwnd, StartAtLogon, StartsAtLogon() ? BST_CHECKED : BST_UNCHECKED);
            }
        }
        return 0;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: settingsWindow = nullptr; return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
static void ShowShortcutSettings() {
    HidePopup();
    if (!settingsWindow) {
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
        RECT rect{0, 0, S(440), S(500)};
        AdjustWindowRectEx(&rect, style, FALSE, 0);
        settingsWindow = CreateWindowExW(0, L"WindowOpacity.Settings", L"设置 · 快捷键与开机启动", style,
            CW_USEDEFAULT, CW_USEDEFAULT, rect.right-rect.left, rect.bottom-rect.top,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    }
    ShowWindow(settingsWindow, SW_RESTORE);
    SetForegroundWindow(settingsWindow);
}
static bool AddTray() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data); data.hWnd = mainWindow; data.uID = 1;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = TrayCallback;
    data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(data.szTip, L"窗口透明度：顶部悬停菜单 / 组合键 + 滚轮\n右键可设置快捷键", ARRAYSIZE(data.szTip));
    trayAdded = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    return trayAdded;
}
static void ShowPanel() {
    HidePopup();
    RefreshWindows();
    ShowWindow(mainWindow, SW_RESTORE);
    SetForegroundWindow(mainWindow);
}
static HWND PointedSource() {
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return nullptr;
    HWND source = GetAncestor(WindowFromPoint(cursor), GA_ROOT);
    if (source == popup && IsWindowVisible(popup)) return popupTarget;
    return ClearRegion::SourceForOverlay(source);
}
static void BeginRegionSelection(HWND source) {
    if (!ClearRegion::CanSelect(source)) {
        StatusMessage(L"请将鼠标指向目标窗口，按 Ctrl + Shift + F8 框选清晰区域。"); return;
    }
    HidePopup();
    if (IsWindowVisible(mainWindow)) ShowWindow(mainWindow, SW_HIDE);
    SetForegroundWindow(source);
    ClearRegion::Begin(source);
}
static void ExitApplication() {
    HidePopup();
    if (RestoreAllWindows()) {
        ShowPanel();
        StatusMessage(L"还有窗口未恢复。请先恢复全部窗口，再退出本工具。");
        return;
    }
    DestroyWindow(mainWindow);
}
static HWND Control(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0) {
    HWND hwnd = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
        mainWindow, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return hwnd;
}
static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (taskbarCreated && message == taskbarCreated) { AddTray(); return 0; }
    switch (message) {
    case WM_CTLCOLORSTATIC:
        SetBkColor(reinterpret_cast<HDC>(wp), GetSysColor(COLOR_WINDOW));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    case WM_CREATE: {
        mainWindow = hwnd;
        font = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        headingFont = CreateFontW(-S(25), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        HWND heading = Control(L"STATIC", L"窗口透明度", 0, 26, 21, 480, 38);
        SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(headingFont), TRUE);
        Control(L"STATIC", L"支持顶部悬停菜单，或使用组合键 + 鼠标滚轮调节。", 0, 27, 66, 510, 24);
        Control(L"STATIC", L"目标窗口", 0, 27, 112, 200, 24);
        windowList = Control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 26, 142, 508, 260, WindowList);
        SendMessageW(windowList, CB_SETDROPPEDWIDTH, S(680), 0);
        Control(L"BUTTON", L"刷新列表", WS_TABSTOP, 26, 182, 120, 33, Refresh);
        pickButton = Control(L"BUTTON", L"3 秒后选取活动窗口", WS_TABSTOP, 158, 182, 220, 33, Pick);
        Control(L"STATIC", L"不透明度（1–100%）", 0, 27, 242, 260, 24);
        slider = Control(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 21, 276, 365, 36, Slider);
        SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(1, 100));
        SendMessageW(slider, TBM_SETPAGESIZE, 0, 10);
        percentEdit = Control(L"EDIT", L"100", WS_BORDER | ES_NUMBER | ES_CENTER | WS_TABSTOP, 394, 278, 60, 30, Percent);
        SendMessageW(percentEdit, EM_SETLIMITTEXT, 3, 0);
        Control(L"BUTTON", L"应用", WS_TABSTOP | BS_DEFPUSHBUTTON, 464, 277, 70, 32, Apply);
        Control(L"STATIC", L"更透明", 0, 27, 316, 180, 23);
        Control(L"STATIC", L"更清晰", SS_RIGHT, 292, 316, 88, 23);
        Control(L"BUTTON", L"恢复所选窗口", WS_TABSTOP, 26, 365, 170, 38, Restore);
        Control(L"BUTTON", L"恢复全部窗口", WS_TABSTOP, 209, 365, 170, 38, RestoreAll);
        Control(L"BUTTON", L"软件设置", WS_TABSTOP, 392, 365, 142, 38, ShortcutCommand);
        Control(L"BUTTON", L"框选局部清晰", WS_TABSTOP, 26, 414, 245, 34, RegionSelectCommand);
        Control(L"BUTTON", L"取消所选窗口的清晰区域", WS_TABSTOP, 284, 414, 250, 34, RegionClearCommand);
        statusText = Control(L"STATIC", L"", 0, 27, 461, 508, 42, Status);
        Control(L"STATIC", L"Ctrl + Shift + F8 框选局部；Ctrl + Shift + F9 取消。\nShift + 右键临时清晰，再右键恢复；右键托盘可退出。", 0, 27, 518, 510, 44);
        RefreshWindows();
        return 0;
    }
    case WheelMessage: ProcessWheel(); return 0;
    case WM_HOTKEY:
        if (wp == RegionSelectHotkey) BeginRegionSelection(PointedSource());
        else if (wp == RegionClearHotkey)
            StatusMessage(ClearRegion::Clear(PointedSource()) ? L"已取消此窗口的清晰区域。" : L"鼠标下的窗口没有清晰区域。");
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case RegionSelectCommand: BeginRegionSelection(target); break;
        case RegionClearCommand:
            StatusMessage(ClearRegion::Clear(target) ? L"已取消所选窗口的清晰区域。" : L"所选窗口没有清晰区域。"); break;
        case RegionClearAllCommand:
            ClearRegion::ClearAll(); StatusMessage(L"已取消所有清晰区域。"); break;
        case ShortcutCommand: ShowShortcutSettings(); break;
        case PanelCommand: ShowPanel(); break;
        case PauseCommand:
            hoverEnabled = !hoverEnabled;
            HidePopup();
            break;
        case ExitCommand: ExitApplication(); break;
        case WindowList: if (HIWORD(wp) == CBN_SELCHANGE) SelectWindow(); break;
        case Refresh: RefreshWindows(); break;
        case Pick:
            if (!countdown) {
                countdown = 3;
                if (!SetTimer(hwnd, 1, 1000, nullptr)) { countdown = 0; StatusMessage(L"无法启动选取计时器。"); break; }
                EnableWindow(pickButton, FALSE);
                StatusMessage(L"请在 3 秒内切换到目标窗口（可使用 Alt+Tab）。");
            }
            break;
        case Apply: {
            wchar_t buffer[16] = {};
            GetWindowTextW(percentEdit, buffer, 16);
            int value = _wtoi(buffer);
            if (value < 1 || value > 100) { StatusMessage(L"请输入 1 到 100 之间的整数。"); break; }
            if (ApplyOpacity(value)) ShowPercent(value);
            break;
        }
        case Restore: {
            Prune();
            auto it = std::find_if(saved.begin(), saved.end(), [](const SavedWindow& item) { return item.hwnd == target; });
            if (it == saved.end()) { StatusMessage(L"所选窗口没有需要恢复的改动。"); break; }
            if (RestoreWindow(*it)) { saved.erase(it); SelectWindow(); StatusMessage(L"已恢复所选窗口的原始状态。"); }
            else ErrorMessage(L"恢复窗口", GetLastError());
            break;
        }
        case RestoreAll:
            if (RestoreAllWindows()) StatusMessage(L"部分窗口恢复失败，请重试或检查运行权限。");
            else { SelectWindow(); StatusMessage(L"已恢复所有窗口的原始状态。"); }
            break;
        }
        return 0;
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lp) == slider) {
            int value = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
            SetWindowTextW(percentEdit, std::to_wstring(value).c_str());
            ApplyOpacity(value);
        }
        return 0;
    case WM_TIMER:
        if (wp == HoverTimer) { HoverTick(); return 0; }
        if (wp == 1 && countdown > 0 && --countdown == 0) {
            KillTimer(hwnd, 1);
            EnableWindow(pickButton, TRUE);
            HWND active = GetAncestor(GetForegroundWindow(), GA_ROOT);
            if (Eligible(active)) RefreshWindows(active);
            else StatusMessage(L"未选到其他应用，请再次选取并切换到目标窗口。");
        }
        return 0;
    case TrayCallback:
        if (lp == WM_LBUTTONUP) ShowPanel();
        else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) {
            HidePopup();
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, PanelCommand, L"打开控制面板");
            AppendMenuW(menu, MF_STRING, ShortcutCommand, L"设置（滚轮幅度 / 开机启动）…");
            AppendMenuW(menu, MF_STRING | (hoverEnabled ? MF_CHECKED : MF_UNCHECKED), PauseCommand, L"启用顶部悬停菜单");
            AppendMenuW(menu, MF_STRING, RestoreAll, L"恢复全部窗口");
            AppendMenuW(menu, MF_STRING, RegionClearAllCommand, L"取消所有局部清晰区域");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, ExitCommand, L"退出并恢复全部窗口");
            POINT point{}; GetCursorPos(&point);
            SetForegroundWindow(hwnd);
            UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            PostMessageW(hwnd, WM_NULL, 0, 0);
            if (command) PostMessageW(hwnd, WM_COMMAND, command, 0);
        }
        return 0;
    case WM_QUERYENDSESSION:
        RestoreAllWindows();
        return TRUE;
    case WM_CLOSE:
        if (trayAdded) ShowWindow(hwnd, SW_HIDE);
        else ExitApplication();
        return 0;
    case WM_DESTROY:
        UnregisterHotKey(hwnd, RegionSelectHotkey);
        UnregisterHotKey(hwnd, RegionClearHotkey);
        ClearRegion::ClearAll();
        if (mouseHook) { UnhookWindowsHookEx(mouseHook); mouseHook = nullptr; }
        KillTimer(hwnd, HoverTimer);
        HidePopup();
        if (trayAdded) {
            NOTIFYICONDATAW data{};
            data.cbSize = sizeof(data); data.hWnd = hwnd; data.uID = 1;
            Shell_NotifyIconW(NIM_DELETE, &data);
        }
        if (popup) DestroyWindow(popup);
        if (settingsWindow) DestroyWindow(settingsWindow);
        DeleteObject(popupFont);
        DeleteObject(popupTitleFont);
        DeleteObject(popupSmallFont);
        DeleteObject(font);
        DeleteObject(headingFont);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int show) {
    bool background = wcsstr(commandLine, L"--background") != nullptr;
    SetProcessDPIAware();
    HDC dc = GetDC(nullptr);
    scale = GetDeviceCaps(dc, LOGPIXELSX) / 96.0;
    ReleaseDC(nullptr, dc);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\WindowOpacity.8CF3470F");
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND previous = FindWindowW(L"WindowOpacity.Main", nullptr);
        if (previous && !background) SendMessageW(previous, WM_COMMAND, PanelCommand, 0);
        CloseHandle(mutex);
        return 0;
    }
    ownerToken = reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(GetCurrentProcessId()));
    LoadSettings();
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSW cls{};
    cls.lpfnWndProc = WindowProc;
    cls.hInstance = instance;
    cls.lpszClassName = L"WindowOpacity.Main";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&cls)) { CloseHandle(mutex); return 1; }
    WNDCLASSW popupClass{};
    popupClass.lpfnWndProc = PopupProc;
    popupClass.hInstance = instance;
    popupClass.lpszClassName = L"WindowOpacity.Hover";
    popupClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&popupClass)) { CloseHandle(mutex); return 1; }
    WNDCLASSW settingsClass = cls;
    settingsClass.lpfnWndProc = SettingsProc;
    settingsClass.lpszClassName = L"WindowOpacity.Settings";
    if (!RegisterClassW(&settingsClass)) { CloseHandle(mutex); return 1; }
    if (!ClearRegion::Initialize(instance, StatusMessage)) { CloseHandle(mutex); return 1; }
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rect{0, 0, S(560), S(574)};
    AdjustWindowRectEx(&rect, style, FALSE, 0);
    HWND hwnd = CreateWindowExW(0, cls.lpszClassName, L"窗口透明度 · WindowOpacity", style,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, instance, nullptr);
    if (!hwnd) { CloseHandle(mutex); return 1; }
    popup = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        popupClass.lpszClassName, L"窗口透明度浮动菜单", WS_POPUP,
        0, 0, S(350), S(132), nullptr, nullptr, instance, nullptr);
    if (!popup || !SetTimer(hwnd, HoverTimer, 60, nullptr)) {
        MessageBoxW(hwnd, L"无法创建顶部悬停菜单。", L"窗口透明度", MB_ICONERROR);
        DestroyWindow(hwnd); CloseHandle(mutex); return 1;
    }
    bool added = AddTray();
    bool regionHotkey = RegisterHotKey(hwnd, RegionSelectHotkey, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F8) != FALSE;
    bool regionClearHotkey = RegisterHotKey(hwnd, RegionClearHotkey, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F9) != FALSE;
    if (!regionHotkey || !regionClearHotkey)
        StatusMessage(L"局部清晰快捷键被占用。仍可通过顶部菜单或控制面板框选和取消。");
    mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, instance, 0);
    if (!mouseHook) StatusMessage(L"滚轮快捷键启动失败，请退出后重新运行。顶部菜单仍可使用。");
    if (!background && (!added || !mouseHook || wcsstr(commandLine, L"--panel"))) ShowWindow(hwnd, show);
    MSG message{};
    int result;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (!(settingsWindow && IsDialogMessageW(settingsWindow, &message)) && !IsDialogMessageW(hwnd, &message)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    }
    CloseHandle(mutex);
    return result == -1 ? 1 : static_cast<int>(message.wParam);
}
