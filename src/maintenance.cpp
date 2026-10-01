#include "maintenance.h"
#include "game_hook.h"
#include <objbase.h>

namespace piu {
fs::path maintenance_directory() {
    GUID guid{};
    winrt::check_hresult(CoCreateGuid(&guid));
    wchar_t text[40];
    StringFromGUID2(guid, text, 40);
    return fs::temp_directory_path() / (L"PiuCompanionSetup-" + std::wstring(text));
}

bool is_maintenance_directory(const fs::path& directory) {
    auto path = fs::absolute(directory).lexically_normal(),
         temp = fs::absolute(fs::temp_directory_path()).lexically_normal();
    auto drive = path.root_name().wstring();
    if (drive.size() != 2 || drive[1] != L':') {
        return false;
    }

    if (!fs::equivalent(path.parent_path(), temp)) {
        return false;
    }

    auto name = path.filename().wstring();
    constexpr std::wstring_view prefix = L"PiuCompanionSetup-";
    if (!name.starts_with(prefix) || name.size() != prefix.size() + 38) {
        return false;
    }

    GUID guid{};
    return SUCCEEDED(CLSIDFromString(name.c_str() + prefix.size(), &guid));
}

void clean_maintenance_after_exit(const fs::path& directory) {
    if (!is_maintenance_directory(directory)) {
        throw Error("Invalid maintenance cleanup location.");
    }

    safe_path(directory);
    safe_path(directory / L"maintenance.exe");
    safe_path(directory / L"cleanup.cmd");
    wchar_t system[MAX_PATH];
    UINT count = GetSystemDirectoryW(system, MAX_PATH);
    if (!count || count >= MAX_PATH) {
        fail("Find Windows cleanup tools");
    }

    fs::path system_path(system);
    auto delay = utf8((system_path / L"ping.exe").wstring());
    if (delay.find_first_of("\"%\r\n") != std::string::npos) {
        throw Error("Unexpected Windows system path.");
    }

    // Only fixed filenames in a validated, private temporary directory reach the shell.
    // No user paths are inserted into deletion commands; rmdir never removes other files.
    std::string script = "@echo off\r\nfor /l %%N in (1,1,30) do (\r\n del /q maintenance.exe >nul 2>nul\r\n "
                         "if not exist maintenance.exe goto finish\r\n \"" +
                         delay +
                         "\" -n 2 127.0.0.1 >nul 2>nul\r\n)\r\nexit /b 1\r\n:finish\r\n(\r\n del /q "
                         "cleanup.cmd >nul 2>nul\r\n cd ..\r\n rmdir \"" +
                         utf8(directory.filename().wstring()) + "\" >nul 2>nul\r\n)\r\n";
    atomic_write(directory / L"cleanup.cmd", script);
    auto shell = system_path / L"cmd.exe";
    std::wstring command = L"\"" + shell.wstring() + L"\" /d /q /v:off /c cleanup.cmd";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(shell.c_str(),
            command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            directory.c_str(),
            &startup,
            &process)) {
        fail("Start installer cleanup");
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}
} // namespace piu
