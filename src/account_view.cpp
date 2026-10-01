#include "account_view.h"
#include <shellapi.h>

namespace piu {
namespace {
constexpr std::array<int, 2> Profiles{IDC_PROFILE1, IDC_PROFILE2};
constexpr std::array<int, 2> Failed{IDC_FAILED1, IDC_FAILED2};
constexpr std::array<int, 2> Tokens{IDC_TOKEN, IDC_TOKEN2};
constexpr std::array<int, 2> PendingLabels{IDC_PENDING1, IDC_PENDING2};
constexpr std::array<int, 2> DiscardButtons{IDC_DISCARD1, IDC_DISCARD2};
} // namespace

void refresh_accounts(HWND dialog, const Engine& engine) {
    auto screenshot_error = wide(engine.screenshot_status());
    if (control_text(dialog, IDC_SCREENSHOT_STATUS) != screenshot_error) {
        SetDlgItemTextW(dialog, IDC_SCREENSHOT_STATUS, screenshot_error.c_str());
    }
    auto text = wide(engine.player_status());
    auto split = text.find(L"\r\n");
    std::array<std::wstring, 2> lines{
        text.substr(0, split), split == std::wstring::npos ? L"" : text.substr(split + 2)};
    for (size_t i = 0; i < lines.size(); ++i) {
        auto id = i == 0 ? IDC_STATUS : IDC_STATUS2;
        if (control_text(dialog, id) != lines[i]) {
            SetDlgItemTextW(dialog, id, lines[i].c_str());
        }
    }

    for (size_t i = 0; i < Profiles.size(); ++i) {
        auto count = engine.pending_count(i);
        auto label = count ? std::to_wstring(count) + L" pending upload(s)" : L"No pending uploads";
        if (control_text(dialog, PendingLabels[i]) != label) {
            SetDlgItemTextW(dialog, PendingLabels[i], label.c_str());
        }

        auto button = GetDlgItem(dialog, DiscardButtons[i]);
        if ((IsWindowEnabled(button) != FALSE) != (count != 0)) {
            EnableWindow(button, count != 0);
        }
    }
}

void initialize_accounts(HWND dialog, const Engine& engine) {
    auto config = engine.config();
    for (size_t i = 0; i < Profiles.size(); ++i) {
        SendDlgItemMessageW(dialog, Profiles[i], EM_SETLIMITTEXT, 128, 0);
        SendDlgItemMessageW(dialog, Tokens[i], EM_SETLIMITTEXT, 2048, 0);
        SetDlgItemTextW(dialog, Profiles[i], wide(config.accounts[i].profile).c_str());
        SetDlgItemTextW(dialog, Tokens[i], wide(engine.token(i)).c_str());
        CheckDlgButton(dialog, Failed[i], config.accounts[i].include_failed ? BST_CHECKED : BST_UNCHECKED);
        EnableWindow(GetDlgItem(dialog, Failed[i]), config.sync);
    }

    SendDlgItemMessageW(dialog, IDC_MIX, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Phoenix"));
    SendDlgItemMessageW(dialog, IDC_MIX, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Phoenix 2"));
    SendDlgItemMessageW(dialog, IDC_MIX, CB_SETCURSEL, config.mix == "Phoenix2" ? 1 : 0, 0);
    CheckDlgButton(dialog, IDC_SCREENSHOTS, config.screenshots.enabled ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemTextW(dialog, IDC_SCREENSHOT_FOLDER, config.screenshots.folder.c_str());
    EnableWindow(GetDlgItem(dialog, IDC_SCREENSHOTS), config.sync);
    update_screenshot_controls(dialog);
    refresh_accounts(dialog, engine);
}

void save_accounts(HWND dialog, Engine& engine) {
    std::array<AccountInput, 2> inputs;
    for (size_t i = 0; i < inputs.size(); ++i) {
        inputs[i] = {utf8(control_text(dialog, Profiles[i])),
            utf8(control_text(dialog, Tokens[i])),
            IsDlgButtonChecked(dialog, Failed[i]) == BST_CHECKED};
    }

    auto mix = SendDlgItemMessageW(dialog, IDC_MIX, CB_GETCURSEL, 0, 0);
    try {
        engine.configure(inputs,
            mix == 1 ? "Phoenix2" : "Phoenix",
            ScreenshotOptions{IsDlgButtonChecked(dialog, IDC_SCREENSHOTS) == BST_CHECKED,
                control_text(dialog, IDC_SCREENSHOT_FOLDER)});
    } catch (...) {
        for (auto& input : inputs) {
            SecureZeroMemory(input.token.data(), input.token.size());
        }

        throw;
    }

    for (auto& input : inputs) {
        SecureZeroMemory(input.token.data(), input.token.size());
    }
}

void update_screenshot_controls(HWND dialog) {
    bool enabled = IsWindowEnabled(GetDlgItem(dialog, IDC_SCREENSHOTS)) &&
                   IsDlgButtonChecked(dialog, IDC_SCREENSHOTS) == BST_CHECKED;
    for (auto id : {IDC_SCREENSHOT_FOLDER, IDC_SCREENSHOT_BROWSE}) {
        EnableWindow(GetDlgItem(dialog, id), enabled);
    }
}

void screenshot_folder_command(HWND dialog, const Engine& engine, bool browse) {
    fs::path folder = control_text(dialog, IDC_SCREENSHOT_FOLDER);
    if (browse) {
        if (auto selected = browse_folder(dialog, folder)) {
            validate_screenshot_folder(*selected, engine.config().game_root);
            SetDlgItemTextW(dialog, IDC_SCREENSHOT_FOLDER, selected->c_str());
        }
    } else {
        validate_screenshot_folder(folder, engine.config().game_root);
        fs::create_directories(folder);
        if (reinterpret_cast<INT_PTR>(
                ShellExecuteW(dialog, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
            throw Error("Could not open the screenshot folder.");
        }
    }
}
} // namespace piu
