#pragma once
#include "game_hook.h"
#include "model.h"

namespace piu {
struct InstallPaths {
    fs::path app, state, menu;
    static InstallPaths user();
};

struct RegistryValue {
    DWORD type = REG_SZ;
    std::vector<BYTE> bytes;
};

using Registration = std::optional<std::map<std::wstring, RegistryValue>>;

class InstallHost {
public:
    virtual ~InstallHost() = default;
    virtual bool game_is_running() = 0;
    virtual void stop_companion() = 0;
    virtual Registration registration() = 0;
    virtual void registration(const Registration& values) = 0;
    virtual void shortcut(const fs::path& file, const fs::path& target, const std::wstring& args) = 0;

    virtual void checkpoint() {
    } // Tests inject failures after mutations.
};

class WindowsInstallHost final : public InstallHost {
    std::wstring mutex_name_, event_name_, window_class_;

public:
    explicit WindowsInstallHost(
        std::wstring mutex = AppMutex, std::wstring event = StopEvent, std::wstring window = WindowClass)
        : mutex_name_(std::move(mutex)), event_name_(std::move(event)), window_class_(std::move(window)) {
    }

    bool game_is_running() override;
    void stop_companion() override;
    Registration registration() override;
    void registration(const Registration& values) override;
    void shortcut(const fs::path& file, const fs::path& target, const std::wstring& args) override;
};

struct InstallSelection {
    fs::path game_root;
    bool sync = true, overlay = false;
};

class Installer {
    InstallPaths paths_;
    InstallHost& host_;
    GameHook hook_;
    std::string app_bytes_, setup_bytes_, license_;

public:
    Installer(InstallPaths paths,
        InstallHost& host,
        GameHook hook,
        std::string app,
        std::string setup,
        std::string license)
        : paths_(std::move(paths)), host_(host), hook_(std::move(hook)), app_bytes_(std::move(app)),
          setup_bytes_(std::move(setup)), license_(std::move(license)) {
    }

    bool installed() const;
    InstallSelection selection() const;
    void apply(
        const InstallSelection& selection, const std::function<void(int, std::wstring_view)>& progress = {});
};

fs::path maintenance_directory();
bool is_maintenance_directory(const fs::path& directory);
void clean_maintenance_after_exit(const fs::path& directory);
} // namespace piu
