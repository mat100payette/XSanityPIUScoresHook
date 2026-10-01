#include "install.h"
#include "version.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <algorithm>
#include <set>

namespace piu {
namespace {
constexpr wchar_t UninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\XSanityPIUScoresHook";

class SetupLock {
    Handle mutex_;

public:
    SetupLock() : mutex_(CreateMutexW(nullptr, FALSE, SetupOperationMutex)) {
        if (!mutex_.get()) {
            fail("Lock companion setup");
        }

        DWORD result = WaitForSingleObject(mutex_.get(), 0);
        if (result == WAIT_FAILED) {
            fail("Lock companion setup");
        }

        if (result == WAIT_TIMEOUT) {
            throw Error("Another setup operation is running. Let it finish before trying again.");
        }
    }

    ~SetupLock() {
        ReleaseMutex(mutex_.get());
    }
};

struct InstallationReceipt {
    InstallSelection selection;
    std::string hook_fingerprint;
    std::wstring hook_file;
    std::string bytes;
};

std::optional<InstallationReceipt> load_installation(const fs::path& folder) {
    auto path = folder / L"installation.json";
    safe_path(path);
    if (!fs::exists(path)) {
        return std::nullopt;
    }

    auto bytes = read(path, 65536);
    try {
        auto marker = parse(bytes);
        auto schema = marker.GetNamedNumber(L"schema");
        auto root_text = marker.GetNamedString(L"gameRoot");
        std::wstring root_name(root_text.c_str(), root_text.size());
        auto root = fs::path(root_name);
        bool sync = marker.GetNamedBoolean(L"sync");
        bool overlay = marker.GetNamedBoolean(L"overlay");
        if (marker.GetNamedString(L"product") != L"XSanityPIUScoresHook" || schema != 1 ||
            !root.is_absolute() || root_name.find(L'\0') != std::wstring::npos ||
            root.wstring().find_first_of(L"\"\r\n") != std::wstring::npos || (!sync && !overlay)) {
            throw Error("Invalid installation receipt.");
        }

        auto fingerprint = winrt::to_string(marker.GetNamedString(L"hookSha256"));
        if (fingerprint.size() != 64 || !std::all_of(fingerprint.begin(), fingerprint.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            })) {
            throw Error("Invalid exporter fingerprint.");
        }

        // Receipts written before the load-point fix refer to the auxiliary actor.
        auto hook_file = wide(str(marker, L"hookFile", "ScreenSystemLayer aux.lua"));
        if (hook_file != GameHook::LayerName && hook_file != L"ScreenSystemLayer aux.lua") {
            throw Error("Invalid exporter filename.");
        }

        return InstallationReceipt{
            {root, sync, overlay}, std::move(fingerprint), std::move(hook_file), std::move(bytes)};
    } catch (const Error&) {
        throw Error("The installation receipt is damaged or unsupported. Setup has not changed any files.");
    } catch (const winrt::hresult_error&) {
        throw Error("The installation receipt is damaged or unsupported. Setup has not changed any files.");
    }
}

bool same_folder(const fs::path& left, const fs::path& right) {
    if (left.empty() || right.empty()) {
        return false;
    }

    safe_path(left);
    safe_path(right);
    auto left_path = fs::absolute(left).lexically_normal();
    auto right_path = fs::absolute(right).lexically_normal();
    if (_wcsicmp(left_path.c_str(), right_path.c_str()) == 0) {
        return true;
    }

    if (!fs::exists(left_path) || !fs::exists(right_path)) {
        return false;
    }

    return fs::equivalent(left_path, right_path);
}

struct Key {
    HKEY value = nullptr;

