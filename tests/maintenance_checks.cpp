#include "support.h"
#include "install.h"

namespace piu::test {
void file_replacement_checks() {
    Fixture fixture("file-replacement");
    auto path = fixture.root / L"state.json";
    atomic_write(path, "previous");
    HANDLE held = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(held != INVALID_HANDLE_VALUE, "file replacement fixture holds a read lock");
    std::exception_ptr error;
    std::jthread writer([&] {
        try {
            atomic_write(path, "updated");
        } catch (...) {
            error = std::current_exception();
        }
    });
    bool intact;
    {
        Handle reader(held);
        Sleep(100);
        intact = read(path) == "previous";
    }

    writer.join();
    if (error) {
        std::rethrow_exception(error);
    }

    check(intact && read(path) == "updated",
        "atomic replacement tolerates a temporary reader without losing old data");
}

int ipc_companion(const std::wstring& prefix) {
    auto mutex_name = L"Local\\" + prefix + L".Mutex", event_name = L"Local\\" + prefix + L".Stop",
         ready_name = L"Local\\" + prefix + L".Ready";
    Handle mutex(CreateMutexW(nullptr, TRUE, mutex_name.c_str()));
    Handle stop(CreateEventW(nullptr, TRUE, FALSE, event_name.c_str()));
    Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, ready_name.c_str()));
    if (!mutex.get() || !stop.get() || !ready.get()) {
        fail("Create isolated IPC fixture");
    }

    WNDCLASSW type{};
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = prefix.c_str();
    type.lpfnWndProc = [](HWND window, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT {
        if (message == WM_CREATE) {
            SetWindowLongPtrW(window,
                GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(
                    static_cast<CREATESTRUCTW*>(reinterpret_cast<void*>(lparam))->lpCreateParams));
        }

        if (message == WM_TIMER &&
            WaitForSingleObject(reinterpret_cast<HANDLE>(GetWindowLongPtrW(window, GWLP_USERDATA)), 0) ==
                WAIT_OBJECT_0) {
            DestroyWindow(window);
        }

        if (message == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProcW(window, message, wparam, lparam);
    };
    if (!RegisterClassW(&type)) {
        fail("Register isolated IPC window");
    }

    HWND window = CreateWindowExW(
        0, prefix.c_str(), L"Test companion", 0, 0, 0, 0, 0, nullptr, nullptr, type.hInstance, stop.get());
    if (!window) {
        fail("Create isolated IPC window");
    }

    SetTimer(window, 1, 20, nullptr);
    SetEvent(ready.get());
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    ReleaseMutex(mutex.get());
    Sleep(300);
    return 0; // Simulate an executable still mapped after mutex release.
}

void ipc_checks() {
    Fixture fixture("ipc");
    auto copy = fixture.root / L"isolated-companion.exe";
    fs::copy_file(executable(), copy);
    auto prefix = L"PiuCompanion.Checks." + std::to_wstring(GetCurrentProcessId());
    auto ready_name = L"Local\\" + prefix + L".Ready";
    Handle ready(CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str()));
    std::wstring command = L"\"" + copy.wstring() + L"\" --ipc-companion " + prefix;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(copy.c_str(),
            command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startup,
            &process)) {
        fail("Start isolated IPC fixture");
    }

    Handle child(process.hProcess);
    ChildProcessGuard stop_child{child.get()};
    CloseHandle(process.hThread);
    check(WaitForSingleObject(ready.get(), 10000) == WAIT_OBJECT_0,
        "isolated companion IPC fixture becomes ready");
    WindowsInstallHost host(L"Local\\" + prefix + L".Mutex", L"Local\\" + prefix + L".Stop", prefix);
    host.stop_companion();
    check(WaitForSingleObject(child.get(), 0) == WAIT_OBJECT_0,
        "maintenance waits for full process exit after singleton release");
    atomic_write(copy, "replacement executable");
    check(read(copy) == "replacement executable",
        "companion executable can be replaced immediately after maintenance stop");
}

void maintenance_checks() {
    file_replacement_checks();
    ipc_checks();
    Fixture self("self-delete", true);
    auto copy = self.root / L"maintenance.exe";
    fs::copy_file(executable(), copy);
    std::wstring command = L"\"" + copy.wstring() + L"\" --self-delete";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(copy.c_str(),
            command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startup,
            &process)) {
        fail("Start self-cleanup test");
    }

    Handle child(process.hProcess);
    ChildProcessGuard stop_child{child.get()};
    CloseHandle(process.hThread);
    auto waited = WaitForSingleObject(child.get(), 10000);
    DWORD code = 0;
    GetExitCodeProcess(child.get(), &code);
    if (code) {
        throw Error("Maintenance self-cleanup returned Windows error " + std::to_string(code));
    }

    auto deadline = GetTickCount64() + 15000;
    while (fs::exists(self.root) && GetTickCount64() < deadline) {
        Sleep(50);
    }

    check(waited == WAIT_OBJECT_0 && !fs::exists(self.root),
        "maintenance cleanup removes its copy, script and private folder after exit");
    rejects(
        [&] {
            clean_maintenance_after_exit(fs::temp_directory_path());
        },
        "cleanup refuses the general temp folder");
    check(!is_maintenance_directory(
              L"\\\\server\\share\\PiuCompanionSetup-{00000000-0000-0000-0000-000000000000}"),
        "cleanup refuses a network working directory");
    Fixture foreign("foreign-cleanup", true);
    atomic_write(foreign.root / L"maintenance.exe", "fake maintenance copy");
    atomic_write(foreign.root / L"keep.txt", "unrelated");
    clean_maintenance_after_exit(foreign.root);
    deadline = GetTickCount64() + 10000;
    while (fs::exists(foreign.root / L"cleanup.cmd") && GetTickCount64() < deadline) {
        Sleep(50);
    }

    check(!fs::exists(foreign.root / L"maintenance.exe") && read(foreign.root / L"keep.txt") == "unrelated",
        "maintenance cleanup leaves unrelated files untouched");
}

} // namespace piu::test
