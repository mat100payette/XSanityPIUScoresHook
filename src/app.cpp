#include "engine.h"
#include "account_view.h"
#include "game_hook.h"
#include "overlay.h"
#include "ui.h"
#include "update.h"
#include <shellapi.h>
#include <shlobj.h>

namespace piu {
namespace {
constexpr UINT TrayMessage = WM_APP + 2;
constexpr UINT Account = 1;
constexpr UINT Manage = 2;
constexpr UINT Exit = 3;
constexpr UINT Updates = 4;

struct App {
    Engine& engine;
    HWND window = nullptr;
    HWND account_dialog = nullptr;
    HWND update_dialog = nullptr;
    HANDLE stop = nullptr;
    NOTIFYICONDATAW icon{};
    Icon large_icon;
    Icon small_icon;
    Icon account_large_icon;
    Icon account_small_icon;
    UINT taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    std::string overlay_error;

    void launch_game() {
        try {
            auto root = engine.config().game_root;
            if (!game_running()) {
                launch(game_executable(root), L"", root);
            }
        } catch (...) {
            show_error(window, exception_message());
        }
    }

    void initialize_account(HWND dialog) {
        account_dialog = dialog;
        set_window_icons(dialog, account_large_icon, account_small_icon, IDI_APP, GetDpiForWindow(dialog));
        initialize_accounts(dialog, engine);
        SetTimer(dialog, 1, 1000, nullptr);
    }

    void save_account(HWND dialog) {
        try {
            save_accounts(dialog, engine);
            EndDialog(dialog, IDOK);
        } catch (...) {
            show_error(dialog, exception_message());
        }
    }

    void discard_account(HWND dialog, size_t account) {
        auto text =
            L"Discard all pending scores for Account " + std::to_wstring(account + 1) +
            L"? They will not be uploaded or recovered from game history. The other account is unchanged.";
        if (MessageBoxW(
                dialog, text.c_str(), L"Discard pending scores", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) !=
            IDYES) {
            return;
        }

        try {
            engine.discard_pending(account);
            refresh_accounts(dialog, engine);
        } catch (...) {
            show_error(dialog, exception_message());
        }
    }

    static INT_PTR CALLBACK account_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
        auto app = reinterpret_cast<App*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (message == WM_INITDIALOG) {
            app = reinterpret_cast<App*>(lparam);
            SetWindowLongPtrW(dialog, DWLP_USER, lparam);
            app->initialize_account(dialog);
            return TRUE;
        }

        if (message == WM_TIMER && app) {
            refresh_accounts(dialog, app->engine);
            return TRUE;
        }

        if (message == WM_DESTROY) {
            KillTimer(dialog, 1);
        }

        if (message == WM_DPICHANGED && app) {
            set_window_icons(
                dialog, app->account_large_icon, app->account_small_icon, IDI_APP, HIWORD(wparam));
        }

        if (message != WM_COMMAND || !app) {
            return FALSE;
        }

        switch (LOWORD(wparam)) {
        case IDC_DISCARD1:
        case IDC_DISCARD2:
            app->discard_account(dialog, LOWORD(wparam) == IDC_DISCARD1 ? 0 : 1);
            return TRUE;
        case IDC_WEBSITE:
            ShellExecuteW(
                dialog, L"open", L"https://piuscores.arroweclip.se", nullptr, nullptr, SW_SHOWNORMAL);
            return TRUE;
        case IDOK:
            app->save_account(dialog);
            return TRUE;
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        default:
            return FALSE;
        }
    }

    void show_account() {
        DialogBoxParamW(GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(IDD_ACCOUNT),
            window,
            account_proc,
            reinterpret_cast<LPARAM>(this));
        account_dialog = nullptr;
    }

    void add_tray_icon() {
        icon.cbSize = sizeof(icon);
        icon.hWnd = window;
        icon.uID = 1;
        icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        icon.uCallbackMessage = TrayMessage;
        icon.hIcon = small_icon.get();
        wcscpy_s(icon.szTip, L"PIU Companion");

        if (!Shell_NotifyIconW(NIM_ADD, &icon)) {
            fail("Add companion tray icon");
        }

        SetTimer(window, 1, 500, nullptr);
    }

    void update_tip() {
        auto status = wide(overlay_error.empty() ? engine.status() : overlay_error);
        wcsncpy_s(icon.szTip, status.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &icon);
    }

