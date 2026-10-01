#include "account_view.h"

namespace piu {
namespace {
constexpr std::array<int, 2> Profiles{IDC_PROFILE1, IDC_PROFILE2};
constexpr std::array<int, 2> Tokens{IDC_TOKEN, IDC_TOKEN2};
constexpr std::array<int, 2> PendingLabels{IDC_PENDING1, IDC_PENDING2};
constexpr std::array<int, 2> DiscardButtons{IDC_DISCARD1, IDC_DISCARD2};
} // namespace

void refresh_accounts(HWND dialog, const Engine& engine) {
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
    }

    SendDlgItemMessageW(dialog, IDC_MIX, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Phoenix"));
    SendDlgItemMessageW(dialog, IDC_MIX, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Phoenix 2"));
    SendDlgItemMessageW(dialog, IDC_MIX, CB_SETCURSEL, config.mix == "Phoenix2" ? 1 : 0, 0);
    refresh_accounts(dialog, engine);
}

void save_accounts(HWND dialog, Engine& engine) {
    std::array<AccountInput, 2> inputs;
    for (size_t i = 0; i < inputs.size(); ++i) {
        inputs[i] = {utf8(control_text(dialog, Profiles[i])), utf8(control_text(dialog, Tokens[i]))};
    }

    auto mix = SendDlgItemMessageW(dialog, IDC_MIX, CB_GETCURSEL, 0, 0);
    try {
        engine.configure(inputs, mix == 1 ? "Phoenix2" : "Phoenix");
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
} // namespace piu
