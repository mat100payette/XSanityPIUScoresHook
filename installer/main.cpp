#include "install.h"
#include "setup_view.h"
#include <shellapi.h>
#include <objbase.h>
#include <algorithm>

namespace piu {
namespace {
constexpr UINT Finished = WM_APP + 20;
constexpr UINT Progress = WM_APP + 21;

struct Update {
    int percent;
    std::wstring status;
};

void post_progress(HWND dialog, int percent, std::wstring_view status) {
    auto update = std::make_unique<Update>(Update{percent, std::wstring(status)});
    if (PostMessageW(dialog, Progress, 0, reinterpret_cast<LPARAM>(update.get()))) {
        update.release();
    }
}

struct Setup {
    Installer& installer;
    InstallPaths paths;
    InstallSelection selected;
    InstallSelection previous;
    SetupView view;
    std::thread worker;
    std::string error;
    bool busy = false;
    bool uninstall = false;
    bool installed = false;
    bool complete = false;
    HWND window = nullptr;

    void refresh() {
        complete = false;
        view.refresh(
            installed, installed && previous.sync && IsDlgButtonChecked(window, IDC_SYNC) != BST_CHECKED);
    }

    void initialize() {
        view.initialize(window);
        CheckDlgButton(window, IDC_SYNC, selected.sync && !uninstall ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(window, IDC_OVERLAY, selected.overlay && !uninstall ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(window, IDC_LAUNCH, BST_CHECKED);
        SetDlgItemTextW(window, IDC_ROOT, selected.game_root.c_str());
        refresh();
    }

    void run_install(HWND dialog) {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            installer.apply(selected, [dialog](int percent, std::wstring_view status) {
                post_progress(dialog, percent, status);
            });
        } catch (...) {
            error = exception_message();
        }

        PostMessageW(dialog, Finished, 0, 0);
    }

    void apply() {
        selected = {fs::path(control_text(window, IDC_ROOT)),
            IsDlgButtonChecked(window, IDC_SYNC) == BST_CHECKED,
            IsDlgButtonChecked(window, IDC_OVERLAY) == BST_CHECKED};
        busy = true;
        error.clear();
        view.working(!selected.sync && !selected.overlay, installed);

        worker = std::thread([this, dialog = window] {
            run_install(dialog);
        });
    }

    void finish() {
        worker.join();
        busy = false;
        if (!error.empty()) {
            refresh();
            view.failure();
            show_error(window, error);
            return;
        }

        bool keep = selected.sync || selected.overlay;
        installed = keep;
        previous = selected;
        complete = true;
        view.finish(keep);

        if (keep && IsDlgButtonChecked(window, IDC_LAUNCH) == BST_CHECKED) {
            try {
                launch(paths.app / L"PiuCompanion.exe", L"--no-game");
            } catch (...) {
                show_error(window, exception_message());
            }
        }
    }

    INT_PTR command(WPARAM wparam) {
        auto id = LOWORD(wparam);
        if (id == IDC_SYNC || id == IDC_OVERLAY || (id == IDC_ROOT && HIWORD(wparam) == EN_CHANGE)) {
            refresh();
            return TRUE;
        }

        switch (id) {
        case IDCANCEL:
            EndDialog(window, 0);
            return TRUE;
        case IDC_BROWSE: {
            auto path = browse_folder(window, control_text(window, IDC_ROOT));
            if (path) {
                SetDlgItemTextW(window, IDC_ROOT, path->c_str());
            }

            return TRUE;
        }
        case IDC_APPLY:
            if (complete) {
                EndDialog(window, 0);
            } else {
                apply();
            }

            return TRUE;
        default:
            return FALSE;
        }
    }

    void fail_message(UINT message) {
        auto failure = exception_message();
        if (message == WM_INITDIALOG) {
            error = failure;
            EndDialog(window, -1);
        } else if (busy && worker.joinable()) {
            show_error(window, failure);
        } else {
            busy = false;
            error = failure;
            refresh();
            view.failure();
            show_error(window, failure);
        }
    }
};

INT_PTR CALLBACK setup_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto setup = reinterpret_cast<Setup*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        setup = reinterpret_cast<Setup*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        setup->window = dialog;
    }

    if (!setup) {
        return FALSE;
    }