    ~Key() {
        if (value) {
            RegCloseKey(value);
        }
    }
};

RegistryValue text_value(const std::wstring& text) {
    RegistryValue value;
    auto ptr = reinterpret_cast<const BYTE*>(text.c_str());
    value.bytes.assign(ptr, ptr + (text.size() + 1) * sizeof(wchar_t));
    return value;
}

RegistryValue int_value(DWORD data) {
    RegistryValue value;
    value.type = REG_DWORD;
    auto ptr = reinterpret_cast<const BYTE*>(&data);
    value.bytes.assign(ptr, ptr + sizeof(data));
    return value;
}

Registration product_registration(const InstallPaths& paths) {
    std::map<std::wstring, RegistryValue> values;
    auto setup = L"\"" + (paths.app / L"PiuCompanionSetup.exe").wstring() + L"\"";
    for (const auto& [name, value] :
        std::vector<std::pair<std::wstring, std::wstring>>{{L"DisplayName", L"XSanity PIU Scores Companion"},
            {L"DisplayVersion", PIU_VERSION_W},
            {L"Publisher", L"mat100payette"},
            {L"InstallLocation", paths.app.wstring()},
            {L"DisplayIcon", (paths.app / L"PiuCompanion.exe").wstring()},
            {L"UninstallString", setup + L" --uninstall"},
            {L"ModifyPath", setup}}) {
        values.emplace(name, text_value(value));
    }

    values.emplace(L"NoRepair", int_value(1));
    values.emplace(L"NoModify", int_value(0));
    return values;
}

class Transaction {
    InstallHost& host_;
    Registration registry_;
    std::map<fs::path, std::optional<std::string>> files_;
    std::vector<fs::path> directories_;
    std::set<fs::path> changed_files_;
    bool registry_changed_ = false;
    bool done_ = false;

public:
    explicit Transaction(InstallHost& host) : host_(host), registry_(host.registration()) {
    }

    void snapshot(const fs::path& path, size_t limit = 64 * 1024 * 1024) {
        safe_path(path);
        if (files_.contains(path)) {
            return;
        }

        if (fs::exists(path) && !fs::is_regular_file(path)) {
            throw Error("Setup expected a file: " + utf8(path.wstring()));
        }

        files_.emplace(path, fs::exists(path) ? std::optional<std::string>(read(path, limit)) : std::nullopt);
    }

    void expect_file(const fs::path& path, const std::optional<std::string>& expected) {
        snapshot(path, 65536);
        if (files_.at(path) != expected) {
            files_.erase(path);
            throw Error("The installation receipt changed during setup. Run setup again.");
        }
    }

    void expect_hook(const fs::path& path, std::string_view expected_fingerprint) {
        snapshot(path, 65536);
        const auto& bytes = files_.at(path);
        auto actual = bytes ? GameHook::fingerprint(*bytes) : std::string{};
        if (actual != expected_fingerprint) {
            files_.erase(path);
            throw Error("The game export layer changed during setup. Setup will leave it untouched.");
        }
    }

    void changed(const fs::path& path) {
        changed_files_.insert(path);
    }

    void directory(const fs::path& path) {
        safe_path(path);
        std::vector<fs::path> missing;
        auto next = path;
        while (!next.empty() && !fs::exists(next)) {
            missing.push_back(next);
            next = next.parent_path();
        }

        for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
            if (fs::create_directory(*it)) {
                directories_.push_back(*it);
            }
        }
    }

    void write(const fs::path& path, std::string_view bytes) {
        snapshot(path);
        directory(path.parent_path());
        changed(path);
        atomic_write(path, bytes);
        host_.checkpoint();
    }

    void remove(const fs::path& path) {
        snapshot(path);
        changed(path);
        if (fs::exists(path)) {
            fs::remove(path);
        }

        host_.checkpoint();
    }

    void shortcut(const fs::path& path, const fs::path& target, const std::wstring& args) {
        snapshot(path);
        directory(path.parent_path());
        changed(path);
        host_.shortcut(path, target, args);
        host_.checkpoint();
    }

    void registration(const Registration& data) {
        registry_changed_ = true;
        host_.registration(data);
        host_.checkpoint();
    }

    void commit() {
        done_ = true;
    }

    void rollback() {
        std::string failures;
        for (const auto& [path, bytes] : files_) {
            if (!changed_files_.contains(path)) {
                continue;
            }

            try {
                safe_path(path);
                if (bytes) {
                    atomic_write(path, *bytes);
                } else if (fs::exists(path)) {
                    fs::remove(path);
                }
            } catch (...) {
                failures += " " + utf8(path.wstring());
            }
        }

        if (registry_changed_) {
            try {
                host_.registration(registry_);
            } catch (...) {
                failures += " uninstall registration";
            }
        }

        for (auto it = directories_.rbegin(); it != directories_.rend(); ++it) {
            try {
                safe_path(*it);
                std::error_code ignored;
                fs::remove(*it, ignored);
            } catch (...) {
                failures += " " + utf8(it->wstring());
            }
        }

        done_ = true;
        if (!failures.empty()) {
            throw Error("Setup could not restore:" + failures);
        }
    }

    ~Transaction() {
        if (!done_) {
            try {
                rollback();
            } catch (...) {
            }
        }
    }
};

