#pragma once
#ifndef RC_INVOKED
#include "platform.h"
#endif

#define IDD_ACCOUNT 201
#define IDD_SETUP 202
#define IDC_TOKEN 1001
#define IDC_MIX 1002
#define IDC_STATUS 1003
#define IDC_WEBSITE 1004
#define IDC_ROOT 1010
#define IDC_BROWSE 1011
#define IDC_SYNC 1012
#define IDC_OVERLAY 1013
#define IDC_APPLY 1014
#define IDC_LAUNCH 1015
#define IDC_INTRO 1016
#define IDC_TITLE 1017
#define IDC_COMPONENTS 1018
#define IDC_FOLDER_LABEL 1019
#define IDC_DETAILS 1020
#define IDI_APP 105

#ifndef RC_INVOKED
namespace piu {
inline std::wstring control_text(HWND dialog, int id) {
    auto control = GetDlgItem(dialog, id); int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<size_t>(length) + 1, L'\0'); GetWindowTextW(control, value.data(), length + 1); value.resize(static_cast<size_t>(length)); return value;
}
inline void show_error(HWND owner, const std::string& message) { MessageBoxW(owner, wide(message).c_str(), L"PIU Companion", MB_OK | MB_ICONERROR); }
inline std::string exception_message() {
    try { throw; } catch (const std::exception& error) { return error.what(); } catch (const winrt::hresult_error& error) { return utf8(error.message().c_str()); } catch (...) { return "An unexpected Windows error occurred."; }
}
}
#endif
