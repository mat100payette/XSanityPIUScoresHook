#include "setup_view.h"
#include <dwmapi.h>
#include <algorithm>

namespace piu {
namespace {
void rounded(HDC dc, RECT rect, int radius, COLORREF fill, COLORREF border, int stroke = 1) {
    auto brush = CreateSolidBrush(fill);
    auto pen = CreatePen(PS_SOLID, stroke, border);
    auto old_brush = SelectObject(dc, brush), old_pen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void text(HDC dc, HFONT font, COLORREF color, std::wstring_view label, RECT rect, UINT format) {
    auto old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, label.data(), static_cast<int>(label.size()), &rect, format);
    SelectObject(dc, old);
}

int text_height(HWND window, HFONT font, std::wstring_view label, int width) {
    HDC dc = GetDC(window);
    RECT rect{0, 0, width, 0};
    auto old = SelectObject(dc, font);
    DrawTextW(
        dc, label.data(), static_cast<int>(label.size()), &rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(window, dc);
    return rect.bottom;
}

constexpr wchar_t SyncDescription[] = L"Automatically upload new personal bests to PIU Scores.";
constexpr wchar_t OverlayDescription[] = L"Current song, difficulty and your website PB in OBS.";
} // namespace

SetupView::~SetupView() {
    if (window_ && IsWindow(window_)) {
        for (const auto& control : interactions_) {
            RemoveWindowSubclass(GetDlgItem(window_, control.id), control_proc, 1);
        }
    }

    for (auto font : {body_, heading_, small_, title_}) {
        if (font) {
            DeleteObject(font);
        }
    }

    if (background_) {
        DeleteObject(background_);
    }

    if (panel_) {
        DeleteObject(panel_);
    }
}

int SetupView::scale(int value) const {
    return MulDiv(value, static_cast<int>(dpi_), 96);
}

void SetupView::theme() {
    HIGHCONTRASTW contrast{sizeof(contrast)};
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
    bool high_contrast = appearance_ == SetupAppearance::Contrast ||
                         (appearance_ == SetupAppearance::System && (contrast.dwFlags & HCF_HIGHCONTRASTON));
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &light,
        &size);
    bool dark = appearance_ == SetupAppearance::Dark || (appearance_ == SetupAppearance::System && !light);
    if (high_contrast) {
        auto background = GetSysColor(COLOR_WINDOW), foreground = GetSysColor(COLOR_WINDOWTEXT);
        auto accent = GetSysColor(COLOR_HIGHLIGHT), accent_text = GetSysColor(COLOR_HIGHLIGHTTEXT);
        theme_ = {background,
            background,
            foreground,
            foreground,
            foreground,
            accent,
            accent_text,
            background,
            background,
            foreground,
            false,
            true};
    } else if (dark) {
        theme_ = {RGB(16, 23, 32),
            RGB(24, 34, 47),
            RGB(242, 246, 251),
            RGB(174, 189, 206),
            RGB(57, 72, 90),
            RGB(36, 220, 205),
            RGB(5, 30, 33),
            RGB(22, 53, 57),
            RGB(34, 49, 65),
            RGB(255, 126, 143),
            true};
    } else {
        theme_ = {RGB(245, 247, 250),
            RGB(255, 255, 255),
            RGB(24, 35, 48),
            RGB(85, 102, 122),
            RGB(202, 212, 224),
            RGB(13, 174, 159),
            RGB(5, 30, 33),
            RGB(233, 248, 245),
            RGB(234, 239, 245),
            RGB(181, 37, 60)};
    }

    if (background_) {
        DeleteObject(background_);
    }

    if (panel_) {
        DeleteObject(panel_);
    }

