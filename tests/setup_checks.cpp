#include "support.h"
#include "setup_view.h"
#include "preview.h"
#include <oleacc.h>

namespace piu::test {
bool icon_size(HICON icon, int width, int height) {
    ICONINFO info{};
    if (!GetIconInfo(icon, &info)) {
        fail("Inspect icon");
    }

    BITMAP bitmap{};
    bool valid = info.hbmColor && GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap) &&
                 bitmap.bmWidth == width && bitmap.bmHeight == height;
    DeleteObject(info.hbmColor);
    DeleteObject(info.hbmMask);
    return valid;
}

void icon_checks() {
    for (int resource : {IDI_APP, IDI_SETUP}) {
        Icon icon;
        bool valid = true;
        for (int size : {16, 20, 24, 28, 32, 40, 48, 56, 64, 72, 84, 96, 112, 128, 144, 192, 256}) {
            icon.load(resource, size, size);
            valid = valid && icon_size(icon.get(), size, size);
        }

        check(valid, "embedded app and setup icons load at tray, title and header sizes");
    }
}

struct Preview {
    SetupView view;
    HWND window = nullptr;
    SetupAppearance appearance;
    UINT dpi;
    bool installed = false;
    std::string error;

    explicit Preview(SetupAppearance style, UINT scale = 96) : appearance(style), dpi(scale) {
        window = CreateDialogParamW(
            GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(IDD_SETUP),
            nullptr,
            [](HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) -> INT_PTR {
                auto preview = reinterpret_cast<Preview*>(GetWindowLongPtrW(dialog, DWLP_USER));
                if (message == WM_INITDIALOG) {
                    preview = reinterpret_cast<Preview*>(lparam);
                    SetWindowLongPtrW(dialog, DWLP_USER, lparam);
                    try {
                        preview->view.initialize(dialog, preview->appearance, preview->dpi);
                        SetDlgItemTextW(dialog, IDC_ROOT, L"C:\\XSanity");
                        CheckDlgButton(dialog, IDC_SYNC, BST_CHECKED);
                        CheckDlgButton(dialog, IDC_LAUNCH, BST_CHECKED);
                        preview->view.refresh(false);
                    } catch (...) {
                        preview->error = exception_message();
                    }

                    return TRUE;
                }

                if (preview) {
                    if (auto result = preview->view.message(message, wparam, lparam)) {
                        return *result;
                    }

                    if (message == WM_COMMAND && HIWORD(wparam) == BN_CLICKED &&
                        (LOWORD(wparam) == IDC_SYNC || LOWORD(wparam) == IDC_OVERLAY)) {
                        preview->view.refresh(preview->installed);
                        return TRUE;
                    }
                }

                return FALSE;
            },
            reinterpret_cast<LPARAM>(this));
        if (!window || !error.empty()) {
            throw Error(error.empty() ? "Could not create setup preview." : error);
        }
    }

    ~Preview() {
        if (window) {
            DestroyWindow(window);
        }
    }

    void save(const fs::path& path, bool client_only = false) {
        save_window_preview(window, path, client_only);
    }
};

