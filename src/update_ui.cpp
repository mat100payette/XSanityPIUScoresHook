#include "update.h"
#include "ui.h"
#include "version.h"

namespace piu {
namespace {
constexpr int Download = 100;

enum class UpdatePhase { Checking, Available, Downloading, Finished };

struct UpdateDialog {
    const UpdateClient& client;
    const UpdateLauncher& launcher;
    HWND& active;
    HANDLE shutdown;
    HWND window = nullptr;
    UpdatePhase phase = UpdatePhase::Checking;
    std::optional<UpdateRelease> release;
    std::unique_ptr<PreparedUpdate> prepared;
    std::jthread worker;
    std::atomic<bool> done = false;
    std::atomic<int> percent = 0;
    std::string error;
    std::exception_ptr callback_error;
    bool cancelling = false;
    bool closing = false;
    std::wstring heading, content;
    TASKDIALOG_BUTTON download{Download, L"Download and open setup"};
    Icon large, small;

    TASKDIALOGCONFIG config() {
        TASKDIALOGCONFIG value{sizeof(value)};
        value.hInstance = GetModuleHandleW(nullptr);
        value.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_CALLBACK_TIMER;
        value.pszWindowTitle = L"PIU Companion update";
        value.pszMainIcon = MAKEINTRESOURCEW(IDI_APP);
        value.pszMainInstruction = heading.c_str();
        value.pszContent = content.c_str();
        value.dwCommonButtons = phase == UpdatePhase::Finished ? TDCBF_CLOSE_BUTTON : TDCBF_CANCEL_BUTTON;
        if (phase == UpdatePhase::Available) {
            value.cButtons = 1;
            value.pButtons = &download;
            value.nDefaultButton = Download;
        } else if (phase == UpdatePhase::Checking) {
            value.dwFlags |= TDF_SHOW_MARQUEE_PROGRESS_BAR;
        } else if (phase == UpdatePhase::Downloading) {
            value.dwFlags |= TDF_SHOW_PROGRESS_BAR;
        }

        value.pfCallback = callback;
        value.lpCallbackData = reinterpret_cast<LONG_PTR>(this);
        value.cxWidth = 320;
        return value;
    }

    void page(UpdatePhase next, std::wstring title, std::wstring message) {
        phase = next;
        heading = std::move(title);
        content = std::move(message);
        auto value = config();
        SendMessageW(window, TDM_NAVIGATE_PAGE, 0, reinterpret_cast<LPARAM>(&value));
    }

    void start(bool downloading) {
        done = false;
        error.clear();
        percent = 0;
        worker = std::jthread([this, downloading](std::stop_token stop) {
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                try {
                    if (downloading) {
                        prepared = client.download(*release, stop, [&](size_t received, size_t total) {
                            percent = static_cast<int>(received * 100 / total);
                        });
                    } else {
                        release = client.check(PIU_VERSION, stop);
                    }
                } catch (...) {
                    error = exception_message();
                }

                winrt::uninit_apartment();
            } catch (...) {
                error = exception_message();
            }

            done = true;
        });
    }

    void cancel() {
        if (phase == UpdatePhase::Checking || phase == UpdatePhase::Downloading) {
            cancelling = true;
            worker.request_stop();
            SendMessageW(window, TDM_ENABLE_BUTTON, IDCANCEL, FALSE);
            SendMessageW(window,
                TDM_SET_ELEMENT_TEXT,
                TDE_MAIN_INSTRUCTION,
                reinterpret_cast<LPARAM>(L"Cancelling update..."));
        } else {
            closing = true;
            SendMessageW(window, TDM_CLICK_BUTTON, phase == UpdatePhase::Finished ? IDCLOSE : IDCANCEL, 0);
        }
    }

    void poll() {
        if (shutdown && WaitForSingleObject(shutdown, 0) == WAIT_OBJECT_0 && !cancelling) {
            cancel();
            return;
        }

        if (phase == UpdatePhase::Downloading) {
            SendMessageW(window, TDM_SET_PROGRESS_BAR_POS, percent.load(), 0);
        }

        if (!done.exchange(false)) {
            return;
        }

        worker.join();
        if (cancelling) {
            closing = true;
            SendMessageW(window, TDM_ENABLE_BUTTON, IDCANCEL, TRUE);
            SendMessageW(window, TDM_CLICK_BUTTON, IDCANCEL, 0);
            return;
        }

        if (!error.empty()) {
            page(UpdatePhase::Finished, L"Couldn't complete the update", wide(error));
        } else if (phase == UpdatePhase::Checking) {
            if (release) {
                page(UpdatePhase::Available,
                    L"Version " + wide(release->version) + L" is available",
                    L"Installed: " PIU_VERSION_W L"\n\n"
                    L"Close XSanity, then click Apply changes in setup. Your settings and component choices "
                    L"are kept.");
            } else {
                page(UpdatePhase::Finished,
                    L"No newer version is available",
                    L"You're running PIU Companion " PIU_VERSION_W L".");
            }
        } else {
            try {
                prepared->open(launcher);
                closing = true;
                SendMessageW(window, TDM_CLICK_BUTTON, IDCANCEL, 0);
            } catch (...) {
                prepared.reset();
                page(UpdatePhase::Finished, L"Couldn't open setup", wide(exception_message()));
            }
        }
    }

    static HRESULT CALLBACK callback(HWND dialog, UINT message, WPARAM wparam, LPARAM, LONG_PTR context) {
        auto& self = *reinterpret_cast<UpdateDialog*>(context);
        try {
            if (message == TDN_CREATED) {
                self.window = self.active = dialog;
                set_window_icons(dialog, self.large, self.small, IDI_APP, GetDpiForWindow(dialog));
                SendMessageW(dialog, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 0);
                self.start(false);
            } else if (message == TDN_TIMER) {
                self.poll();
            } else if (message == TDN_BUTTON_CLICKED) {
                if (wparam == Download) {
                    self.page(UpdatePhase::Downloading,
                        L"Downloading version " + wide(self.release->version),
                        L"The installer will open after its download is verified.");
                    self.start(true);
                    return S_FALSE;
                }

                if (!self.closing &&
                    (self.phase == UpdatePhase::Checking || self.phase == UpdatePhase::Downloading)) {
                    self.cancel();
                    return S_FALSE;
                }
            } else if (message == TDN_DESTROYED) {
                self.active = nullptr;
            }
        } catch (...) {
            self.callback_error = std::current_exception();
            self.worker.request_stop();
            self.closing = true;
            SendMessageW(
                dialog, TDM_CLICK_BUTTON, self.phase == UpdatePhase::Finished ? IDCLOSE : IDCANCEL, 0);
        }

        return S_OK;
    }
};
} // namespace

void show_updates(HWND owner,
    HANDLE shutdown,
    HWND& active_dialog,
    const UpdateClient& client,
    const UpdateLauncher& launcher) {
    if (active_dialog) {
        SetForegroundWindow(active_dialog);
        return;
    }

    UpdateDialog dialog{client, launcher, active_dialog, shutdown};
    dialog.heading = L"Checking for updates...";
    dialog.content = L"Looking for the latest release on GitHub.";
    auto config = dialog.config();
    config.hwndParent = owner;
    auto result = TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
    dialog.worker.request_stop();
    if (dialog.worker.joinable()) {
        dialog.worker.join();
    }

    active_dialog = nullptr;
    if (dialog.callback_error) {
        std::rethrow_exception(dialog.callback_error);
    }

    winrt::check_hresult(result);
}
} // namespace piu