    background_ = CreateSolidBrush(theme_.background);
    panel_ = CreateSolidBrush(theme_.panel);
    BOOL use_dark = theme_.dark;
    DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &use_dark, sizeof(use_dark));
    auto corners = theme_.contrast ? DWMWCP_DONOTROUND : DWMWCP_ROUND;
    DwmSetWindowAttribute(window_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
}

void SetupView::fonts() {
    auto make = [&](int height, int weight, const wchar_t* face) {
        auto font = CreateFontW(-scale(height),
            0,
            0,
            0,
            weight,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH,
            face);
        if (!font) {
            fail("Create setup font");
        }

        return font;
    };
    for (auto font : {body_, heading_, small_, title_}) {
        if (font) {
            DeleteObject(font);
        }
    }

    body_ = heading_ = small_ = title_ = nullptr;
    body_ = make(15, FW_NORMAL, L"Segoe UI");
    heading_ = make(15, FW_SEMIBOLD, L"Segoe UI");
    small_ = make(14, FW_NORMAL, L"Segoe UI");
    title_ = make(26, FW_SEMIBOLD, L"Segoe UI Variable Display");
    for (int id : {IDC_INTRO, IDC_ROOT, IDC_SYNC, IDC_OVERLAY, IDC_LAUNCH, IDC_BROWSE, IDC_APPLY, IDCANCEL}) {
        SendDlgItemMessageW(window_, id, WM_SETFONT, reinterpret_cast<WPARAM>(body_), FALSE);
    }

    for (int id : {IDC_COMPONENTS, IDC_DETAILS}) {
        SendDlgItemMessageW(window_, id, WM_SETFONT, reinterpret_cast<WPARAM>(small_), FALSE);
    }

    for (int id : {IDC_FOLDER_LABEL, IDC_STATUS}) {
        SendDlgItemMessageW(window_, id, WM_SETFONT, reinterpret_cast<WPARAM>(heading_), FALSE);
    }

    SendDlgItemMessageW(window_, IDC_TITLE, WM_SETFONT, reinterpret_cast<WPARAM>(title_), FALSE);
}

void SetupView::layout(const RECT* suggested) {
    int width = scale(560), margin = scale(28), inner = width - margin * 2;
    auto height = [&](HFONT font, std::wstring_view label, int available) {
        return text_height(window_, font, label, available);
    };
    int line = height(body_, L"Ag", inner), small_line = height(small_, L"Ag", inner);

    struct Placement {
        int id, x, y, width, height;
    };

    std::vector<Placement> placements;
    auto place = [&](int id, int x, int y, int w, int h) {
        placements.push_back({id, x, y, w, h});
    };
    place(IDC_TITLE,
        scale(100),
        scale(24),
        width - scale(128),
        height(title_, L"XSanity companion", width - scale(128)));
    place(IDC_INTRO, scale(101), scale(65), width - scale(129), line * 2);
    int y = scale(120);
    place(IDC_COMPONENTS, margin, y, inner, small_line);
    y += small_line + scale(12);
    int description = std::max(height(small_, SyncDescription, inner - scale(78)),
        height(small_, OverlayDescription, inner - scale(78)));
    int card_height = std::max(scale(78), scale(36) + line + description);
    place(IDC_SYNC, margin, y, inner, card_height);
    y += card_height + scale(12);
    place(IDC_OVERLAY, margin, y, inner, card_height);
    y += card_height + scale(22);
    place(IDC_FOLDER_LABEL, margin, y, inner, line);
    y += line + scale(10);
    input_ = {margin, y, margin + inner - scale(124), y + scale(42)};
    place(IDC_ROOT,
        input_.left + scale(13),
        y + (scale(42) - line - scale(2)) / 2,
        input_.right - input_.left - scale(26),
        line + scale(2));
    place(IDC_BROWSE, input_.right + scale(12), y, scale(112), scale(42));
    y += scale(42) + scale(18);
    place(IDC_LAUNCH, margin, y, inner, std::max(scale(30), line + scale(8)));
    y += std::max(scale(30), line + scale(8)) + scale(18);
    place(IDC_STATUS, margin, y, inner, line);
    y += line + scale(7);
    int detail_height = std::max(small_line * 2,
        height(small_, L"Both components and your saved account data will be removed.", inner));
    place(IDC_DETAILS, margin, y, inner, detail_height);
    y += detail_height + scale(14);
    progress_ = {margin, y, width - margin, y + scale(6)};
    y += scale(28);
    place(IDC_APPLY, width - margin - scale(completed_ ? 132 : 246), y, scale(132), scale(38));
    place(IDCANCEL, width - margin - scale(102), y, scale(102), scale(38));
    content_height_ = y + scale(38) + scale(24);

    RECT current{};
    GetWindowRect(window_, &current);
    auto monitor = MonitorFromRect(suggested ? suggested : &current, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_STYLE));
    DWORD extended = static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_EXSTYLE));
    UINT frame_dpi = GetDpiForWindow(window_);
    RECT frame{};
    AdjustWindowRectExForDpi(&frame, style & ~WS_VSCROLL, FALSE, extended, frame_dpi);
    int frame_height = frame.bottom - frame.top;
    viewport_height_ = std::min(
        content_height_, static_cast<int>(info.rcWork.bottom - info.rcWork.top) - frame_height - scale(16));
    viewport_height_ = std::max(scale(240), viewport_height_);
    bool scrollable = viewport_height_ < content_height_;
    SetWindowLongPtrW(window_, GWL_STYLE, (style & ~WS_VSCROLL) | (scrollable ? WS_VSCROLL : 0));
    RECT bounds{
        0, 0, width + (scrollable ? GetSystemMetricsForDpi(SM_CXVSCROLL, frame_dpi) : 0), viewport_height_};
    AdjustWindowRectExForDpi(&bounds, style & ~WS_VSCROLL, FALSE, extended, frame_dpi);
    int window_width = bounds.right - bounds.left, window_height = bounds.bottom - bounds.top;
    int x = suggested ? suggested->left : current.left;
    int top = suggested ? suggested->top : current.top;
    if (!IsWindowVisible(window_)) {
        x = info.rcWork.left + (info.rcWork.right - info.rcWork.left - window_width) / 2;
        top = info.rcWork.top + (info.rcWork.bottom - info.rcWork.top - window_height) / 2;
    }

    x = std::max(
        static_cast<int>(info.rcWork.left), std::min(x, static_cast<int>(info.rcWork.right) - window_width));
    top = std::max(static_cast<int>(info.rcWork.top),
        std::min(top, static_cast<int>(info.rcWork.bottom) - window_height));
    SetWindowPos(window_,
        nullptr,
        x,
        top,
        window_width,
        window_height,
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    scroll_ = std::clamp(scroll_, 0, content_height_ - viewport_height_);
    SCROLLINFO range{sizeof(range),
        SIF_RANGE | SIF_PAGE | SIF_POS,
        0,
        content_height_ - 1,
        static_cast<UINT>(viewport_height_),
        scroll_};
    SetScrollInfo(window_, SB_VERT, &range, TRUE);
    for (const auto& p : placements) {
        MoveWindow(GetDlgItem(window_, p.id), p.x, p.y - scroll_, p.width, p.height, FALSE);
    }

    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void SetupView::scroll_to(int position) {
    position = std::clamp(position, 0, content_height_ - viewport_height_);
    int delta = scroll_ - position;
    if (!delta) {
        return;
    }

    scroll_ = position;
    for (HWND control = GetWindow(window_, GW_CHILD); control; control = GetWindow(control, GW_HWNDNEXT)) {
        RECT rect{};
        GetWindowRect(control, &rect);
        MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&rect), 2);
        SetWindowPos(
            control, nullptr, rect.left, rect.top + delta, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    SetScrollPos(window_, SB_VERT, scroll_, TRUE);
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void SetupView::reveal(HWND control) {
    RECT rect{};
    GetWindowRect(control, &rect);
    MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&rect), 2);
    if (rect.top < scale(12)) {
        scroll_to(scroll_ + rect.top - scale(12));
    } else if (rect.bottom > viewport_height_ - scale(12)) {
        scroll_to(scroll_ + rect.bottom - viewport_height_ + scale(12));
    }
}