void view_removal_transition_checks() {
    struct RedrawAudit {
        bool paused = false;
        int changes = 0, intermediate = 0;

        static LRESULT CALLBACK observe(
            HWND control, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data) {
            auto audit = reinterpret_cast<RedrawAudit*>(data);
            if (message == WM_SETREDRAW) {
                audit->paused = !wparam;
            } else if (message == WM_ENABLE || message == WM_SETTEXT) {
                ++audit->changes;
                if (!audit->paused) {
                    ++audit->intermediate;
                }
            }

            return DefSubclassProc(control, message, wparam, lparam);
        }
    };

    for (auto appearance : {SetupAppearance::Dark, SetupAppearance::Light, SetupAppearance::Contrast}) {
        for (bool installed : {false, true}) {
            Preview preview(appearance);
            preview.installed = installed;
            preview.view.refresh(installed);
            auto dialog = preview.window;
            constexpr std::array<int, 5> Controls{IDC_ROOT, IDC_BROWSE, IDC_APPLY, IDC_STATUS, IDC_DETAILS};
            std::array<RedrawAudit, Controls.size()> audits;
            for (size_t index = 0; index < Controls.size(); ++index) {
                check(SetWindowSubclass(GetDlgItem(dialog, Controls[index]),
                          RedrawAudit::observe,
                          3,
                          reinterpret_cast<DWORD_PTR>(&audits[index])) != FALSE,
                    "observe native redraw state during component transitions");
            }

            auto field = GetDlgItem(dialog, IDC_ROOT);
            auto dc = GetDC(field);
            for (bool keep : {false, true, false, true}) {
                SendDlgItemMessageW(dialog, IDC_SYNC, BM_CLICK, 0, 0);
                check(control_text(dialog, IDC_APPLY) ==
                              (keep ? (installed ? L"&Apply changes" : L"&Install") : L"&Remove all") &&
                          (IsWindowEnabled(GetDlgItem(dialog, IDC_APPLY)) != FALSE) == (keep || installed),
                    "real checkbox notifications update removal caption and action availability");
                check((IsWindowEnabled(field) != FALSE) == keep &&
                          (IsWindowEnabled(GetDlgItem(dialog, IDC_BROWSE)) != FALSE) == keep &&
                          (IsWindowEnabled(GetDlgItem(dialog, IDC_LAUNCH)) != FALSE) == keep,
                    "entering and leaving removal updates all folder and launch controls");

                auto brush = reinterpret_cast<HBRUSH>(SendMessageW(dialog,
                    keep ? WM_CTLCOLOREDIT : WM_CTLCOLORSTATIC,
                    reinterpret_cast<WPARAM>(dc),
                    reinterpret_cast<LPARAM>(field)));
                LOGBRUSH background{};
                GetObjectW(brush, sizeof(background), &background);
                check(GetBkColor(dc) == background.lbColor,
                    "enabled and disabled folder text uses the same background as its returned brush");
            }

            bool batched = true;
            int changes = 0;
            for (size_t index = 0; index < Controls.size(); ++index) {
                auto control = GetDlgItem(dialog, Controls[index]);
                batched &= !audits[index].paused && audits[index].intermediate == 0 &&
                           (GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0;
                changes += audits[index].changes;
                RemoveWindowSubclass(control, RedrawAudit::observe, 3);
            }

            check(changes >= 12 && batched,
                "removal transitions suppress intermediate native redraws and restore every child");
            check((GetWindowLongPtrW(dialog, GWL_STYLE) & WS_VISIBLE) == 0 &&
                      (GetWindowLongPtrW(dialog, GWL_EXSTYLE) & WS_EX_COMPOSITED) != 0,
                "batched composited updates do not reveal a hidden installer preview");
            ReleaseDC(field, dc);
            if (!installed && appearance == SetupAppearance::Dark) {
                SendDlgItemMessageW(dialog, IDC_SYNC, BM_CLICK, 0, 0);
                preview.save(executable().parent_path() / L"setup-none-client.png", true);
                SendDlgItemMessageW(dialog, IDC_SYNC, BM_CLICK, 0, 0);
                preview.save(executable().parent_path() / L"setup-restored-client.png", true);
            }
        }
    }
}

void view_input_visibility_checks() {
    struct VisibilityAudit {
        HWND dialog;
        int observations = 0;
        bool visible = true;

        static LRESULT CALLBACK observe(
            HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data) {
            auto audit = reinterpret_cast<VisibilityAudit*>(data);
            auto result = DefSubclassProc(window, message, wparam, lparam);
            if (message == WM_SETREDRAW || message == WM_ENABLE || message == WM_SETTEXT) {
                ++audit->observations;
                audit->visible &= IsWindowVisible(audit->dialog) != FALSE;
            }

            return result;
        }
    };

    for (auto appearance : {SetupAppearance::Dark, SetupAppearance::Light, SetupAppearance::Contrast}) {
        Preview preview(appearance);
        auto dialog = preview.window;
        // Exercise a visible window without moving the user's pointer or covering their desktop.
        SetWindowPos(dialog,
            nullptr,
            GetSystemMetrics(SM_XVIRTUALSCREEN) - 5000,
            GetSystemMetrics(SM_YVIRTUALSCREEN) - 5000,
            0,
            0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(dialog, SW_SHOWNOACTIVATE);
        check(IsWindowVisible(dialog) != FALSE, "input regression uses a visible installer");

        VisibilityAudit audit{dialog};
        std::vector<HWND> windows{dialog};
        for (HWND child = GetWindow(dialog, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            windows.push_back(child);
        }

        for (auto window : windows) {
            if (!SetWindowSubclass(
                    window, VisibilityAudit::observe, 4, reinterpret_cast<DWORD_PTR>(&audit))) {
                fail("Observe setup input visibility");
            }
        }

        for (bool installed : {false, true}) {
            preview.installed = installed;
            // Include ordinary toggles and repeated transitions into and out of Remove all.
            for (int id : {IDC_OVERLAY, IDC_SYNC, IDC_OVERLAY, IDC_SYNC, IDC_SYNC, IDC_SYNC}) {
                SendDlgItemMessageW(dialog, id, BM_CLICK, 0, 0);
            }
        }

        for (auto window : windows) {
            RemoveWindowSubclass(window, VisibilityAudit::observe, 4);
        }

        check(audit.observations > 0 && audit.visible && IsWindowVisible(dialog),
            "installer stays visible to input throughout checkbox redraws, including Remove all");
    }
}

void view_repaint_checks() {
    struct Messages {
        int layouts = 0, enables = 0;

        static LRESULT CALLBACK observe(
            HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data) {
            auto counts = reinterpret_cast<Messages*>(data);
            if (message == WM_WINDOWPOSCHANGING) {
                ++counts->layouts;
            } else if (message == WM_ENABLE) {
                ++counts->enables;
            }

            return DefSubclassProc(window, message, wparam, lparam);
        }
    };

    for (auto appearance : {SetupAppearance::Dark, SetupAppearance::Light, SetupAppearance::Contrast}) {
        Preview preview(appearance);
        auto dialog = preview.window;
        auto browse = GetDlgItem(dialog, IDC_BROWSE);
        Messages counts;
        SetWindowSubclass(dialog, Messages::observe, 2, reinterpret_cast<DWORD_PTR>(&counts));
        SetWindowSubclass(browse, Messages::observe, 2, reinterpret_cast<DWORD_PTR>(&counts));
        for (int id : {IDC_OVERLAY, IDC_SYNC, IDC_LAUNCH}) {
            SendDlgItemMessageW(dialog, id, BM_CLICK, 0, 0);
            preview.view.refresh(false);
            SendDlgItemMessageW(dialog, id, BM_CLICK, 0, 0);
            preview.view.refresh(false);
        }

        // Sync toggles disable Browse when no components remain; overlay/launch do not.
        check(counts.layouts == 0 && counts.enables == 2 && IsWindowEnabled(browse),
            "checkbox changes avoid relayout and only change Browse availability when necessary");
        check((GetWindowLongPtrW(dialog, GWL_STYLE) & WS_CLIPCHILDREN) != 0,
            "dialog background redraws exclude child controls");
        RemoveWindowSubclass(browse, Messages::observe, 2);
        RemoveWindowSubclass(dialog, Messages::observe, 2);

        RECT rect{};
        GetClientRect(browse, &rect);
        auto screen = GetDC(browse);
        auto memory = CreateCompatibleDC(screen);
        auto bitmap = CreateCompatibleBitmap(screen, rect.right, rect.bottom);
        auto old = SelectObject(memory, bitmap);
        auto sentinel = RGB(201, 17, 163);
        SetPixelV(memory, 12, 8, sentinel);
        NMCUSTOMDRAW draw{};
        draw.hdr = {browse, IDC_BROWSE, NM_CUSTOMDRAW};
        draw.hdc = memory;
        draw.rc = rect;
        draw.dwDrawStage = CDDS_PREERASE;
        SendMessageW(dialog, WM_NOTIFY, IDC_BROWSE, reinterpret_cast<LPARAM>(&draw));
        check(GetWindowLongPtrW(dialog, DWLP_MSGRESULT) == CDRF_SKIPDEFAULT &&
                  GetPixel(memory, 12, 8) == sentinel,
            "native button erase notifications cannot flash the system background");
        draw.dwDrawStage = CDDS_PREPAINT;
        SendMessageW(dialog, WM_NOTIFY, IDC_BROWSE, reinterpret_cast<LPARAM>(&draw));
        auto panel = appearance == SetupAppearance::Dark    ? RGB(24, 34, 47)
                     : appearance == SetupAppearance::Light ? RGB(255, 255, 255)
                                                            : GetSysColor(COLOR_WINDOW);
        check(
            GetWindowLongPtrW(dialog, DWLP_MSGRESULT) == CDRF_SKIPDEFAULT && GetPixel(memory, 12, 8) == panel,
            "native button state redraws use the styled palette in each appearance");

        auto apply = GetDlgItem(dialog, IDC_APPLY);
        RECT ready{}, finished{}, restored{};
        GetWindowRect(apply, &ready);
        preview.view.finish(true);
        GetWindowRect(apply, &finished);
        preview.view.refresh(true);
        GetWindowRect(apply, &restored);
        check(ready.left != finished.left && EqualRect(&ready, &restored),
            "editing after completion restores the action-row layout");
        SelectObject(memory, old);
        DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(browse, screen);
    }
}

void view_checks() {
    view_input_visibility_checks();
    view_removal_transition_checks();
    view_repaint_checks();
    Preview preview(SetupAppearance::Dark);
    auto dialog = preview.window;
    check(IsDlgButtonChecked(dialog, IDC_SYNC) == BST_CHECKED &&
              IsDlgButtonChecked(dialog, IDC_OVERLAY) == BST_UNCHECKED,
        "polished setup preserves optional-overlay defaults");
    auto overlay = GetDlgItem(dialog, IDC_OVERLAY);
    SendMessageW(overlay, BM_CLICK, 0, 0);
    check(IsDlgButtonChecked(dialog, IDC_OVERLAY) == BST_CHECKED,
        "whole component card retains native checkbox behavior");
    winrt::com_ptr<IAccessible> accessible;
    winrt::check_hresult(AccessibleObjectFromWindow(
        overlay, static_cast<DWORD>(OBJID_CLIENT), IID_IAccessible, accessible.put_void()));
    VARIANT self{};
    self.vt = VT_I4;
    self.lVal = CHILDID_SELF;
    VARIANT role{}, state{};
    winrt::check_hresult(accessible->get_accRole(self, &role));
    winrt::check_hresult(accessible->get_accState(self, &state));
    check(role.vt == VT_I4 && role.lVal == ROLE_SYSTEM_CHECKBUTTON && state.vt == VT_I4 &&
              (state.lVal & STATE_SYSTEM_CHECKED),
        "styled cards expose native accessible checkbox role and checked state");
    BSTR name = nullptr;
    winrt::check_hresult(accessible->get_accName(self, &name));
    bool named = name && std::wstring_view(name).find(L"OBS overlay") != std::wstring_view::npos;
    SysFreeString(name);
    check(named, "styled component keeps its accessible name");
    CheckDlgButton(dialog, IDC_OVERLAY, BST_UNCHECKED);
    SetFocus(overlay);
    SendMessageW(overlay, WM_KEYDOWN, VK_SPACE, 0);
    SendMessageW(overlay, WM_KEYUP, VK_SPACE, 0);
    check(
        IsDlgButtonChecked(dialog, IDC_OVERLAY) == BST_CHECKED, "space key toggles a focused component card");
    RECT hit{};
    GetClientRect(overlay, &hit);
    LPARAM point = MAKELPARAM(hit.right - 12, hit.bottom - 12);
    SendMessageW(overlay, WM_LBUTTONDOWN, MK_LBUTTON, point);
    SendMessageW(overlay, WM_LBUTTONUP, 0, point);
    check(IsDlgButtonChecked(dialog, IDC_OVERLAY) == BST_UNCHECKED,
        "clicking the card outside its glyph and title toggles the component");
    preview.view.working(false, false);
    bool locked = true;
    for (int id : {IDC_SYNC, IDC_OVERLAY, IDC_ROOT, IDC_BROWSE, IDC_LAUNCH, IDC_APPLY, IDCANCEL}) {
        locked &= !IsWindowEnabled(GetDlgItem(dialog, id));
    }

    check(locked, "working setup disables actions that could interrupt a transaction");
    preview.view.progress(60, L"Installing the companion");
    check(control_text(dialog, IDC_STATUS) == L"Installing the companion",
        "progress displays the current installation phase");
    preview.view.finish(true);
    check(control_text(dialog, IDC_APPLY) == L"&Done" && IsWindowEnabled(GetDlgItem(dialog, IDC_APPLY)),
        "success gives one enabled finish action");
    CheckDlgButton(dialog, IDC_SYNC, BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_OVERLAY, BST_UNCHECKED);
    preview.view.refresh(true);
    check(control_text(dialog, IDC_APPLY) == L"&Remove all" &&
              IsWindowEnabled(GetDlgItem(dialog, IDC_APPLY)) &&
              !IsWindowEnabled(GetDlgItem(dialog, IDC_ROOT)),
        "maintenance supports full removal and disables irrelevant folder input");
    check(control_text(dialog, IDC_DETAILS).find(L"saved account data") != std::wstring::npos,
        "full-removal consequences are visible before applying");
    preview.view.refresh(false);
    check(
        !IsWindowEnabled(GetDlgItem(dialog, IDC_APPLY)), "fresh setup cannot remove an absent installation");
    CheckDlgButton(dialog, IDC_OVERLAY, BST_CHECKED);
    preview.view.refresh(true, true);
    check(control_text(dialog, IDC_DETAILS).find(L"pending uploads") != std::wstring::npos &&
              IsWindowEnabled(GetDlgItem(dialog, IDC_ROOT)),
        "overlay-only maintenance explains discarded pending uploads");
    preview.view.failure();
    preview.view.refresh(true);
    check(IsWindowEnabled(GetDlgItem(dialog, IDCANCEL)) &&
              control_text(dialog, IDC_STATUS) == L"Ready to apply changes",
        "editing after an error restores ready controls");

    for (UINT dpi : {96u, 144u, 192u}) {
        Preview scaled(SetupAppearance::Light, dpi);
        RECT client{}, card{}, folder{};
        GetClientRect(scaled.window, &client);
        GetWindowRect(GetDlgItem(scaled.window, IDC_SYNC), &card);
        GetWindowRect(GetDlgItem(scaled.window, IDC_ROOT), &folder);
        MapWindowPoints(nullptr, scaled.window, reinterpret_cast<POINT*>(&card), 2);
        MapWindowPoints(nullptr, scaled.window, reinterpret_cast<POINT*>(&folder), 2);
        check(card.left >= 0 && card.right <= client.right && folder.left >= 0 &&
                  folder.right <= client.right && folder.top >= card.bottom,
            "scaled layout keeps component cards and folder input aligned without horizontal clipping");
        scaled.view.message(WM_VSCROLL, SB_BOTTOM, 0);
        RECT action{};
        GetWindowRect(GetDlgItem(scaled.window, IDC_APPLY), &action);
        MapWindowPoints(nullptr, scaled.window, reinterpret_cast<POINT*>(&action), 2);
        scaled.save(executable().parent_path() / (L"setup-light-" + std::to_wstring(dpi) + L".png"));
        if (action.top < 0 || action.bottom > client.bottom) {
            std::cerr << "Layout DPI " << dpi << ", window DPI " << GetDpiForWindow(scaled.window)
                      << ", client " << client.bottom << ", action " << action.top << ".." << action.bottom
                      << '\n';
        }

        check(action.top >= 0 && action.bottom <= client.bottom,
            "short displays can scroll to the complete action row");
    }

    RECT frame{};
    GetWindowRect(dialog, &frame);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromRect(&frame, MONITOR_DEFAULTTONEAREST), &monitor);
    auto tall_dpi = static_cast<UINT>(((monitor.rcWork.bottom - monitor.rcWork.top) / 600 + 2) * 96);
    Preview compact(SetupAppearance::Dark, tall_dpi);
    check((GetWindowLongPtrW(compact.window, GWL_STYLE) & WS_VSCROLL) != 0,
        "layout adds scrolling when content exceeds the display height");
    compact.view.message(WM_VSCROLL, SB_BOTTOM, 0);
    RECT client{}, action{};
    GetClientRect(compact.window, &client);
    GetWindowRect(GetDlgItem(compact.window, IDC_APPLY), &action);
    MapWindowPoints(nullptr, compact.window, reinterpret_cast<POINT*>(&action), 2);
    check(action.top >= 0 && action.bottom <= client.bottom,
        "scrolling reaches the action row on an actually constrained display");
    int previous_scroll = GetScrollPos(compact.window, SB_VERT);
    SendMessageW(GetDlgItem(compact.window, IDC_SYNC), WM_SETFOCUS, 0, 0);
    RECT focused{};
    GetWindowRect(GetDlgItem(compact.window, IDC_SYNC), &focused);
    MapWindowPoints(nullptr, compact.window, reinterpret_cast<POINT*>(&focused), 2);
    check(GetScrollPos(compact.window, SB_VERT) < previous_scroll && focused.top >= 0 &&
              focused.bottom <= client.bottom,
        "keyboard focus brings a scrolled-out component back into view");
    Preview transition(SetupAppearance::Dark);
    RECT suggested{};
    GetWindowRect(transition.window, &suggested);
    transition.view.message(WM_DPICHANGED, MAKELONG(144, 144), reinterpret_cast<LPARAM>(&suggested));
    LOGFONTW font{};
    GetObjectW(reinterpret_cast<HFONT>(SendDlgItemMessageW(transition.window, IDC_ROOT, WM_GETFONT, 0, 0)),
        sizeof(font),
        &font);
    check(font.lfHeight == -MulDiv(15, 144, 96) &&
              control_text(transition.window, IDC_ROOT) == L"C:\\XSanity" &&
              IsDlgButtonChecked(transition.window, IDC_SYNC) == BST_CHECKED,
        "DPI changes rebuild fonts and preserve folder and component selection");
    auto small_icon = reinterpret_cast<HICON>(SendMessageW(transition.window, WM_GETICON, ICON_SMALL, 0));
    auto large_icon = reinterpret_cast<HICON>(SendMessageW(transition.window, WM_GETICON, ICON_BIG, 0));
    check(
        icon_size(
            small_icon, GetSystemMetricsForDpi(SM_CXSMICON, 144), GetSystemMetricsForDpi(SM_CYSMICON, 144)) &&
            icon_size(
                large_icon, GetSystemMetricsForDpi(SM_CXICON, 144), GetSystemMetricsForDpi(SM_CYICON, 144)),
        "DPI changes reload both setup title-bar icons at the new scale");
    if (previews) {
        auto output = executable().parent_path();
        CheckDlgButton(dialog, IDC_SYNC, BST_CHECKED);
        CheckDlgButton(dialog, IDC_OVERLAY, BST_UNCHECKED);
        preview.view.refresh(false);
        preview.save(output / L"setup-dark.png");
        preview.save(output / L"setup-dark-client.png", true);
        CheckDlgButton(dialog, IDC_SYNC, BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_OVERLAY, BST_UNCHECKED);
        preview.view.refresh(true);
        preview.save(output / L"setup-remove.png");
        CheckDlgButton(dialog, IDC_SYNC, BST_CHECKED);
        CheckDlgButton(dialog, IDC_OVERLAY, BST_CHECKED);
        preview.view.refresh(true);
        preview.view.working(false, true);
        preview.view.progress(60, L"Installing the companion");
        preview.save(output / L"setup-progress.png");
        preview.view.finish(true);
        preview.save(output / L"setup-complete.png");
        preview.view.refresh(true);
        preview.view.failure();
        preview.save(output / L"setup-error.png");
        Preview contrast(SetupAppearance::Contrast);
        contrast.save(output / L"setup-contrast.png");
    }
}

} // namespace piu::test
