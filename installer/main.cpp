#include "install.h"
#include "setup_view.h"
#include <shellapi.h>
#include <objbase.h>
#include <algorithm>

namespace piu {
namespace {
constexpr UINT Finished = WM_APP + 20, Progress = WM_APP + 21;
struct Update { int percent; std::wstring status; };
struct Setup {
    Installer& installer;
    InstallPaths paths;
    InstallSelection selected, previous;
    SetupView view;
    std::thread worker;
    std::string error;
    bool busy = false, uninstall = false, installed = false, complete = false;
    void refresh() {
        complete = false;
        view.refresh(installed, installed && previous.sync && IsDlgButtonChecked(window, IDC_SYNC) != BST_CHECKED);
    }
    HWND window = nullptr;
};
INT_PTR CALLBACK setup_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto setup = reinterpret_cast<Setup*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        setup = reinterpret_cast<Setup*>(lparam); SetWindowLongPtrW(dialog, DWLP_USER, lparam); setup->window = dialog;
    }
    if (!setup) return FALSE;
    try {
        if (message == WM_INITDIALOG) {
            setup->view.initialize(dialog);
            CheckDlgButton(dialog, IDC_SYNC, setup->selected.sync && !setup->uninstall ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(dialog, IDC_OVERLAY, setup->selected.overlay && !setup->uninstall ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(dialog, IDC_LAUNCH, BST_CHECKED); SetDlgItemTextW(dialog, IDC_ROOT, setup->selected.game_root.c_str());
            setup->refresh(); return TRUE;
        }
        if (auto result = setup->view.message(message, wparam, lparam)) return *result;
        if (message == WM_CLOSE) { if (!setup->busy) EndDialog(dialog, 0); return TRUE; }
        if (message == Progress) {
            std::unique_ptr<Update> update(reinterpret_cast<Update*>(lparam));
            setup->view.progress(update->percent, update->status); return TRUE;
        }
        if (message == Finished) {
            setup->worker.join(); setup->busy = false;
            if (setup->error.empty()) {
                bool keep = setup->selected.sync || setup->selected.overlay;
                setup->installed = keep; setup->previous = setup->selected; setup->complete = true;
                setup->view.finish(keep);
                if (keep && IsDlgButtonChecked(dialog, IDC_LAUNCH) == BST_CHECKED) {
                    try { launch(setup->paths.app / L"PiuCompanion.exe", L"--no-game"); }
                    catch (...) { show_error(dialog, exception_message()); }
                }
            } else {
                setup->refresh(); setup->view.failure(); show_error(dialog, setup->error);
            }
            return TRUE;
        }
        if (message == WM_COMMAND && !setup->busy) {
            auto id = LOWORD(wparam);
            if (id == IDCANCEL) { EndDialog(dialog, 0); return TRUE; }
            if (id == IDC_SYNC || id == IDC_OVERLAY || (id == IDC_ROOT && HIWORD(wparam) == EN_CHANGE)) { setup->refresh(); return TRUE; }
            if (id == IDC_BROWSE) {
                auto path = browse_folder(dialog, control_text(dialog, IDC_ROOT));
                if (path) SetDlgItemTextW(dialog, IDC_ROOT, path->c_str()); return TRUE;
            }
            if (id == IDC_APPLY) {
                if (setup->complete) { EndDialog(dialog, 0); return TRUE; }
                setup->selected = {fs::path(control_text(dialog, IDC_ROOT)), IsDlgButtonChecked(dialog, IDC_SYNC) == BST_CHECKED, IsDlgButtonChecked(dialog, IDC_OVERLAY) == BST_CHECKED};
                setup->busy = true; setup->error.clear();
                setup->view.working(!setup->selected.sync && !setup->selected.overlay, setup->installed);
                setup->worker = std::thread([setup, dialog] {
                    try {
                        winrt::init_apartment(winrt::apartment_type::multi_threaded);
                        setup->installer.apply(setup->selected, [dialog](int percent, std::wstring_view status) {
                            auto update = std::make_unique<Update>(Update{percent, std::wstring(status)});
                            if (PostMessageW(dialog, Progress, 0, reinterpret_cast<LPARAM>(update.get()))) update.release();
                        });
                    } catch (...) { setup->error = exception_message(); }
                    PostMessageW(dialog, Finished, 0, 0);
                }); return TRUE;
            }
        }
    } catch (...) {
        auto error = exception_message();
        if (message == WM_INITDIALOG) { setup->error = error; EndDialog(dialog, -1); }
        else if (setup->busy && setup->worker.joinable()) show_error(dialog, error);
        else { setup->busy = false; setup->error = error; setup->refresh(); setup->view.failure(); show_error(dialog, error); }
        return TRUE;
    }
    return FALSE;
}
bool same_path(const fs::path& left, const fs::path& right) { return _wcsicmp(fs::absolute(left).lexically_normal().c_str(), fs::absolute(right).lexically_normal().c_str()) == 0; }
}
int run_setup(const std::vector<std::wstring>& args) {
    auto paths = InstallPaths::user(); auto self = executable(); bool uninstall = false, copy = false;
    for (const auto& arg : args) { if (arg == L"--uninstall") uninstall = true; if (arg == L"--maintenance-copy") copy = true; }
    if (!copy && same_path(self, paths.app / L"PiuCompanionSetup.exe")) {
        auto directory = maintenance_directory(); auto temporary = directory / L"maintenance.exe"; safe_path(temporary); fs::create_directory(directory);
        fs::copy_file(self, temporary, fs::copy_options::none);
        try { launch(temporary, L"--maintenance-copy --wait-pid " + std::to_wstring(GetCurrentProcessId()) + (uninstall ? L" --uninstall" : L"")); }
        catch (...) { fs::remove(temporary); fs::remove(directory); throw; } return 0;
    }
    if (copy) {
        if (!is_maintenance_directory(self.parent_path()) || self.filename() != L"maintenance.exe") throw Error("Invalid maintenance location.");
        for (size_t i = 0; i + 1 < args.size(); ++i) if (args[i] == L"--wait-pid") {
            Handle parent(OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(std::stoul(args[i + 1]))));
            if (parent.get() && WaitForSingleObject(parent.get(), 30000) != WAIT_OBJECT_0) throw Error("The previous setup is still open. Close it and retry.");
        }
    }
    // One installer at a time, including installed maintenance copies.
    Handle mutex(CreateMutexW(nullptr, TRUE, L"Local\\XSanityPIUScoresHook.Setup")); if (!mutex.get()) fail("Start setup");
    if (GetLastError() == ERROR_ALREADY_EXISTS) throw Error("Setup is already open.");
    WindowsInstallHost host; Installer installer(paths, host, GameHook{}, resource(AppResource), read(self, 64 * 1024 * 1024), resource(104));
    Setup setup{installer, paths}; setup.uninstall = uninstall; setup.selected = installer.selection(); setup.previous = setup.selected; setup.installed = installer.installed();
    INT_PTR result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETUP), nullptr, setup_proc, reinterpret_cast<LPARAM>(&setup));
    if (setup.worker.joinable()) setup.worker.join(); if (result == -1) throw Error(setup.error.empty() ? "Could not open setup." : setup.error);
    ReleaseMutex(mutex.get()); return 0;
}
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    piu::fs::path cleanup;
    int result = 1;
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded); int count = 0; auto args = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!args) piu::fail("Read setup arguments"); std::vector<std::wstring> values; for (int i = 1; i < count; ++i) values.emplace_back(args[i]); LocalFree(args);
        auto self = piu::executable();
        if (std::find(values.begin(), values.end(), L"--maintenance-copy") != values.end() && self.filename() == L"maintenance.exe" && piu::is_maintenance_directory(self.parent_path())) cleanup = self.parent_path();
        result = piu::run_setup(values);
    } catch (...) { piu::show_error(nullptr, piu::exception_message()); }
    if (!cleanup.empty()) { try { piu::clean_maintenance_after_exit(cleanup); } catch (...) { piu::show_error(nullptr, piu::exception_message()); } }
    return result;
}
