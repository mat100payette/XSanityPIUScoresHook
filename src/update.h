#pragma once
#include "platform.h"
#include <stop_token>

namespace piu {
inline constexpr char LatestReleaseUrl[] =
    "https://api.github.com/repos/mat100payette/XSanityPIUScoresHook/releases/latest";
inline constexpr size_t MaxInstallerSize = 64 * 1024 * 1024;

struct UpdateAsset {
    std::string name, url;
    size_t size = 0;
};

struct UpdateRelease {
    std::string version;
    UpdateAsset installer, checksum;
};

using UpdateProgress = std::function<void(size_t, size_t)>;
using UpdateTransport =
    std::function<std::string(const std::string&, size_t, std::stop_token, const UpdateProgress&)>;
using UpdateLauncher = std::function<void(const fs::path&, std::wstring_view)>;

class UpdateCancelled : public Error {
public:
    UpdateCancelled() : Error("Update cancelled.") {
    }
};

void check_update_cancelled(std::stop_token stop);
bool allowed_update_url(const std::string& url);
std::string update_http(
    const std::string& url, size_t limit, std::stop_token stop, const UpdateProgress& progress);
std::optional<UpdateRelease> select_update(const Object& release, std::string_view current_version);
std::string verify_update(
    const UpdateRelease& release, std::string_view installer, std::string_view checksum);

// Owns only the downloaded installer in a private temporary directory.
// On successful launch, the installer's existing maintenance cleanup takes ownership.
class PreparedUpdate {
    fs::path directory_;
    std::string fingerprint_;
    bool handed_off_ = false;

public:
    PreparedUpdate(std::string_view bytes, std::string fingerprint);
    ~PreparedUpdate();
    PreparedUpdate(const PreparedUpdate&) = delete;
    PreparedUpdate& operator=(const PreparedUpdate&) = delete;
    fs::path file() const;
    void open(const UpdateLauncher& launcher);
};

class UpdateClient {
    UpdateTransport transport_;

public:
    explicit UpdateClient(UpdateTransport transport = {});
    std::optional<UpdateRelease> check(std::string_view current_version, std::stop_token stop = {}) const;
    std::unique_ptr<PreparedUpdate> download(
        const UpdateRelease& release, std::stop_token stop = {}, const UpdateProgress& progress = {}) const;
};

void show_updates(
    HWND owner,
    HANDLE shutdown,
    HWND& active_dialog,
    const UpdateClient& client = UpdateClient{},
    const UpdateLauncher& launcher = [](const fs::path& file, std::wstring_view args) {
        launch(file, args);
    });
} // namespace piu