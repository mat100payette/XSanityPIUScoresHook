#include "install.h"
#include "version.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <algorithm>

namespace piu {
namespace {
constexpr wchar_t UninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\XSanityPIUScoresHook";
struct Key {
    HKEY value = nullptr;
    ~Key() { if (value) RegCloseKey(value); }
};
RegistryValue text_value(const std::wstring& text) {
    RegistryValue value; auto ptr = reinterpret_cast<const BYTE*>(text.c_str()); value.bytes.assign(ptr, ptr + (text.size() + 1) * sizeof(wchar_t)); return value;
}
RegistryValue int_value(DWORD data) { RegistryValue value; value.type = REG_DWORD; auto ptr = reinterpret_cast<const BYTE*>(&data); value.bytes.assign(ptr, ptr + sizeof(data)); return value; }
Registration product_registration(const InstallPaths& paths) {
    std::map<std::wstring, RegistryValue> values;
    auto setup = L"\"" + (paths.app / L"PiuCompanionSetup.exe").wstring() + L"\"";
    for (const auto& [name, value] : std::vector<std::pair<std::wstring, std::wstring>>{
        {L"DisplayName", L"XSanity PIU Scores Companion"}, {L"DisplayVersion", PIU_VERSION_W},
        {L"Publisher", L"mat100payette"}, {L"InstallLocation", paths.app.wstring()},
        {L"DisplayIcon", (paths.app / L"PiuCompanion.exe").wstring()}, {L"UninstallString", setup + L" --uninstall"},
        {L"ModifyPath", setup}}) values.emplace(name, text_value(value));
    values.emplace(L"NoRepair", int_value(1)); values.emplace(L"NoModify", int_value(0)); return values;
}
class Transaction {
    InstallHost& host_;
    Registration registry_;
    std::map<fs::path, std::optional<std::string>> files_;
    std::vector<fs::path> directories_;
    bool done_ = false;
public:
    explicit Transaction(InstallHost& host) : host_(host), registry_(host.registration()) {}
    void snapshot(const fs::path& path) {
        safe_path(path); if (files_.contains(path)) return;
        if (fs::exists(path) && !fs::is_regular_file(path)) throw Error("Setup expected a file: " + utf8(path.wstring()));
        files_.emplace(path, fs::exists(path) ? std::optional<std::string>(read(path, 64 * 1024 * 1024)) : std::nullopt);
    }
    void directory(const fs::path& path) {
        safe_path(path); std::vector<fs::path> missing; auto next = path;
        while (!next.empty() && !fs::exists(next)) { missing.push_back(next); next = next.parent_path(); }
        for (auto it = missing.rbegin(); it != missing.rend(); ++it) { fs::create_directory(*it); directories_.push_back(*it); }
    }
    void write(const fs::path& path, std::string_view bytes) { snapshot(path); directory(path.parent_path()); atomic_write(path, bytes); host_.checkpoint(); }
    void remove(const fs::path& path) { snapshot(path); if (fs::exists(path)) fs::remove(path); host_.checkpoint(); }
    void shortcut(const fs::path& path, const fs::path& target, const std::wstring& args) { snapshot(path); directory(path.parent_path()); host_.shortcut(path, target, args); host_.checkpoint(); }
    void registration(const Registration& data) { host_.registration(data); host_.checkpoint(); }
    void commit() { done_ = true; }
    void rollback() {
        std::string failures;
        for (const auto& [path, bytes] : files_) {
            try { if (bytes) atomic_write(path, *bytes); else if (fs::exists(path)) fs::remove(path); }
            catch (...) { failures += " " + utf8(path.wstring()); }
        }
        try { host_.registration(registry_); } catch (...) { failures += " uninstall registration"; }
        for (auto it = directories_.rbegin(); it != directories_.rend(); ++it) { std::error_code ignored; fs::remove(*it, ignored); }
        done_ = true; if (!failures.empty()) throw Error("Setup could not restore:" + failures);
    }
    ~Transaction() { if (!done_) { try { rollback(); } catch (...) {} } }
};
void remove_empty(const fs::path& path) { std::error_code ignored; fs::remove(path, ignored); }
}
InstallPaths InstallPaths::user() {
    auto local = known_folder(FOLDERID_LocalAppData);
    return {local / L"Programs" / L"XSanityPIUScoresHook", local / L"XSanityPIUScoresHook", known_folder(FOLDERID_Programs) / L"XSanity PIU Scores Companion"};
}
bool WindowsInstallHost::game_is_running() { return game_running(); }
void WindowsInstallHost::stop_companion() {
    Handle mutex(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, mutex_name_.c_str())); if (!mutex.get()) return;
    DWORD state = WaitForSingleObject(mutex.get(), 0);
    if (state == WAIT_OBJECT_0 || state == WAIT_ABANDONED) { ReleaseMutex(mutex.get()); return; }
    Handle event(OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name_.c_str()));
    if (!event.get()) throw Error("Close the existing PIU Companion from its tray menu, then apply the changes again.");
    DWORD pid = 0; auto window = FindWindowW(window_class_.c_str(), nullptr);
    if (window) GetWindowThreadProcessId(window, &pid);
    Handle process(pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr);
    if (pid && !process.get()) fail("Wait for companion shutdown");
    if (!SetEvent(event.get())) fail("Stop companion");
    state = WaitForSingleObject(mutex.get(), 60000);
    if (state != WAIT_OBJECT_0 && state != WAIT_ABANDONED) throw Error("The companion is still finishing a request. Close it, then apply the changes again.");
    ReleaseMutex(mutex.get());
    // Releasing the singleton mutex can precede CRT/DLL shutdown and image unmapping.
    if (process.get() && WaitForSingleObject(process.get(), 60000) != WAIT_OBJECT_0)
        throw Error("The companion has not exited yet. Close it, then apply the changes again.");
}
Registration WindowsInstallHost::registration() {
    Key key; LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, UninstallKey, 0, KEY_READ, &key.value);
    if (status == ERROR_FILE_NOT_FOUND) return std::nullopt; if (status != ERROR_SUCCESS) throw Error("Could not read the uninstall registration.");
    std::map<std::wstring, RegistryValue> values;
    for (DWORD index = 0;; ++index) {
        wchar_t name[256]; DWORD length = 256, size = 0, type = 0;
        status = RegEnumValueW(key.value, index, name, &length, nullptr, &type, nullptr, &size);
        if (status == ERROR_NO_MORE_ITEMS) break; if (status != ERROR_SUCCESS || size > 65536) throw Error("Invalid uninstall registration.");
        RegistryValue value; value.type = type; value.bytes.resize(size);
        if (RegQueryValueExW(key.value, name, nullptr, nullptr, value.bytes.data(), &size) != ERROR_SUCCESS) throw Error("Could not read the uninstall registration.");
        values.emplace(std::wstring(name, length), std::move(value));
    }
    return values;
}
void WindowsInstallHost::registration(const Registration& values) {
    LONG status = RegDeleteTreeW(HKEY_CURRENT_USER, UninstallKey);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) throw Error("Could not update the uninstall registration.");
    if (!values) return;
    Key key; if (RegCreateKeyExW(HKEY_CURRENT_USER, UninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key.value, nullptr) != ERROR_SUCCESS) throw Error("Could not create the uninstall registration.");
    for (const auto& [name, value] : *values) if (RegSetValueExW(key.value, name.c_str(), 0, value.type, value.bytes.data(), static_cast<DWORD>(value.bytes.size())) != ERROR_SUCCESS) throw Error("Could not save the uninstall registration.");
}
void WindowsInstallHost::shortcut(const fs::path& file, const fs::path& target, const std::wstring& args) {
    auto link = winrt::create_instance<IShellLinkW>(CLSID_ShellLink, CLSCTX_INPROC_SERVER);
    winrt::check_hresult(link->SetPath(target.c_str())); winrt::check_hresult(link->SetArguments(args.c_str()));
    winrt::check_hresult(link->SetWorkingDirectory(target.parent_path().c_str())); winrt::check_hresult(link->SetIconLocation(target.c_str(), 0));
    auto persisted = link.as<IPersistFile>(); winrt::check_hresult(persisted->Save(file.c_str(), TRUE));
}
bool Installer::installed() const {
    auto path = paths_.app / L"installation.json";
    if (!fs::exists(path)) return false;
    auto marker = parse(read(path, 65536)); return str(marker, L"product") == "XSanityPIUScoresHook" && number(marker, L"schema") == 1;
}
InstallSelection Installer::selection() const {
    if (installed()) {
        auto marker = parse(read(paths_.app / L"installation.json", 65536));
        return {wide(str(marker, L"gameRoot")), flag(marker, L"sync"), flag(marker, L"overlay")};
    }
    auto config = load_preferences(paths_.state);
    return {config.game_root.empty() ? fs::path(L"C:\\XSanity") : config.game_root, config.sync, false};
}
void Installer::apply(const InstallSelection& selection, const std::function<void(int, std::wstring_view)>& progress) {
    // UI notifications must not affect a file transaction or its rollback.
    auto report = [&](int percent, std::wstring_view status) { if (progress) { try { progress(percent, status); } catch (...) {} } };
    report(0, L"Checking your setup");
    bool keep = selection.sync || selection.overlay;
    bool existing = installed(); auto previous = this->selection();
    auto config = keep ? load_preferences(paths_.state) : Preferences{};
    auto old_root = existing ? previous.game_root : config.game_root;
    auto root = keep ? GameHook::validate(selection.game_root) : old_root;
    if (!keep && !existing) throw Error("Nothing is installed yet. Choose at least one component.");
    for (const auto& path : {paths_.app, paths_.state, paths_.menu}) safe_path(path);
    if (!existing) for (auto name : {L"PiuCompanion.exe", L"PiuCompanionSetup.exe", L"LICENSE.txt", L"installation.json"})
        if (fs::exists(paths_.app / name)) throw Error("The installation folder already contains an unrecognized file. Setup will leave it untouched.");
    if (!existing) for (auto name : {L"Play XSanity.lnk", L"Manage installation.lnk"})
        if (fs::exists(paths_.menu / name)) throw Error("The Start menu folder already contains an unrecognized shortcut. Setup will leave it untouched.");
    if (keep) hook_.preflight(root);
    auto same = [](const fs::path& left, const fs::path& right) { return _wcsicmp(fs::absolute(left).lexically_normal().c_str(), fs::absolute(right).lexically_normal().c_str()) == 0; };
    bool remove_old = !old_root.empty() && (!keep || !same(old_root, root));
    if (remove_old) {
        safe_path(GameHook::layer(old_root)); safe_path(GameHook::exports(old_root));
        if (fs::exists(GameHook::layer(old_root)) && !hook_.owned(GameHook::layer(old_root))) throw Error("The installed game export layer was modified. Restore it before removing or moving this installation.");
    }
    bool change_layer = remove_old || (keep && !hook_.owned(GameHook::layer(root)));
    if (change_layer && host_.game_is_running()) throw Error("Close XSanity before adding, moving, or removing its game connection.");
    // Finish any in-flight POST before reading the queue or backing up settings.
    report(15, L"Waiting for the companion to finish");
    host_.stop_companion(); if (keep) config = load_preferences(paths_.state);
    Transaction transaction(host_);
    try {
        report(30, keep ? L"Connecting XSanity" : L"Removing the game connection");
        if (remove_old) {
            transaction.remove(GameHook::layer(old_root));
            for (auto file : {L"current.json", L"result.json"}) transaction.remove(GameHook::exports(old_root) / file);
        }
        if (keep) {
            if (!hook_.owned(GameHook::layer(root))) transaction.write(GameHook::layer(root), hook_.source());
            transaction.directory(GameHook::exports(root));
            report(45, L"Saving your component choices");
            auto next = config; next.game_root = root; next.sync = selection.sync; next.overlay = selection.overlay;
            if ((!previous.sync && next.sync) || !existing || !same(old_root, root)) next.capture_after = now();
            transaction.snapshot(paths_.state / L"settings.json"); transaction.directory(paths_.state); save_preferences(paths_.state, next); host_.checkpoint();
            if (!next.sync && fs::exists(paths_.state / L"uploads.json")) {
                transaction.remove(paths_.state / L"uploads.json");
            }
            Object marker; put(marker, L"product", "XSanityPIUScoresHook"); put(marker, L"schema", 1); put(marker, L"version", PIU_VERSION);
            put(marker, L"gameRoot", utf8(root.wstring())); put(marker, L"sync", next.sync); put(marker, L"overlay", next.overlay);
            transaction.write(paths_.app / L"installation.json", encode(marker));
            report(60, L"Installing the companion");
            transaction.write(paths_.app / L"PiuCompanion.exe", app_bytes_);
            transaction.write(paths_.app / L"PiuCompanionSetup.exe", setup_bytes_);
            transaction.write(paths_.app / L"LICENSE.txt", license_);
            report(80, L"Creating Start menu shortcuts");
            transaction.shortcut(paths_.menu / L"Play XSanity.lnk", paths_.app / L"PiuCompanion.exe", L"");
            transaction.shortcut(paths_.menu / L"Manage installation.lnk", paths_.app / L"PiuCompanionSetup.exe", L"");
            report(90, L"Finishing installation");
            transaction.registration(product_registration(paths_));
        } else {
            report(60, L"Removing the companion and saved account data");
            transaction.registration(std::nullopt);
            for (auto file : {L"PiuCompanion.exe", L"PiuCompanionSetup.exe", L"LICENSE.txt"}) transaction.remove(paths_.app / file);
            for (auto file : {L"settings.json", L"uploads.json"}) transaction.remove(paths_.state / file);
            report(85, L"Removing Start menu shortcuts");
            for (auto file : {L"Play XSanity.lnk", L"Manage installation.lnk"}) transaction.remove(paths_.menu / file);
            transaction.remove(paths_.app / L"installation.json");
        }
        transaction.commit();
    } catch (...) {
        auto error = std::current_exception(); report(0, L"Restoring the previous installation"); transaction.rollback(); std::rethrow_exception(error);
    }
    if (remove_old) remove_empty(GameHook::exports(old_root));
    if (!keep) { remove_empty(paths_.app); remove_empty(paths_.state); remove_empty(paths_.menu); }
    report(100, keep ? L"Setup complete" : L"Removal complete");
}
fs::path maintenance_directory() {
    GUID guid{}; winrt::check_hresult(CoCreateGuid(&guid)); wchar_t text[40]; StringFromGUID2(guid, text, 40);
    return fs::temp_directory_path() / (L"PiuCompanionSetup-" + std::wstring(text));
}
bool is_maintenance_directory(const fs::path& directory) {
    auto path = fs::absolute(directory).lexically_normal(), temp = fs::absolute(fs::temp_directory_path()).lexically_normal();
    auto drive = path.root_name().wstring();
    if (drive.size() != 2 || drive[1] != L':') return false;
    if (!fs::equivalent(path.parent_path(), temp)) return false;
    auto name = path.filename().wstring(); constexpr std::wstring_view prefix = L"PiuCompanionSetup-";
    if (!name.starts_with(prefix) || name.size() != prefix.size() + 38) return false;
    GUID guid{}; return SUCCEEDED(CLSIDFromString(name.c_str() + prefix.size(), &guid));
}
void clean_maintenance_after_exit(const fs::path& directory) {
    if (!is_maintenance_directory(directory)) throw Error("Invalid maintenance cleanup location.");
    safe_path(directory); safe_path(directory / L"maintenance.exe"); safe_path(directory / L"cleanup.cmd");
    wchar_t system[MAX_PATH]; UINT count = GetSystemDirectoryW(system, MAX_PATH); if (!count || count >= MAX_PATH) fail("Find Windows cleanup tools");
    fs::path system_path(system); auto delay = utf8((system_path / L"ping.exe").wstring());
    if (delay.find_first_of("\"%\r\n") != std::string::npos) throw Error("Unexpected Windows system path.");
    // Only fixed filenames in a validated, private temporary directory reach the shell.
    // No user paths are inserted into deletion commands; rmdir never removes other files.
    std::string script = "@echo off\r\nfor /l %%N in (1,1,30) do (\r\n del /q maintenance.exe >nul 2>nul\r\n if not exist maintenance.exe goto finish\r\n \"" + delay + "\" -n 2 127.0.0.1 >nul 2>nul\r\n)\r\nexit /b 1\r\n:finish\r\n(\r\n del /q cleanup.cmd >nul 2>nul\r\n cd ..\r\n rmdir \"" + utf8(directory.filename().wstring()) + "\" >nul 2>nul\r\n)\r\n";
    atomic_write(directory / L"cleanup.cmd", script);
    auto shell = system_path / L"cmd.exe"; std::wstring command = L"\"" + shell.wstring() + L"\" /d /q /v:off /c cleanup.cmd";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
    if (!CreateProcessW(shell.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &process)) fail("Start installer cleanup");
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
}
}