void SetupView::initialize(HWND window, SetupAppearance appearance, UINT dpi) {
    window_ = window;
    appearance_ = appearance;
    dpi_ = dpi ? dpi : GetDpiForWindow(window_);
    SetDialogDpiChangeBehavior(window_, DDC_DISABLE_ALL, DDC_DISABLE_ALL);
    for (HWND child = GetWindow(window_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        SetDialogControlDpiChangeBehavior(child,
            DCDC_DISABLE_FONT_UPDATE | DCDC_DISABLE_RELAYOUT,
            DCDC_DISABLE_FONT_UPDATE | DCDC_DISABLE_RELAYOUT);
    }

    auto edit = GetDlgItem(window_, IDC_ROOT);
    SetWindowLongPtrW(edit, GWL_STYLE, GetWindowLongPtrW(edit, GWL_STYLE) & ~WS_BORDER);
    SetWindowLongPtrW(edit, GWL_EXSTYLE, GetWindowLongPtrW(edit, GWL_EXSTYLE) & ~WS_EX_CLIENTEDGE);
    SendMessageW(edit, EM_SETLIMITTEXT, 32767, 0);
    for (const auto& control : interactions_) {
        if (!SetWindowSubclass(
                GetDlgItem(window_, control.id), control_proc, 1, reinterpret_cast<DWORD_PTR>(this))) {
            fail("Style setup control");
        }
    }

    auto icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP));
    SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
    SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
    theme();
    fonts();
    layout();
}