    void show_menu() {
        HMENU menu = CreatePopupMenu();
        auto status = wide(overlay_error.empty() ? engine.status() : overlay_error);
        if (status.size() > 100) {
            status = status.substr(0, 97) + L"...";
        }

        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, status.c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, Account, L"Account settings...");
        AppendMenuW(menu, MF_STRING, Manage, L"Manage installation...");
        AppendMenuW(menu, MF_STRING, Updates, L"Check for updates...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, Exit, L"Exit");

        POINT point{};
        GetCursorPos(&point);
        SetForegroundWindow(window);
        auto choice =
            TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window, nullptr);
        DestroyMenu(menu);
        PostMessageW(window, WM_NULL, 0, 0);

        switch (choice) {
        case Account:
            show_account();
            break;
        case Manage:
            try {
                launch(executable().parent_path() / L"PiuCompanionSetup.exe");
            } catch (...) {
                show_error(window, exception_message());
            }

            break;
        case Updates:
            try {
                show_updates(window, stop, update_dialog);
            } catch (...) {
                show_error(window, exception_message());
            }

            break;
        case Exit:
            DestroyWindow(window);
            break;
        }
    }
};

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_CREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }

    if (!app) {
        return DefWindowProcW(window, message, wparam, lparam);
    }

    if (app->taskbar_created && message == app->taskbar_created) {
        Shell_NotifyIconW(NIM_ADD, &app->icon);
        return 0;
    }

    switch (message) {
    case TrayMessage:
        if (lparam == WM_RBUTTONUP || lparam == WM_LBUTTONUP) {
            app->show_menu();
        }

        return 0;
    case LaunchGameMessage:
        app->launch_game();
        return 0;
    case WM_TIMER:
        app->update_tip();
        if (WaitForSingleObject(app->stop, 0) == WAIT_OBJECT_0 && !app->update_dialog) {
            if (app->account_dialog) {
                EndDialog(app->account_dialog, IDCANCEL);
            }

            DestroyWindow(window);
        }

        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &app->icon);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

void check_game_connection(const Preferences& config) {
    if ((!config.sync && !config.overlay) || config.game_root.empty()) {
        throw Error("Run the companion installer to choose your components and XSanity folder.");
    }

    auto hook = GameHook{}.configured(config.sync, config.overlay);
    if (hook.preflight(config.game_root).state != HookState::Current) {
        throw Error("The game connection needs updating. Reopen setup and apply your installed components.");
    }
}

void create_app_window(App& app) {
    auto dpi = GetDpiForSystem();
    app.large_icon.load(
        IDI_APP, GetSystemMetricsForDpi(SM_CXICON, dpi), GetSystemMetricsForDpi(SM_CYICON, dpi));
    app.small_icon.load(
        IDI_APP, GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CYSMICON, dpi));

    WNDCLASSEXW type{sizeof(type)};
    type.lpfnWndProc = window_proc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = WindowClass;
    type.hIcon = app.large_icon.get();
    type.hIconSm = app.small_icon.get();
    if (!RegisterClassExW(&type)) {
        fail("Register companion window");
    }

    HWND window = CreateWindowExW(
        0, WindowClass, L"PIU Companion", 0, 0, 0, 0, 0, nullptr, nullptr, type.hInstance, &app);
    if (!window) {
        fail("Create companion window");
    }
}
} // namespace

int run_app(bool start_game) {
    Handle mutex(CreateMutexW(nullptr, TRUE, AppMutex));
    if (!mutex.get()) {
        fail("Start companion");
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (start_game) {
            auto window = FindWindowW(WindowClass, nullptr);
            if (window) {
                PostMessageW(window, LaunchGameMessage, 0, 0);
            }
        }

        return 0;
    }

    Handle event(CreateEventW(nullptr, TRUE, FALSE, StopEvent));
    if (!event.get()) {
        fail("Create companion stop event");
    }

    ResetEvent(event.get());

    PiuScoresApi api;
    Engine engine(known_folder(FOLDERID_LocalAppData) / L"XSanityPIUScoresHook", api);
    engine.load();
    auto config = engine.config();
    check_game_connection(config);

    App app{engine};
    app.stop = event.get();
    OverlayServer overlay([&] {
        return engine.state();
    });
    try {
        overlay.start(config.overlay);
    } catch (...) {
        app.overlay_error = exception_message();
        show_error(nullptr, app.overlay_error);
    }

    create_app_window(app);
    app.add_tray_icon();
    engine.start();
    if ((engine.token().empty() && engine.token(1).empty()) ||
        (config.sync && !engine.token().empty() && config.accounts[0].profile.empty())) {
        app.show_account();
    }

    if (start_game) {
        app.launch_game();
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    engine.stop();
    overlay.stop();
    ReleaseMutex(mutex.get());
    return 0;
}
} // namespace piu

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR args, int) {
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        return piu::run_app(std::wstring_view(args) != L"--no-game");
    } catch (...) {
        piu::show_error(nullptr, piu::exception_message());
        return 1;
    }
}