    try {
        if (message == WM_INITDIALOG) {
            setup->initialize();
            return TRUE;
        }

        if (auto result = setup->view.message(message, wparam, lparam)) {
            return *result;
        }

        switch (message) {
        case WM_CLOSE:
            if (!setup->busy) {
                EndDialog(dialog, 0);
            }

            return TRUE;
        case Progress: {
            std::unique_ptr<Update> update(reinterpret_cast<Update*>(lparam));
            setup->view.progress(update->percent, update->status);
            return TRUE;
        }
        case Finished:
            setup->finish();
            return TRUE;
        case WM_COMMAND:
            if (!setup->busy) {
                return setup->command(wparam);
            }

            break;
        }
    } catch (...) {
        setup->fail_message(message);
        return TRUE;
    }

    return FALSE;
}

bool same_path(const fs::path& left, const fs::path& right) {
    return _wcsicmp(fs::absolute(left).lexically_normal().c_str(),
               fs::absolute(right).lexically_normal().c_str()) == 0;
}
} // namespace

int run_setup(const std::vector<std::wstring>& args) {
    auto paths = InstallPaths::user();
    auto self = executable();
    bool uninstall = false, copy = false;
    for (const auto& arg : args) {
        if (arg == L"--uninstall") {
            uninstall = true;
        }

        if (arg == L"--maintenance-copy") {
            copy = true;
        }
    }

    if (!copy && same_path(self, paths.app / L"PiuCompanionSetup.exe")) {
        auto directory = maintenance_directory();
        auto temporary = directory / L"maintenance.exe";
        safe_path(temporary);
        fs::create_directory(directory);
        fs::copy_file(self, temporary, fs::copy_options::none);
        try {
            launch(temporary,
                L"--maintenance-copy --wait-pid " + std::to_wstring(GetCurrentProcessId()) +
                    (uninstall ? L" --uninstall" : L""));
        } catch (...) {
            fs::remove(temporary);
            fs::remove(directory);
            throw;
        }

        return 0;
    }

    if (copy) {
        if (!is_maintenance_directory(self.parent_path()) || self.filename() != L"maintenance.exe") {
            throw Error("Invalid maintenance location.");
        }

        for (size_t i = 0; i + 1 < args.size(); ++i) {
            if (args[i] == L"--wait-pid") {
                Handle parent(OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(std::stoul(args[i + 1]))));
                if (parent.get() && WaitForSingleObject(parent.get(), 30000) != WAIT_OBJECT_0) {
                    throw Error("The previous setup is still open. Close it and retry.");
                }
            }
        }
    }

    // One installer at a time, including installed maintenance copies.
    Handle mutex(CreateMutexW(nullptr, TRUE, SetupWindowMutex));
    if (!mutex.get()) {
        fail("Start setup");
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        throw Error("Setup is already open.");
    }

    WindowsInstallHost host;
    Installer installer(
        paths, host, GameHook{}, resource(AppResource), read(self, 64 * 1024 * 1024), resource(104));
    Setup setup{installer, paths};
    setup.uninstall = uninstall;
    setup.selected = installer.selection();
    setup.previous = setup.selected;
    setup.installed = installer.installed();
    INT_PTR result = DialogBoxParamW(GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(IDD_SETUP),
        nullptr,
        setup_proc,
        reinterpret_cast<LPARAM>(&setup));
    if (setup.worker.joinable()) {
        setup.worker.join();
    }

    if (result == -1) {
        throw Error(setup.error.empty() ? "Could not open setup." : setup.error);
    }

    ReleaseMutex(mutex.get());
    return 0;
}
} // namespace piu

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    piu::fs::path cleanup;
    int result = 1;
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        int count = 0;
        auto args = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!args) {
            piu::fail("Read setup arguments");
        }

        std::vector<std::wstring> values;
        for (int i = 1; i < count; ++i) {
            values.emplace_back(args[i]);
        }

        LocalFree(args);
        auto self = piu::executable();
        if (std::find(values.begin(), values.end(), L"--maintenance-copy") != values.end() &&
            self.filename() == L"maintenance.exe" && piu::is_maintenance_directory(self.parent_path())) {
            cleanup = self.parent_path();
        }

        result = piu::run_setup(values);
    } catch (...) {
        piu::show_error(nullptr, piu::exception_message());
    }

    if (!cleanup.empty()) {
        try {
            piu::clean_maintenance_after_exit(cleanup);
        } catch (...) {
            piu::show_error(nullptr, piu::exception_message());
        }
    }

    return result;
}