void remove_empty(const fs::path& path) {
    try {
        safe_path(path);
        std::error_code ignored;
        fs::remove(path, ignored);
    } catch (const Error&) {
        // Optional cleanup must not follow a newly introduced junction.
    }
}
} // namespace

InstallPaths InstallPaths::user() {
    auto local = known_folder(FOLDERID_LocalAppData);
    return {local / L"Programs" / L"XSanityPIUScoresHook",
        local / L"XSanityPIUScoresHook",
        known_folder(FOLDERID_Programs) / L"XSanity PIU Scores Companion"};
}

bool WindowsInstallHost::game_is_running() {
    return game_running();
}

void WindowsInstallHost::stop_companion() {
    Handle mutex(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, mutex_name_.c_str()));
    if (!mutex.get()) {
        return;
    }

    DWORD state = WaitForSingleObject(mutex.get(), 0);
    if (state == WAIT_OBJECT_0 || state == WAIT_ABANDONED) {
        ReleaseMutex(mutex.get());
        return;
    }

    Handle event(OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name_.c_str()));
    if (!event.get()) {
        throw Error("Close the existing PIU Companion from its tray menu, then apply the changes again.");
    }

    DWORD pid = 0;
    auto window = FindWindowW(window_class_.c_str(), nullptr);
    if (window) {
        GetWindowThreadProcessId(window, &pid);
    }

    Handle process(pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr);
    if (pid && !process.get()) {
        fail("Wait for companion shutdown");
    }

    if (!SetEvent(event.get())) {
        fail("Stop companion");
    }

    state = WaitForSingleObject(mutex.get(), 60000);
    if (state != WAIT_OBJECT_0 && state != WAIT_ABANDONED) {
        throw Error("The companion is still finishing a request. Close it, then apply the changes again.");
    }

    ReleaseMutex(mutex.get());
    // Releasing the singleton mutex can precede CRT/DLL shutdown and image unmapping.
    if (process.get() && WaitForSingleObject(process.get(), 60000) != WAIT_OBJECT_0) {
        throw Error("The companion has not exited yet. Close it, then apply the changes again.");
    }
}

Registration WindowsInstallHost::registration() {
    Key key;
    LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, UninstallKey, 0, KEY_READ, &key.value);
    if (status == ERROR_FILE_NOT_FOUND) {
        return std::nullopt;
    }

    if (status != ERROR_SUCCESS) {
        throw Error("Could not read the uninstall registration.");
    }

    std::map<std::wstring, RegistryValue> values;
    for (DWORD index = 0;; ++index) {
        wchar_t name[256];
        DWORD length = 256, size = 0, type = 0;
        status = RegEnumValueW(key.value, index, name, &length, nullptr, &type, nullptr, &size);
        if (status == ERROR_NO_MORE_ITEMS) {
            break;
        }

        if (status != ERROR_SUCCESS || size > 65536) {
            throw Error("Invalid uninstall registration.");
        }

        RegistryValue value;
        value.type = type;
        value.bytes.resize(size);
        if (RegQueryValueExW(key.value, name, nullptr, nullptr, value.bytes.data(), &size) != ERROR_SUCCESS) {
            throw Error("Could not read the uninstall registration.");
        }

        values.emplace(std::wstring(name, length), std::move(value));
    }

    return values;
}

void WindowsInstallHost::registration(const Registration& values) {
    LONG status = RegDeleteTreeW(HKEY_CURRENT_USER, UninstallKey);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        throw Error("Could not update the uninstall registration.");
    }

    if (!values) {
        return;
    }

    Key key;
    if (RegCreateKeyExW(
            HKEY_CURRENT_USER, UninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key.value, nullptr) !=
        ERROR_SUCCESS) {
        throw Error("Could not create the uninstall registration.");
    }

    for (const auto& [name, value] : *values) {
        if (RegSetValueExW(key.value,
                name.c_str(),
                0,
                value.type,
                value.bytes.data(),
                static_cast<DWORD>(value.bytes.size())) != ERROR_SUCCESS) {
            throw Error("Could not save the uninstall registration.");
        }
    }
}

void WindowsInstallHost::shortcut(const fs::path& file, const fs::path& target, const std::wstring& args) {
    auto link = winrt::create_instance<IShellLinkW>(CLSID_ShellLink, CLSCTX_INPROC_SERVER);
    winrt::check_hresult(link->SetPath(target.c_str()));
    winrt::check_hresult(link->SetArguments(args.c_str()));
    winrt::check_hresult(link->SetWorkingDirectory(target.parent_path().c_str()));
    winrt::check_hresult(link->SetIconLocation(target.c_str(), 0));
    auto persisted = link.as<IPersistFile>();
    winrt::check_hresult(persisted->Save(file.c_str(), TRUE));
}

