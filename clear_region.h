#pragma once
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <algorithm>
#include <string>
#include <vector>

// Rectangular DWM tiles form one non-overlapping live view of each source.
// Keeping the real source at alpha 1 preserves input hit testing, while each
// tile supplies the remaining alpha needed for its independent opacity.
namespace ClearRegion {
struct View { HWND source{}; RECT area{}; UINT dpi = 96; unsigned id{}; int opacity = 100; };
struct Tile { HWND window{}; HTHUMBNAIL thumbnail{}; };
struct Source {
    HWND window{}; DWORD pid{}, tid{};
    bool layered{}, suspended = true;
    COLORREF color{}; BYTE alpha = 255; DWORD flags = LWA_ALPHA;
    std::vector<Tile> tiles;
};
struct Piece { RECT rect; int alpha; unsigned id; };
inline std::vector<View> views;
inline std::vector<Source> sources;
inline unsigned nextId = 1;
inline HINSTANCE instance;
inline void (*report)(const std::wstring&);
inline constexpr const wchar_t* marker = L"WindowOpacity.ClearRegion.8CF3470F";
inline HWND selector{}, selectionSource{}, previousFocus{};
inline DWORD selectionPid{}, selectionTid{};
inline RECT selectionBounds{}, originalClient{};
inline POINT startPoint{}, endPoint{};
inline bool selectingDrag;
inline HFONT selectionFont;
inline HWND manager{}, managedSource{};
inline DWORD managedPid{}, managedTid{};
inline HFONT managerFont;
inline unsigned selectedId{};
inline double managerScale = 1;
enum { RegionList = 401, RegionSlider, RegionValue, RegionEdit, RegionApply, RegionDelete, RegionAdd, RegionClear };
inline bool Begin(HWND source);
inline void OpenManager(HWND source, unsigned preferred = 0);
inline void RefreshManager(unsigned preferred = 0);
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

inline Source* FindSource(HWND hwnd) {
    for (auto& source : sources) if (source.window == hwnd && Identity(hwnd, source.pid, source.tid) &&
        GetPropW(hwnd, marker) == Token()) return &source;
    return nullptr;
}
inline bool IsLayered(HWND hwnd) {
    if (auto source = FindSource(hwnd)) return source->layered;
    return (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) != 0;
}
inline bool ReadAppearance(HWND hwnd, COLORREF* color, BYTE* alpha, DWORD* flags) {
    if (auto source = FindSource(hwnd)) {
        *color = source->color; *alpha = source->alpha; *flags = source->flags; return true;
    }
    return GetLayeredWindowAttributes(hwnd, color, alpha, flags) != FALSE;
}
inline bool WriteAppearance(HWND hwnd, COLORREF color, BYTE alpha, DWORD flags) {
    if (auto source = FindSource(hwnd)) {
        // Color-keyed sources cannot be faithfully partitioned by rectangular tiles.
        if (flags & LWA_COLORKEY) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
        if (!SetLayeredWindowAttributes(hwnd, color, source->suspended ? alpha : 1, flags)) return false;
        source->layered = true; source->color = color; source->alpha = alpha; source->flags = flags;
        return true;
    }
    return SetLayeredWindowAttributes(hwnd, color, alpha, flags) != FALSE;
}
inline bool RestoreUnlayered(HWND hwnd) {
    auto source = FindSource(hwnd);
    if (!source) return false;
    source->layered = false; source->alpha = 255; source->flags = LWA_ALPHA;
    return true;
}
inline bool IsOverlay(HWND hwnd) {
    for (const auto& source : sources) for (const auto& tile : source.tiles) if (tile.window == hwnd) return true;
    return false;
}
inline HWND SourceForOverlay(HWND hwnd) {
    for (const auto& source : sources) for (const auto& tile : source.tiles) if (tile.window == hwnd) return source.window;
    return hwnd;
}
inline void DestroyTile(Tile& tile) {
    if (tile.thumbnail) DwmUnregisterThumbnail(tile.thumbnail);
    if (tile.window) DestroyWindow(tile.window);
    tile = {};
}
inline bool RestoreSource(Source& source) {
    if (!Identity(source.window, source.pid, source.tid) || GetPropW(source.window, marker) != Token()) return true;
    if (source.layered) return SetLayeredWindowAttributes(source.window, source.color, source.alpha, source.flags) != FALSE;
    SetLastError(0);
    auto old = SetWindowLongPtrW(source.window, GWL_EXSTYLE,
        GetWindowLongPtrW(source.window, GWL_EXSTYLE) & ~static_cast<LONG_PTR>(WS_EX_LAYERED));
    if (!old && GetLastError()) return false;
    RedrawWindow(source.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    return true;
}
inline bool Suspend(Source& source) {
    if (!RestoreSource(source)) return false;
    source.suspended = true;
    for (auto& tile : source.tiles) ShowWindow(tile.window, SW_HIDE);
    return true;
}
inline bool Dispose(Source& source) {
    // Keep the recoverable state and tiles if Windows refuses restoration.
    if (!RestoreSource(source)) return false;
    for (auto& tile : source.tiles) DestroyTile(tile);
    if (Identity(source.window, source.pid, source.tid) && GetPropW(source.window, marker) == Token()) RemovePropW(source.window, marker);
    return true;
}
inline bool Clear(HWND hwnd) {
    bool removed = false;
    for (auto it = sources.begin(); it != sources.end();) {
        if (it->window == hwnd) {
            if (!Dispose(*it)) { Say(L"恢复目标窗口失败，请检查权限后重试。"); return false; }
            it = sources.erase(it); removed = true;
        } else ++it;
    }
    views.erase(std::remove_if(views.begin(), views.end(), [hwnd](const View& v) { return v.source == hwnd; }), views.end());
    if (manager && managedSource == hwnd) RefreshManager();
    return removed;
}
inline void CancelSelection() {
    if (!selector) return;
    HWND window = selector;
    bool restoreFocus = GetForegroundWindow() == window;
    selector = nullptr; selectionSource = nullptr; selectingDrag = false;
    if (GetCapture() == window) ReleaseCapture();
    DestroyWindow(window);
    if (selectionFont) { DeleteObject(selectionFont); selectionFont = nullptr; }
    if (restoreFocus && IsWindow(previousFocus)) SetForegroundWindow(previousFocus);
    if (manager) ShowWindow(manager, SW_SHOWNOACTIVATE);
}
inline void ClearAll() {
    CancelSelection();
    if (manager) DestroyWindow(manager);
    for (auto it = sources.begin(); it != sources.end();) {
        HWND hwnd = it->window;
        if (Dispose(*it)) {
            it = sources.erase(it);
            views.erase(std::remove_if(views.begin(), views.end(), [hwnd](const View& v) { return v.source == hwnd; }), views.end());
        } else { Say(L"仍有局部区域未恢复，请检查目标窗口权限后重试。"); ++it; }
    }
}
inline void Subtract(std::vector<Piece>& pieces, RECT area) {
    std::vector<Piece> result;
    for (const auto& p : pieces) {
        RECT cut{};
        if (!IntersectRect(&cut, &p.rect, &area)) { result.push_back(p); continue; }
        const RECT parts[] = {{p.rect.left,p.rect.top,p.rect.right,cut.top}, {p.rect.left,cut.bottom,p.rect.right,p.rect.bottom},
            {p.rect.left,cut.top,cut.left,cut.bottom}, {cut.right,cut.top,p.rect.right,cut.bottom}};
        for (const RECT& rect : parts) if (rect.right > rect.left && rect.bottom > rect.top) result.push_back({rect,p.alpha,p.id});
    }
    pieces.swap(result);
}
inline bool Update(Source& source) {
    HWND hwnd = source.window;
    if (!Identity(hwnd, source.pid, source.tid) || GetPropW(hwnd, marker) != Token()) return false;
    if (selector || !Visible(hwnd)) {
        if (!source.suspended) Suspend(source);
        return true;
    }
    RECT client{}, frame{}, bounds{};
    if (!ClientBounds(hwnd, client) || !GetWindowRect(hwnd, &frame)) return false;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) bounds = frame;
    std::vector<Piece> pieces{{bounds, (source.flags & LWA_ALPHA) ? source.alpha : 255, 0}};
    for (const auto& view : views) if (view.source == hwnd) {
        double ratio = static_cast<double>(Dpi(hwnd)) / view.dpi;
        auto scaled = [ratio](LONG value) { return static_cast<LONG>(value * ratio + .5); };
        RECT area{client.left+scaled(view.area.left), client.top+scaled(view.area.top),
            client.left+scaled(view.area.right), client.top+scaled(view.area.bottom)}, clipped{};
        if (IntersectRect(&clipped, &area, &client)) {
            Subtract(pieces, clipped);
            pieces.push_back({clipped,(view.opacity*255+50)/100,view.id});
        }
    }
    // Allocate before changing the source's appearance. A failed addition leaves
    // existing regions intact and never strands an almost invisible window.
    const size_t previousCount = source.tiles.size();
    while (source.tiles.size() < pieces.size()) {
        Tile tile{};
        tile.window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
            L"WindowOpacity.ClearRegion", L"局部透明度", WS_POPUP, 0, 0, 1, 1, hwnd, nullptr, instance, nullptr);
        if (!tile.window || !SetLayeredWindowAttributes(tile.window, 0, 255, LWA_ALPHA) ||
            FAILED(DwmRegisterThumbnail(tile.window, hwnd, &tile.thumbnail))) {
            DestroyTile(tile);
            while (source.tiles.size() > previousCount) { DestroyTile(source.tiles.back()); source.tiles.pop_back(); }
            return false;
        }
        source.tiles.push_back(tile);
    }
    if (source.suspended) {
        LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (!(style & WS_EX_LAYERED)) {
            SetLastError(0);
            if (!SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style | WS_EX_LAYERED) && GetLastError()) return false;
        }
        if (!SetLayeredWindowAttributes(hwnd, 0, 1, LWA_ALPHA)) return false;
        source.suspended = false;
    }
    HWND above = GetWindow(hwnd, GW_HWNDPREV);
    while (above && IsOverlay(above)) above = GetWindow(above, GW_HWNDPREV);
    for (size_t i = 0; i < pieces.size(); ++i) {
        auto& tile = source.tiles[i]; const auto& p = pieces[i];
        DWM_THUMBNAIL_PROPERTIES properties{};
        properties.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        properties.rcDestination = RECT{0,0,p.rect.right-p.rect.left,p.rect.bottom-p.rect.top};
        properties.rcSource = RECT{p.rect.left-frame.left,p.rect.top-frame.top,p.rect.right-frame.left,p.rect.bottom-frame.top};
        // Compensate the alpha-1 input surface below the tile: a + b(1-a).
        BYTE alpha = static_cast<BYTE>(std::clamp((p.alpha-1)*255/254, 0, 255));
        if (!SetLayeredWindowAttributes(tile.window, 0, alpha, LWA_ALPHA)) return false;
        properties.opacity = 255;
        properties.fVisible = TRUE; properties.fSourceClientAreaOnly = FALSE;
        if (FAILED(DwmUpdateThumbnailProperties(tile.thumbnail, &properties))) return false;
        SetWindowTextW(tile.window, (p.id ? L"区域 " + std::to_wstring(p.id) : L"窗口其余部分").c_str());
        SetWindowPos(tile.window, above ? above : HWND_TOP, p.rect.left, p.rect.top, p.rect.right-p.rect.left, p.rect.bottom-p.rect.top,
            SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    }
    while (source.tiles.size() > pieces.size()) { DestroyTile(source.tiles.back()); source.tiles.pop_back(); }
    return true;
}
inline bool Create(HWND hwnd, RECT area) {
    if (!CanSelect(hwnd)) { Say(L"目标窗口已不可用，无法创建区域。"); return false; }
    Source* source = FindSource(hwnd);
    bool first = !source;
    if (first) {
        Source state{}; state.window = hwnd; state.tid = GetWindowThreadProcessId(hwnd, &state.pid);
        state.layered = IsLayered(hwnd);
        if (state.layered && (!ReadAppearance(hwnd, &state.color, &state.alpha, &state.flags) || (state.flags & LWA_COLORKEY))) {
            Say(L"此窗口使用特殊分层或颜色键渲染，暂不支持独立局部透明度。"); return false;
        }
        if (!SetPropW(hwnd, marker, Token())) { Say(L"无法访问目标窗口。管理员窗口需要以管理员身份运行本工具。"); return false; }
        sources.push_back(state); source = &sources.back();
    }
    unsigned id = nextId++;
    views.push_back({hwnd,area,Dpi(hwnd),id,100});
    if (!Update(*source)) {
        views.pop_back();
        if (first) { if (Dispose(*source)) sources.pop_back(); }
        else Update(*source);
        Say(L"无法创建实时区域，原有区域保持不变。"); return false;
    }
    OpenManager(hwnd, id);
    Say(L"已添加局部区域。可在区域设置中分别调整 1–100% 不透明度。");
    return true;
}
inline int M(int value) { return static_cast<int>(value * managerScale + .5); }
inline View* SelectedView() {
    for (auto& view : views) if (view.id == selectedId && view.source == managedSource) return &view;
    return nullptr;
}
inline void FillValue() {
    if (!manager) return;
    auto view = SelectedView(); int value = view ? view->opacity : 100;
    SendDlgItemMessageW(manager, RegionSlider, TBM_SETPOS, TRUE, value);
    SetDlgItemTextW(manager, RegionEdit, std::to_wstring(value).c_str());
    SetDlgItemTextW(manager, RegionValue, view ? (L"区域 " + std::to_wstring(view->id) + L" · 不透明度 " + std::to_wstring(value) + L"%").c_str() : L"还没有区域，请点击“添加区域”框选。");
    for (int id : {RegionSlider,RegionEdit,RegionApply,RegionDelete}) EnableWindow(GetDlgItem(manager,id),view != nullptr);
}
inline void RefreshManager(unsigned preferred) {
    if (!manager) return;
    if (preferred) selectedId = preferred;
    HWND list = GetDlgItem(manager,RegionList); SendMessageW(list, CB_RESETCONTENT, 0, 0);
    int selected = 0;
    for (const auto& view : views) if (view.source == managedSource) {
        std::wstring label = L"区域 " + std::to_wstring(view.id) + L"  (" + std::to_wstring(view.area.left) + L", " +
            std::to_wstring(view.area.top) + L")  " + std::to_wstring(view.area.right-view.area.left) + L" × " +
            std::to_wstring(view.area.bottom-view.area.top);
        LRESULT index = SendMessageW(list,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
        if (index >= 0) { SendMessageW(list,CB_SETITEMDATA,index,view.id); if (view.id == selectedId) selected = static_cast<int>(index); }
    }
    SendMessageW(list,CB_SETCURSEL,selected,0);
    LRESULT index = SendMessageW(list,CB_GETCURSEL,0,0);
    selectedId = index == CB_ERR ? 0 : static_cast<unsigned>(SendMessageW(list,CB_GETITEMDATA,index,0));
    FillValue();
}
inline void ApplyRegionValue(int value) {
    auto view = SelectedView();
    if (!view || value < 1 || value > 100) { Say(L"区域不透明度请输入 1 到 100 之间的整数。"); return; }
    int old = view->opacity; view->opacity = value;
    auto source = FindSource(view->source);
    if (!source || !Update(*source)) { view->opacity = old; Say(L"区域透明度更新失败，请重试。"); }
    FillValue();
}
inline LRESULT CALLBACK ManagerProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case RegionList:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                LRESULT index = SendDlgItemMessageW(hwnd,RegionList,CB_GETCURSEL,0,0);
                if (index != CB_ERR) selectedId = static_cast<unsigned>(SendDlgItemMessageW(hwnd,RegionList,CB_GETITEMDATA,index,0));
                FillValue();
            }
            break;
        case RegionApply: {
            wchar_t text[16]{}; GetDlgItemTextW(hwnd,RegionEdit,text,16); int value = _wtoi(text);
            if (value < 1 || value > 100) MessageBoxW(hwnd,L"请输入 1 到 100 之间的整数。",L"区域不透明度",MB_ICONINFORMATION);
            else ApplyRegionValue(value);
            break;
        }
        case RegionDelete: {
            auto found = std::find_if(views.begin(),views.end(),[](const View& v) { return v.id == selectedId && v.source == managedSource; });
            if (found != views.end()) {
                View old = *found; auto index = found-views.begin(); views.erase(found);
                bool remaining = std::any_of(views.begin(),views.end(),[](const View& v) { return v.source == managedSource; });
                if (!remaining) {
                    if (!Clear(managedSource)) views.insert(views.begin()+index,old);
                } else if (auto source = FindSource(managedSource)) Update(*source);
                RefreshManager();
            }
            break;
        }
        case RegionAdd: Begin(managedSource); break;
        case RegionClear: Clear(managedSource); break;
        case IDCANCEL: DestroyWindow(hwnd); break;
        }
        return 0;
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lp) == GetDlgItem(hwnd,RegionSlider)) ApplyRegionValue(static_cast<int>(SendDlgItemMessageW(hwnd,RegionSlider,TBM_GETPOS,0,0)));
        return 0;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        manager = nullptr; managedSource = nullptr; selectedId = 0;
        if (managerFont) { DeleteObject(managerFont); managerFont = nullptr; }
        return 0;
    }
    return DefWindowProcW(hwnd,message,wp,lp);
}
inline void OpenManager(HWND hwnd, unsigned preferred) {
    if (!CanSelect(hwnd)) { Say(L"请先选择目标窗口。"); return; }
    managedSource = hwnd; managedTid = GetWindowThreadProcessId(hwnd,&managedPid);
    if (!manager) {
        managerScale = Dpi(hwnd)/96.0;
        RECT size{0,0,M(440),M(350)}; DWORD style = WS_CAPTION | WS_SYSMENU | WS_OVERLAPPED;
        AdjustWindowRectEx(&size,style,FALSE,WS_EX_TOPMOST | WS_EX_TOOLWINDOW);
        RECT frame{}; GetWindowRect(hwnd,&frame);
        MONITORINFO monitor{sizeof(monitor),{},{},0}; GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor);
        int x = std::clamp(static_cast<int>(frame.right)+12,static_cast<int>(monitor.rcWork.left),std::max(static_cast<int>(monitor.rcWork.left),static_cast<int>(monitor.rcWork.right)-static_cast<int>(size.right-size.left)));
        int y = std::clamp(static_cast<int>(frame.top),static_cast<int>(monitor.rcWork.top),std::max(static_cast<int>(monitor.rcWork.top),static_cast<int>(monitor.rcWork.bottom)-static_cast<int>(size.bottom-size.top)));
        manager = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,L"WindowOpacity.RegionSettings",L"局部区域设置",style,
            x,y,size.right-size.left,size.bottom-size.top,nullptr,nullptr,instance,nullptr);
        if (!manager) { Say(L"无法打开区域设置。"); return; }
        managerFont = CreateFontW(-M(15),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
        auto control = [](const wchar_t* cls,const wchar_t* text,DWORD flags,int x,int y,int width,int height,int id=0) {
            HWND child = CreateWindowExW(0,cls,text,WS_CHILD | WS_VISIBLE | flags,M(x),M(y),M(width),M(height),manager,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
            SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(managerFont),TRUE);
        };
        control(L"STATIC",L"选择区域，每个区域的透明度独立生效。",0,20,16,400,25);
        control(L"COMBOBOX",L"",CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,20,48,400,240,RegionList);
        control(L"STATIC",L"",0,20,92,400,24,RegionValue);
        control(TRACKBAR_CLASSW,L"",TBS_HORZ | TBS_NOTICKS | WS_TABSTOP,15,124,405,34,RegionSlider);
        SendDlgItemMessageW(manager,RegionSlider,TBM_SETRANGE,TRUE,MAKELPARAM(1,100));
        control(L"STATIC",L"1% 更透明",0,20,164,160,22);
        control(L"STATIC",L"100% 完全清晰",SS_RIGHT,240,164,180,22);
        control(L"EDIT",L"100",WS_BORDER | ES_NUMBER | ES_CENTER | WS_TABSTOP,20,201,70,30,RegionEdit);
        SendDlgItemMessageW(manager,RegionEdit,EM_SETLIMITTEXT,3,0);
        control(L"BUTTON",L"应用 %",WS_TABSTOP | BS_DEFPUSHBUTTON,100,200,90,32,RegionApply);
        control(L"BUTTON",L"删除此区域",WS_TABSTOP,275,200,145,32,RegionDelete);
        control(L"BUTTON",L"添加区域",WS_TABSTOP,20,248,190,34,RegionAdd);
        control(L"BUTTON",L"清除本窗口全部区域",WS_TABSTOP,220,248,200,34,RegionClear);
        control(L"STATIC",L"重叠处按最后添加的区域显示；其余部分保持窗口透明度。\n关闭此面板会保留区域，退出软件后恢复。",0,20,300,400,42);
    }
    wchar_t title[160]{}; GetWindowTextW(hwnd,title,160);
    SetWindowTextW(manager,(L"局部区域设置 · " + std::wstring(title)).c_str());
    RefreshManager(preferred); ShowWindow(manager,SW_SHOW); SetForegroundWindow(manager);
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
        DrawTextW(dc, L"拖动添加局部区域 · Esc / 右键取消", -1, &caption, DT_WORDBREAK | DT_NOPREFIX);
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
    for (auto& source : sources) Suspend(source);
    if (manager) ShowWindow(manager, SW_HIDE);
    ShowWindow(selector, SW_SHOW); SetForegroundWindow(selector); SetFocus(selector);
    Say(L"拖动添加区域，已有区域保留；按 Esc 或右键可取消。");
    return true;
}