void SetupView::paint(HDC dc) {
    RECT client{};
    GetClientRect(window_, &client);
    FillRect(dc, &client, background_);
    POINT old{};
    SetViewportOrgEx(dc, 0, -scroll_, &old);
    RECT accent{0, 0, scale(560), scale(3)};
    auto brush = CreateSolidBrush(theme_.accent);
    FillRect(dc, &accent, brush);
    DeleteObject(brush);
    if (!theme_.contrast) {
        auto icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(IDI_APP),
            IMAGE_ICON,
            scale(56),
            scale(56),
            LR_SHARED));
        DrawIconEx(dc, scale(28), scale(25), icon, scale(56), scale(56), 0, nullptr, DI_NORMAL);
    }

    bool enabled = IsWindowEnabled(GetDlgItem(window_, IDC_ROOT)) != FALSE;
    rounded(dc,
        input_,
        scale(12),
        enabled ? theme_.panel : theme_.background,
        GetFocus() == GetDlgItem(window_, IDC_ROOT) ? theme_.accent : theme_.border,
        GetFocus() == GetDlgItem(window_, IDC_ROOT) ? scale(2) : 1);
    if (working_ || completed_ || failed_) {
        rounded(dc, progress_, scale(6), theme_.border, theme_.border);
        RECT filled = progress_;
        filled.right = filled.left + MulDiv(filled.right - filled.left, progress_value_, 100);
        if (filled.right > filled.left) {
            rounded(dc,
                filled,
                scale(6),
                failed_ ? theme_.danger : theme_.accent,
                failed_ ? theme_.danger : theme_.accent);
        }
    }

    SetViewportOrgEx(dc, old.x, old.y, nullptr);
}

