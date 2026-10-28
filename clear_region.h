#pragma once
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <algorithm>
#include <string>
#include <vector>

// In-place, click-through views of an unmodified DWM source surface.
namespace ClearRegion {
struct View {
    HWND source{}, window{};
    DWORD pid{}, tid{};
    HTHUMBNAIL thumbnail{};
    RECT area{};
    UINT dpi = 96;
};
inline std::vector<View> views;
inline HINSTANCE instance;
inline void (*report)(const std::wstring&);
inline constexpr const wchar_t* marker = L"WindowOpacity.ClearRegion.8CF3470F";
inline HWND selector{}, selectionSource{}, previousFocus{};
inline DWORD selectionPid{}, selectionTid{};
inline RECT selectionBounds{}, originalClient{};
inline POINT startPoint{}, endPoint{};
inline bool selectingDrag;
inline HFONT selectionFont;
inline HANDLE Token() { return reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(GetCurrentProcessId())); }
inline void Say(const wchar_t* message) { if (report) report(message); }
inline bool SameRect(const RECT& a, const RECT& b) { return EqualRect(&a, &b) != FALSE; }
inline UINT Dpi(HWND hwnd) {
    using Getter = UINT(WINAPI*)(HWND);
    static auto getter = reinterpret_cast<Getter>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    return getter ? std::max(96U, getter(hwnd)) : 96;
}
inline bool Identity(HWND hwnd, DWORD pid, DWORD tid) {
    DWORD actual = 0;
    return IsWindow(hwnd) && GetWindowThreadProcessId(hwnd, &actual) == tid && actual == pid;
}
inline bool ClientBounds(HWND hwnd, RECT& rect) {
    if (!GetClientRect(hwnd, &rect)) return false;
    POINT origin{0, 0};
    if (!ClientToScreen(hwnd, &origin)) return false;
    OffsetRect(&rect, origin.x, origin.y);
    return rect.right > rect.left && rect.bottom > rect.top;
}
inline bool Visible(HWND hwnd) {
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return false;
    DWORD cloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return !cloaked;
}
inline bool CanSelect(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return hwnd && pid && pid != GetCurrentProcessId() && Visible(hwnd) &&
        hwnd != GetShellWindow() && hwnd != GetDesktopWindow() && GetWindowTextLengthW(hwnd);
}
inline bool IsOverlay(HWND hwnd) {
    return std::any_of(views.begin(), views.end(), [hwnd](const View& view) { return view.window == hwnd; });
}
inline HWND SourceForOverlay(HWND hwnd) {
    for (const auto& view : views) if (view.window == hwnd) return view.source;
    return hwnd;
}
inline void Dispose(View& view, bool removeMarker = true) {
    if (view.thumbnail) DwmUnregisterThumbnail(view.thumbnail);
    if (view.window) DestroyWindow(view.window);
    if (removeMarker && Identity(view.source, view.pid, view.tid) && GetPropW(view.source, marker) == Token())
        RemovePropW(view.source, marker);
    view.thumbnail = nullptr; view.window = nullptr;
}
inline bool Clear(HWND source) {
    bool removed = false;
    for (auto it = views.begin(); it != views.end();) {
        if (it->source == source) { Dispose(*it); it = views.erase(it); removed = true; }
        else ++it;
    }
    return removed;
}
inline void CancelSelection() {
    if (!selector) return;
    HWND window = selector;
    bool restoreFocus = GetForegroundWindow() == window;
    selector = nullptr;
    selectionSource = nullptr;
    selectingDrag = false;
    if (GetCapture() == window) ReleaseCapture();
    DestroyWindow(window);
    if (selectionFont) { DeleteObject(selectionFont); selectionFont = nullptr; }
    if (restoreFocus && IsWindow(previousFocus)) SetForegroundWindow(previousFocus);
}
inline void ClearAll() {
    CancelSelection();
    for (auto& view : views) Dispose(view);
    views.clear();
}
inline bool Update(View& view) {
    if (!Identity(view.source, view.pid, view.tid) || GetPropW(view.source, marker) != Token()) return false;
    if (selector || !Visible(view.source)) { ShowWindow(view.window, SW_HIDE); return true; }
    RECT client{}, frame{};
    if (!ClientBounds(view.source, client) || !GetWindowRect(view.source, &frame)) return false;
    double ratio = static_cast<double>(Dpi(view.source)) / view.dpi;
    auto scaled = [ratio](LONG value) { return static_cast<LONG>(value * ratio + .5); };
    RECT area{scaled(view.area.left), scaled(view.area.top), scaled(view.area.right), scaled(view.area.bottom)};
    RECT localClient{0, 0, client.right-client.left, client.bottom-client.top}, clipped{};
    if (!IntersectRect(&clipped, &area, &localClient)) { ShowWindow(view.window, SW_HIDE); return true; }
    RECT destination = clipped;
    OffsetRect(&destination, client.left, client.top);
    // rcSource uses window-relative coordinates, including the invisible resize
    // margins. QueryThumbnailSourceSize excludes those margins on modern Windows,
    // so rescaling by that size would visibly shift and stretch a cropped region.
    DWM_THUMBNAIL_PROPERTIES properties{};
    properties.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
    properties.rcDestination = RECT{0, 0, destination.right-destination.left, destination.bottom-destination.top};
    properties.rcSource = RECT{destination.left-frame.left, destination.top-frame.top,
        destination.right-frame.left, destination.bottom-frame.top};
    properties.opacity = 255; properties.fVisible = TRUE; properties.fSourceClientAreaOnly = FALSE;
    if (FAILED(DwmUpdateThumbnailProperties(view.thumbnail, &properties))) return false;
    // Stay immediately above the source, below windows that already cover it.
    // Ownership keeps the two together when Windows changes their stacking band.
    HWND above = GetWindow(view.source, GW_HWNDPREV);
    while (above && IsOverlay(above)) above = GetWindow(above, GW_HWNDPREV);
    SetWindowPos(view.window, above ? above : HWND_TOP, destination.left, destination.top,
        destination.right-destination.left, destination.bottom-destination.top,
        SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    return true;
}
inline bool Create(HWND source, RECT area) {
    if (!CanSelect(source)) { Say(L"目标窗口已不可用，无法创建清晰区域。"); return false; }
    View view{};
    view.source = source; view.tid = GetWindowThreadProcessId(source, &view.pid); view.area = area; view.dpi = Dpi(source);
    bool replacing = std::any_of(views.begin(), views.end(), [source](const View& old) { return old.source == source; });
    if (!SetPropW(source, marker, Token())) {
        Say(L"无法访问目标窗口。管理员窗口需要以管理员身份运行本工具。"); return false;
    }
    view.window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        L"WindowOpacity.ClearRegion", L"局部清晰预览", WS_POPUP, 0, 0, 1, 1, source, nullptr, instance, nullptr);
    if (!view.window || !SetLayeredWindowAttributes(view.window, 0, 255, LWA_ALPHA) ||
        FAILED(DwmRegisterThumbnail(view.window, source, &view.thumbnail))) {
        Dispose(view, !replacing);
        Say(L"此窗口不支持实时局部预览，或访问权限不足。"); return false;
    }
    for (auto it = views.begin(); it != views.end();) {
        if (it->source == source) { Dispose(*it, false); it = views.erase(it); }
        else ++it;
    }
    views.push_back(view);
    if (!Update(views.back())) { Dispose(views.back()); views.pop_back(); Say(L"无法显示此窗口的清晰区域。"); return false; }
    Say(L"局部清晰已开启：画面实时更新。Ctrl + Shift + F9 可取消鼠标下窗口的清晰区域。");
    return true;
}
inline RECT SelectionRect() {
    return RECT{std::min(startPoint.x, endPoint.x), std::min(startPoint.y, endPoint.y),
        std::max(startPoint.x, endPoint.x), std::max(startPoint.y, endPoint.y)};
}
inline POINT ClampedPoint(LPARAM lp) {
    return POINT{std::clamp(GET_X_LPARAM(lp), 0, static_cast<int>(selectionBounds.right-selectionBounds.left)),
        std::clamp(GET_Y_LPARAM(lp), 0, static_cast<int>(selectionBounds.bottom-selectionBounds.top))};
}
inline LRESULT CALLBACK ViewProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(hwnd, message, wp, lp);
}
inline LRESULT CALLBACK SelectionProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(hwnd, &paint);
        RECT client{}; GetClientRect(hwnd, &client); FillRect(dc, &client, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
        HGDIOBJ oldFont = SelectObject(dc, selectionFont);
        RECT caption{12, 10, client.right-12, 55};
        DrawTextW(dc, L"拖动框选清晰区域 · Esc / 右键取消", -1, &caption, DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, oldFont);
        if (selectingDrag) {
            HPEN pen = CreatePen(PS_SOLID, 3, RGB(70, 185, 255));
            HGDIOBJ oldPen = SelectObject(dc, pen), oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            RECT selected = SelectionRect(); Rectangle(dc, selected.left, selected.top, selected.right, selected.bottom);
            SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(pen);
        }
        EndPaint(hwnd, &paint); return 0;
    }
    case WM_LBUTTONDOWN:
        startPoint = endPoint = ClampedPoint(lp); selectingDrag = true; SetCapture(hwnd); InvalidateRect(hwnd, nullptr, TRUE); return 0;
    case WM_MOUSEMOVE:
        if (selectingDrag) { endPoint = ClampedPoint(lp); InvalidateRect(hwnd, nullptr, TRUE); }
        return 0;
    case WM_LBUTTONUP:
        if (selectingDrag) {
            endPoint = ClampedPoint(lp);
            RECT area = SelectionRect();
            OffsetRect(&area, selectionBounds.left-originalClient.left, selectionBounds.top-originalClient.top);
            HWND source = selectionSource; DWORD pid = selectionPid, tid = selectionTid;
            CancelSelection();
            if (area.right-area.left < 16 || area.bottom-area.top < 16) Say(L"框选区域太小，请至少选择 16 × 16 像素。");
            else if (Identity(source, pid, tid)) Create(source, area);
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { CancelSelection(); Say(L"已取消框选，原有区域保持不变。"); }
        return 0;
    case WM_RBUTTONDOWN:
    case WM_CLOSE:
        CancelSelection(); Say(L"已取消框选，原有区域保持不变。"); return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && selector == hwnd) CancelSelection();
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
inline bool Begin(HWND source) {
    if (!CanSelect(source)) { Say(L"请先将鼠标指向目标窗口，再按 Ctrl + Shift + F8 框选。"); return false; }
    CancelSelection();
    if (!ClientBounds(source, originalClient)) return false;
    RECT desktop{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_XVIRTUALSCREEN)+GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN)+GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    if (!IntersectRect(&selectionBounds, &originalClient, &desktop)) return false;
    selectionSource = source; selectionTid = GetWindowThreadProcessId(source, &selectionPid);
    previousFocus = GetForegroundWindow(); selectingDrag = false;
    selectionFont = CreateFontW(-MulDiv(16, Dpi(source), 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    selector = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        L"WindowOpacity.RegionSelector", L"框选清晰区域", WS_POPUP, selectionBounds.left, selectionBounds.top,
        selectionBounds.right-selectionBounds.left, selectionBounds.bottom-selectionBounds.top, nullptr, nullptr, instance, nullptr);
    if (!selector) {
        if (selectionFont) { DeleteObject(selectionFont); selectionFont = nullptr; }
        selectionSource = nullptr; Say(L"无法启动框选。"); return false;
    }
    SetLayeredWindowAttributes(selector, 0, 90, LWA_ALPHA);
    for (auto& view : views) ShowWindow(view.window, SW_HIDE);
    ShowWindow(selector, SW_SHOW); SetForegroundWindow(selector); SetFocus(selector);
    Say(L"拖动框选要保持清晰的区域，按 Esc 或右键可取消。");
    return true;
}
inline void Tick() {
    if (selector) {
        RECT current{};
        if (!Identity(selectionSource, selectionPid, selectionTid) || !Visible(selectionSource) ||
            !ClientBounds(selectionSource, current) || !SameRect(current, originalClient)) CancelSelection();
    }
    for (auto it = views.begin(); it != views.end();) {
        if (!Update(*it)) { Dispose(*it); it = views.erase(it); }
        else ++it;
    }
}
inline bool Initialize(HINSTANCE module, void (*reporter)(const std::wstring&)) {
    instance = module; report = reporter;
    WNDCLASSW cls{}; cls.hInstance = module; cls.lpfnWndProc = ViewProc;
    cls.lpszClassName = L"WindowOpacity.ClearRegion"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (!RegisterClassW(&cls)) return false;
    cls.lpfnWndProc = SelectionProc; cls.lpszClassName = L"WindowOpacity.RegionSelector";
    cls.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    return RegisterClassW(&cls) != 0;
}
} // namespace ClearRegion