bool Installer::installed() const {
    return load_installation(paths_.app).has_value();
}

InstallSelection Installer::selection() const {
    if (auto receipt = load_installation(paths_.app)) {
        return receipt->selection;
    }

    auto config = load_preferences(paths_.state);
    return {config.game_root.empty() ? fs::path(L"C:\\XSanity") : config.game_root, config.sync, false};
}

void Installer::apply(
    const InstallSelection& selection, const std::function<void(int, std::wstring_view)>& progress) {
    SetupLock lock;
    // UI notifications must not affect a file transaction or its rollback.
    auto report = [&](int percent, std::wstring_view status) {
        if (progress) {
            try {
                progress(percent, status);
            } catch (...) {
            }
        }
    };
    report(0, L"Checking your setup");
    auto selected_hook = hook_.configured(selection.sync, selection.overlay);
    bool keep = selection.sync || selection.overlay;
    auto receipt = load_installation(paths_.app);
    bool existing = receipt.has_value();
    auto config = keep ? load_preferences(paths_.state) : Preferences{};
    auto previous = receipt ? receipt->selection : InstallSelection{config.game_root, config.sync, false};
    auto old_root = existing ? previous.game_root : config.game_root;
    auto root = keep ? GameHook::validate(selection.game_root) : old_root;
    if (!keep && !existing) {
        throw Error("Nothing is installed yet. Choose at least one component.");
    }

    for (const auto& path : {paths_.app, paths_.state, paths_.menu}) {
        safe_path(path);
    }

    if (!existing) {
        for (auto name :
            {L"PiuCompanion.exe", L"PiuCompanionSetup.exe", L"LICENSE.txt", L"installation.json"}) {
            if (fs::exists(paths_.app / name)) {
                throw Error("The installation folder already contains an unrecognized file. Setup will leave "
                            "it untouched.");
            }
        }

        for (auto name : {L"Play XSanity.lnk", L"Manage installation.lnk"}) {
            if (fs::exists(paths_.menu / name)) {
                throw Error("The Start menu folder already contains an unrecognized shortcut. Setup will "
                            "leave it untouched.");
            }
        }
    }

    // The user-selected folder may contain the recorded exporter after a move or copy.
    auto installed_fingerprint = receipt ? receipt->hook_fingerprint : std::string{};
    bool moving = keep && !old_root.empty() && !same_folder(old_root, root);
    HookStatus destination;
    if (keep) {
        destination = selected_hook.preflight(root, installed_fingerprint);
    }

    auto old_layer = GameHook::layer(old_root);
    if (receipt) {
        old_layer.replace_filename(receipt->hook_file);
    }

    bool changing_file = receipt && receipt->hook_file != GameHook::LayerName;
    bool remove_old = !old_root.empty() && (!keep || moving || changing_file);
    std::map<fs::path, HookStatus> old_hooks;
    auto remember_old = [&](const fs::path& path) {
        auto status = selected_hook.inspect(path, installed_fingerprint);
        if (status.state == HookState::Conflict) {
            throw Error("The installed game export layer was modified. Restore it before removing or moving "
                        "this installation.");
        }

        old_hooks.emplace(path, std::move(status));
    };
    if (remove_old) {
        safe_path(GameHook::exports(old_root));
        remember_old(old_layer);
    }

    if (changing_file && moving) {
        // A moved or copied game may carry the receipt-owned actor at its former filename.
        auto moved_layer = GameHook::layer(root);
        moved_layer.replace_filename(receipt->hook_file);
        remember_old(moved_layer);
    }

    bool replace_hook = keep && destination.state != HookState::Current;
    bool change_layer = remove_old || replace_hook;
    if (change_layer && host_.game_is_running()) {
        throw Error("Close XSanity before adding, updating, moving, or removing its game connection.");
    }

    // Finish any in-flight POST before reading the queue or backing up settings.
    report(15, L"Waiting for the companion to finish");
    host_.stop_companion();
    if (keep) {
        config = load_preferences(paths_.state);
    }

    bool old_changed = std::any_of(old_hooks.begin(), old_hooks.end(), [&](const auto& entry) {
        return selected_hook.inspect(entry.first, installed_fingerprint) != entry.second;
    });
    if ((keep && selected_hook.preflight(root, installed_fingerprint) != destination) || old_changed) {
        throw Error("The game export layer changed during setup. Run setup again.");
    }

    if (change_layer && host_.game_is_running()) {
        throw Error("XSanity started during setup. Close it, then apply the changes again.");
    }

    Transaction transaction(host_);
    try {
        transaction.expect_file(paths_.app / L"installation.json",
            receipt ? std::optional<std::string>(receipt->bytes) : std::nullopt);
        report(30, keep ? L"Connecting XSanity" : L"Removing the game connection");
        for (const auto& [path, status] : old_hooks) {
            transaction.expect_hook(path, status.fingerprint);
            transaction.remove(path);
        }

        if (remove_old) {
            for (auto file : {L"current.json", L"result.json"}) {
                auto path = GameHook::exports(old_root) / file;
                if (fs::exists(path)) {
                    transaction.remove(path);
                }
            }
        }

        if (keep) {
            transaction.expect_hook(GameHook::layer(root), destination.fingerprint);
            if (replace_hook) {
                transaction.write(GameHook::layer(root), selected_hook.source());
            }

            if (replace_hook || moving) {
                for (auto file : {L"current.json", L"result.json"}) {
                    auto path = GameHook::exports(root) / file;
                    if (fs::exists(path)) {
                        transaction.remove(path);
                    }
                }
            }

            report(45, L"Saving your component choices");
            auto next = config;
            next.game_root = root;
            next.sync = selection.sync;
            next.overlay = selection.overlay;
            if (replace_hook || moving || (!previous.sync && next.sync) || !existing) {
                next.capture_after = now();
            }

            transaction.snapshot(paths_.state / L"settings.json");
            transaction.directory(paths_.state);
            transaction.changed(paths_.state / L"settings.json");
            save_preferences(paths_.state, next);
            host_.checkpoint();
            if (!next.sync && fs::exists(paths_.state / L"uploads.json")) {
                transaction.remove(paths_.state / L"uploads.json");
            }

            report(60, L"Installing the companion");
            transaction.write(paths_.app / L"PiuCompanion.exe", app_bytes_);
            transaction.write(paths_.app / L"PiuCompanionSetup.exe", setup_bytes_);
            transaction.write(paths_.app / L"LICENSE.txt", license_);
            report(80, L"Creating Start menu shortcuts");
            transaction.shortcut(paths_.menu / L"Play XSanity.lnk", paths_.app / L"PiuCompanion.exe", L"");
            transaction.shortcut(
                paths_.menu / L"Manage installation.lnk", paths_.app / L"PiuCompanionSetup.exe", L"");
            report(90, L"Finishing installation");
            transaction.registration(product_registration(paths_));

            // Commit the receipt last, after the hook and companion have been installed.
            Object marker;
            put(marker, L"product", "XSanityPIUScoresHook");
            put(marker, L"schema", 1);
            put(marker, L"version", PIU_VERSION);
            put(marker, L"gameRoot", utf8(root.wstring()));
            put(marker, L"hookSha256", selected_hook.fingerprint());
            put(marker, L"hookFile", utf8(GameHook::LayerName));
            put(marker, L"sync", next.sync);
            put(marker, L"overlay", next.overlay);
            transaction.write(paths_.app / L"installation.json", encode(marker));
        } else {
            report(60, L"Removing the companion and saved account data");
            transaction.registration(std::nullopt);
            for (auto file : {L"PiuCompanion.exe", L"PiuCompanionSetup.exe", L"LICENSE.txt"}) {
                transaction.remove(paths_.app / file);
            }

            for (auto file : {L"settings.json", L"uploads.json"}) {
                transaction.remove(paths_.state / file);
            }

            report(85, L"Removing Start menu shortcuts");
            for (auto file : {L"Play XSanity.lnk", L"Manage installation.lnk"}) {
                transaction.remove(paths_.menu / file);
            }

            transaction.remove(paths_.app / L"installation.json");
        }

        transaction.commit();
    } catch (...) {
        auto error = std::current_exception();
        report(0, L"Restoring the previous installation");
        transaction.rollback();
        std::rethrow_exception(error);
    }

    if (remove_old && (!keep || moving)) {
        remove_empty(GameHook::exports(old_root));
    }

    if (keep) {
        remove_empty(GameHook::exports(root));
    }

    if (!keep) {
        remove_empty(paths_.app);
        remove_empty(paths_.state);
        remove_empty(paths_.menu);
    }

    report(100, keep ? L"Setup complete" : L"Removal complete");
}

} // namespace piu