void SetupView::paint_control(HWND control, HDC dc) {
    int id = GetDlgCtrlID(control);
    RECT rect{};
    GetClientRect(control, &rect);
    auto state = std::find_if(interactions_.begin(), interactions_.end(), [id](const auto& item) {
        return item.id == id;
    });
    bool enabled = IsWindowEnabled(control) != FALSE, focus = GetFocus() == control;
    bool hot = enabled && state != interactions_.end() && state->hot;
    bool pressed = enabled && (SendMessageW(control, BM_GETSTATE, 0, 0) & BST_PUSHED);
    UINT format = DT_SINGLELINE | DT_VCENTER |
                  ((SendMessageW(control, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL) ? DT_HIDEPREFIX : 0);
    COLORREF foreground = enabled ? theme_.text : theme_.muted;
    FillRect(dc, &rect, background_);
    if (id == IDC_SYNC || id == IDC_OVERLAY || id == IDC_LAUNCH) {
        bool selected = SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
        bool card = id != IDC_LAUNCH;
        if (card) {
            rounded(dc,
                rect,
                scale(16),
                selected ? theme_.selected : (hot ? theme_.hover : theme_.panel),
                (selected || hot || focus) ? theme_.accent : theme_.border,
                focus ? scale(2) : 1);
        }

        int left = card ? scale(18) : 1, top = card ? scale(18) : (rect.bottom - scale(18)) / 2;
        RECT checkbox{left, top, left + scale(18), top + scale(18)};
        rounded(dc,
            checkbox,
            scale(6),
            selected ? theme_.accent : theme_.panel,
            selected ? theme_.accent : theme_.border);
        if (selected) {
            auto pen = CreatePen(PS_SOLID, std::max(2, scale(2)), theme_.accent_text);
            auto old = SelectObject(dc, pen);
            MoveToEx(dc, left + scale(4), top + scale(9), nullptr);
            LineTo(dc, left + scale(8), top + scale(13));
            LineTo(dc, left + scale(14), top + scale(5));
            SelectObject(dc, old);
            DeleteObject(pen);
        }

        RECT label{
            left + scale(30), card ? scale(14) : 0, rect.right - scale(18), card ? scale(38) : rect.bottom};
        std::wstring caption = id == IDC_OVERLAY ? L"&OBS overlay" : control_text(window_, id);
        if (id == IDC_OVERLAY) {
            RECT badge{rect.right - scale(91), scale(17), rect.right - scale(16), scale(37)};
            rounded(dc, badge, scale(10), theme_.hover, theme_.hover);
            text(dc,
                small_,
                theme_.muted,
                L"Optional",
                badge,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            label.right = badge.left - scale(8);
        }

        text(dc, card ? heading_ : body_, foreground, caption, label, format);
        if (card) {
            RECT description{left + scale(30), scale(41), rect.right - scale(18), rect.bottom - scale(12)};
            text(dc,
                small_,
                theme_.muted,
                id == IDC_SYNC ? SyncDescription : OverlayDescription,
                description,
                DT_WORDBREAK | DT_NOPREFIX);
        }

        if (focus) {
            RECT ring = rect;
            InflateRect(&ring, -scale(4), -scale(4));
            DrawFocusRect(dc, &ring);
        }
    } else {
        bool primary = id == IDC_APPLY, danger = primary && removing_ && !completed_;
        auto fill = primary ? (danger ? theme_.danger : theme_.accent) : theme_.panel;
        if (!enabled) {
            fill = theme_.border;
        } else if (pressed) {
            fill = primary
                       ? (danger || theme_.contrast ? fill
                                                    : (theme_.dark ? RGB(20, 179, 168) : RGB(11, 152, 139)))
                       : theme_.selected;
        } else if (hot) {
            fill = primary
                       ? (danger || theme_.contrast ? fill
                                                    : (theme_.dark ? RGB(65, 234, 219) : RGB(31, 194, 177)))
                       : theme_.hover;
        }

        auto label_color =
            primary ? (danger ? (theme_.dark || theme_.contrast ? theme_.background : RGB(255, 255, 255))
                              : theme_.accent_text)
                    : foreground;
        if (!enabled) {
            label_color = theme_.muted;
        }

        rounded(dc,
            rect,
            scale(10),
            fill,
            (focus || hot) ? theme_.accent : (primary ? fill : theme_.border),
            focus ? scale(2) : 1);
        RECT label = rect;
        if (pressed) {
            OffsetRect(&label, 0, scale(1));
        }

        text(dc, body_, label_color, control_text(window_, id), label, format | DT_CENTER);
        if (focus) {
            RECT ring = rect;
            InflateRect(&ring, -scale(4), -scale(4));
            DrawFocusRect(dc, &ring);
        }
    }
}

LRESULT CALLBACK SetupView::control_proc(
    HWND control, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data) {
    auto view = reinterpret_cast<SetupView*>(data);
    int id = GetDlgCtrlID(control);
    auto state = std::find_if(view->interactions_.begin(), view->interactions_.end(), [id](const auto& item) {
        return item.id == id;
    });
    if (id != IDC_ROOT && (message == WM_PAINT || message == WM_PRINTCLIENT)) {
        PAINTSTRUCT paint{};
        HDC dc = message == WM_PAINT ? BeginPaint(control, &paint) : reinterpret_cast<HDC>(wparam);
        view->paint_control(control, dc);
        if (message == WM_PAINT) {
            EndPaint(control, &paint);
        }

        return 0;
    }

    if (id != IDC_ROOT && message == WM_ERASEBKGND) {
        return 1;
    }

    if (message == WM_MOUSEMOVE && IsWindowEnabled(control) && state != view->interactions_.end() &&
        !state->hot) {
        state->hot = true;
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, control, 0};
        TrackMouseEvent(&tracking);
        InvalidateRect(control, nullptr, FALSE);
    }

    if ((message == WM_MOUSELEAVE || message == WM_ENABLE) && state != view->interactions_.end()) {
        state->hot = false;
    }

    if (message == WM_SETFOCUS) {
        view->reveal(control);
    }

    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(control, control_proc, 1);
    }

    LRESULT result = DefSubclassProc(control, message, wparam, lparam);
    if (message == BM_SETCHECK || message == BM_SETSTATE || message == WM_MOUSELEAVE ||
        message == WM_ENABLE || message == WM_SETFOCUS || message == WM_KILLFOCUS ||
        message == WM_UPDATEUISTATE || message == WM_LBUTTONUP || message == WM_KEYUP ||
        message == WM_CAPTURECHANGED) {
        InvalidateRect(control, nullptr, FALSE);
        if (id == IDC_ROOT) {
            InvalidateRect(view->window_, nullptr, FALSE);
        }
    }

    return result;
}