inline void Tick() {
    if (selector) {
        RECT current{};
        if (!Identity(selectionSource,selectionPid,selectionTid) || !Visible(selectionSource) ||
            !ClientBounds(selectionSource,current) || !SameRect(current,originalClient)) CancelSelection();
    }
    for (auto it = sources.begin(); it != sources.end();) {
        if (!Update(*it)) {
            HWND hwnd = it->window;
            if (Dispose(*it)) {
                it = sources.erase(it);
                views.erase(std::remove_if(views.begin(),views.end(),[hwnd](const View& view) { return view.source == hwnd; }),views.end());
                if (manager && managedSource == hwnd) RefreshManager();
            } else ++it;
        } else ++it;
    }
    if (manager && !Identity(managedSource,managedPid,managedTid)) DestroyWindow(manager);
}
inline bool Initialize(HINSTANCE module, void (*reporter)(const std::wstring&)) {
    instance = module; report = reporter;
    WNDCLASSW cls{}; cls.hInstance = module; cls.lpfnWndProc = ViewProc;
    cls.lpszClassName = L"WindowOpacity.ClearRegion"; cls.hCursor = LoadCursorW(nullptr,IDC_ARROW);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (!RegisterClassW(&cls)) return false;
    cls.lpfnWndProc = SelectionProc; cls.lpszClassName = L"WindowOpacity.RegionSelector";
    cls.hCursor = LoadCursorW(nullptr,IDC_CROSS);
    if (!RegisterClassW(&cls)) return false;
    cls.lpfnWndProc = ManagerProc; cls.lpszClassName = L"WindowOpacity.RegionSettings";
    cls.hCursor = LoadCursorW(nullptr,IDC_ARROW); cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
    return RegisterClassW(&cls) != 0;
}
} // namespace ClearRegion