std::optional<INT_PTR> SetupView::message(UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_ERASEBKGND) {
        return TRUE;
    }

    if (message == WM_PAINT) {
        PAINTSTRUCT info{};
        auto dc = BeginPaint(window_, &info);
        RECT client{};
        GetClientRect(window_, &client);
        auto memory = CreateCompatibleDC(dc);
        auto bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
        auto old = SelectObject(memory, bitmap);
        paint(memory);
        BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
        SelectObject(memory, old);
        DeleteObject(bitmap);
        DeleteDC(memory);
        EndPaint(window_, &info);
        return TRUE;
    }

    if (message == WM_PRINTCLIENT) {
        paint(reinterpret_cast<HDC>(wparam));
        return TRUE;
    }

    if (message == WM_CTLCOLORDLG) {
        return reinterpret_cast<INT_PTR>(background_);
    }

    if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT) {
        auto dc = reinterpret_cast<HDC>(wparam);
        auto control = reinterpret_cast<HWND>(lparam);
        int id = GetDlgCtrlID(control);
        bool muted =
            id == IDC_INTRO || id == IDC_COMPONENTS || id == IDC_DETAILS || !IsWindowEnabled(control);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, failed_ && id == IDC_STATUS ? theme_.danger : (muted ? theme_.muted : theme_.text));
        SetBkColor(dc, id == IDC_ROOT ? theme_.panel : theme_.background);
        return reinterpret_cast<INT_PTR>(id == IDC_ROOT && IsWindowEnabled(control) ? panel_ : background_);
    }

    if (message == WM_THEMECHANGED || message == WM_SETTINGCHANGE || message == WM_SYSCOLORCHANGE) {
        theme();
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        return FALSE;
    }

    if (message == WM_DPICHANGED) {
        auto old_dpi = dpi_;
        dpi_ = HIWORD(wparam);
        scroll_ = MulDiv(scroll_, static_cast<int>(dpi_), static_cast<int>(old_dpi));
        fonts();
        layout(reinterpret_cast<const RECT*>(lparam));
        return TRUE;
    }

    if (message == WM_VSCROLL) {
        SCROLLINFO info{sizeof(info), SIF_TRACKPOS};
        GetScrollInfo(window_, SB_VERT, &info);
        int position = scroll_;
        switch (LOWORD(wparam)) {
        case SB_LINEUP:
            position -= scale(32);
            break;
        case SB_LINEDOWN:
            position += scale(32);
            break;
        case SB_PAGEUP:
            position -= viewport_height_;
            break;
        case SB_PAGEDOWN:
            position += viewport_height_;
            break;
        case SB_THUMBTRACK:
            position = info.nTrackPos;
            break;
        case SB_TOP:
            position = 0;
            break;
        case SB_BOTTOM:
            position = content_height_;
            break;
        default:
            break;
        }

        scroll_to(position);
        return TRUE;
    }

    if (message == WM_MOUSEWHEEL) {
        scroll_to(scroll_ - MulDiv(GET_WHEEL_DELTA_WPARAM(wparam), scale(72), WHEEL_DELTA));
        return TRUE;
    }

    return std::nullopt;
}

void SetupView::refresh(bool installed, bool discards_pending) {
    bool keep = IsDlgButtonChecked(window_, IDC_SYNC) == BST_CHECKED ||
                IsDlgButtonChecked(window_, IDC_OVERLAY) == BST_CHECKED;
    working_ = completed_ = failed_ = false;
    removing_ = !keep && installed;
    progress_value_ = 0;
    for (int id : {IDC_SYNC, IDC_OVERLAY, IDCANCEL}) {
        EnableWindow(GetDlgItem(window_, id), TRUE);
    }

    for (int id : {IDC_ROOT, IDC_BROWSE, IDC_LAUNCH}) {
        EnableWindow(GetDlgItem(window_, id), keep);
    }

    ShowWindow(GetDlgItem(window_, IDCANCEL), SW_SHOW);
    SetDlgItemTextW(window_,
        IDC_INTRO,
        installed ? L"Add, change or remove your installed components."
                  : L"Your personal bests, connected to PIU Scores.");
    SetDlgItemTextW(
        window_, IDC_APPLY, keep ? (installed ? L"&Apply changes" : L"&Install") : L"&Remove all");
    EnableWindow(GetDlgItem(window_, IDC_APPLY), keep || installed);
    SetDlgItemTextW(window_,
        IDC_STATUS,
        removing_ ? L"Ready to remove" : (installed ? L"Ready to apply changes" : L"Ready to install"));
    SetDlgItemTextW(window_,
        IDC_DETAILS,
        !keep ? (installed ? L"Both components and your saved account data will be removed."
                           : L"Choose at least one component to continue.")
              : (discards_pending ? L"Removing PB syncing discards pending uploads. Your account settings "
                                    L"stay with the overlay."
                                  : L"You can add or remove either component anytime by reopening setup."));
    layout();
}

void SetupView::working(bool removing, bool installed) {
    working_ = true;
    completed_ = failed_ = false;
    removing_ = removing;
    progress_value_ = 0;
    for (int id : {IDC_SYNC, IDC_OVERLAY, IDC_ROOT, IDC_BROWSE, IDC_LAUNCH, IDC_APPLY, IDCANCEL}) {
        EnableWindow(GetDlgItem(window_, id), FALSE);
    }

    SetDlgItemTextW(
        window_, IDC_APPLY, removing ? L"Removing..." : (installed ? L"Applying..." : L"Installing..."));
    SetDlgItemTextW(window_, IDC_DETAILS, L"Please keep this window open while setup finishes.");
    progress(0, L"Checking your setup");
}

void SetupView::progress(int percent, std::wstring_view status) {
    progress_value_ = std::clamp(percent, 0, 100);
    SetDlgItemTextW(window_, IDC_STATUS, std::wstring(status).c_str());
    InvalidateRect(window_, nullptr, FALSE);
}

void SetupView::finish(bool keep) {
    working_ = failed_ = false;
    completed_ = true;
    progress_value_ = 100;
    for (int id : {IDC_SYNC, IDC_OVERLAY, IDC_APPLY}) {
        EnableWindow(GetDlgItem(window_, id), TRUE);
    }

    for (int id : {IDC_ROOT, IDC_BROWSE, IDC_LAUNCH}) {
        EnableWindow(GetDlgItem(window_, id), keep);
    }

    ShowWindow(GetDlgItem(window_, IDCANCEL), SW_HIDE);
    SetDlgItemTextW(window_, IDC_APPLY, L"&Done");
    SetDlgItemTextW(window_, IDC_STATUS, keep ? L"You're all set" : L"Removal complete");
    SetDlgItemTextW(window_,
        IDC_DETAILS,
        keep ? L"Use Play XSanity in the Start menu to launch the game and companion together."
             : L"The companion, game connection and saved account data were removed.");
    layout();
    SetFocus(GetDlgItem(window_, IDC_APPLY));
}

void SetupView::failure() {
    working_ = completed_ = false;
    failed_ = true;
    progress_value_ = 100;
    SetDlgItemTextW(window_, IDC_STATUS, L"Setup could not finish");
    SetDlgItemTextW(window_, IDC_DETAILS, L"Review the error, make any needed changes, and try again.");
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}
} // namespace piu
