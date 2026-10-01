#include "update.h"
#include "game_hook.h"
#include "maintenance.h"
#include <array>
#include <charconv>
#include <cmath>

namespace piu {
namespace {
std::array<unsigned, 3> version_parts(std::string_view version) {
    std::array<unsigned, 3> parts{};
    for (size_t index = 0; index < parts.size(); ++index) {
        auto end = index == 2 ? version.size() : version.find('.');
        auto part = version.substr(0, end);
        if (part.empty() || (part.size() > 1 && part.front() == '0')) {
            throw Error("The release has an unsupported version number.");
        }

        auto [next, error] = std::from_chars(part.data(), part.data() + part.size(), parts[index]);
        if (error != std::errc{} || next != part.data() + part.size() || parts[index] > 65534 ||
            (index != 2 && end == std::string_view::npos)) {
            throw Error("The release has an unsupported version number.");
        }

        version.remove_prefix(part.size() + (index == 2 ? 0 : 1));
    }

    return parts;
}

UpdateAsset find_asset(const Array& assets, const std::string& name, const std::string& tag, size_t limit) {
    std::optional<UpdateAsset> found;
    const auto expected =
        "https://github.com/mat100payette/XSanityPIUScoresHook/releases/download/" + tag + "/" + name;
    for (const auto& value : assets) {
        auto asset = value.GetObject();
        if (asset.GetNamedString(L"name") != wide(name)) {
            continue;
        }

        auto size = asset.GetNamedNumber(L"size");
        if (found || asset.GetNamedString(L"state") != L"uploaded" ||
            asset.GetNamedString(L"browser_download_url") != wide(expected) || !std::isfinite(size) ||
            std::floor(size) != size || size <= 0 || size > static_cast<double>(limit)) {
            throw Error("The release installer details are invalid. Download it from GitHub instead.");
        }

        found = UpdateAsset{name, expected, static_cast<size_t>(size)};
    }

    if (!found) {
        throw Error("The release installer is not ready yet. Please try again later.");
    }

    return *found;
}

void check_executable(std::string_view bytes) {
    auto word = [&](size_t offset) {
        return static_cast<unsigned char>(bytes[offset]) |
               (static_cast<unsigned>(static_cast<unsigned char>(bytes[offset + 1])) << 8);
    };
    if (bytes.size() < 64 || bytes.substr(0, 2) != "MZ") {
        throw Error("The downloaded file is not a Windows installer.");
    }

    size_t offset = word(60) | (static_cast<size_t>(word(62)) << 16);
    if (offset > bytes.size() - 26 || bytes.substr(offset, 4) != std::string_view("PE\0\0", 4) ||
        word(offset + 4) != 0x8664 || word(offset + 24) != 0x20b) {
        throw Error("The downloaded file is not a Windows x64 installer.");
    }
}

void discard_download(const fs::path& directory) noexcept {
    try {
        if (is_maintenance_directory(directory)) {
            safe_path(directory);
            safe_path(directory / L"maintenance.exe");
            std::error_code ignored;
            fs::remove(directory / L"maintenance.exe", ignored);
            fs::remove(directory, ignored);
        }
    } catch (...) {
        // Never broaden cleanup to other files or follow a changed junction.
    }
}
} // namespace

void check_update_cancelled(std::stop_token stop) {
    if (stop.stop_requested()) {
        throw UpdateCancelled{};
    }
}

std::optional<UpdateRelease> select_update(const Object& release, std::string_view current_version) {
    auto current = version_parts(current_version);
    if (release.GetNamedBoolean(L"draft") || release.GetNamedBoolean(L"prerelease")) {
        throw Error("GitHub did not return a published stable release.");
    }

    auto tag = winrt::to_string(release.GetNamedString(L"tag_name"));
    if (!tag.starts_with('v')) {
        throw Error("The release has an unsupported version number.");
    }

    auto version = tag.substr(1);
    if (version_parts(version) <= current) {
        return std::nullopt;
    }

    auto name = "XSanityPIUScoresHook-" + tag + "-win-x64-setup.exe";
    auto assets = release.GetNamedArray(L"assets");
    return UpdateRelease{version,
        find_asset(assets, name, tag, MaxInstallerSize),
        find_asset(assets, name + ".sha256", tag, 1024)};
}

std::string verify_update(
    const UpdateRelease& release, std::string_view installer, std::string_view checksum) {
    if (installer.size() != release.installer.size || checksum.size() != release.checksum.size) {
        throw Error("The update download is incomplete. Please try again.");
    }

    if (checksum.ends_with("\n")) {
        checksum.remove_suffix(1);
        if (checksum.ends_with("\r")) {
            checksum.remove_suffix(1);
        }
    }

    auto digest = sha256(installer);
    if (checksum != digest + "  " + release.installer.name) {
        throw Error("The installer failed its checksum check. It has not been opened.");
    }

    check_executable(installer);
    return digest;
}

PreparedUpdate::PreparedUpdate(std::string_view bytes, std::string fingerprint)
    : directory_(maintenance_directory()), fingerprint_(std::move(fingerprint)) {
    safe_path(directory_);
    if (!fs::create_directory(directory_)) {
        throw Error("Could not create a private update folder.");
    }

    try {
        atomic_write(file(), bytes);
    } catch (...) {
        discard_download(directory_);
        throw;
    }
}

PreparedUpdate::~PreparedUpdate() {
    if (!handed_off_) {
        discard_download(directory_);
    }
}

fs::path PreparedUpdate::file() const {
    return directory_ / L"maintenance.exe";
}

void PreparedUpdate::open(const UpdateLauncher& launcher) {
    safe_path(file());
    Handle locked(CreateFileW(file().c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (locked.get() == INVALID_HANDLE_VALUE) {
        fail("Open verified installer");
    }

    if (sha256(read(file(), MaxInstallerSize)) != fingerprint_) {
        throw Error("The downloaded installer changed. Check for updates again.");
    }

    launcher(file(), L"--maintenance-copy");
    handed_off_ = true;
}

UpdateClient::UpdateClient(UpdateTransport transport)
    : transport_(transport ? std::move(transport) : update_http) {
}

std::optional<UpdateRelease> UpdateClient::check(
    std::string_view current_version, std::stop_token stop) const {
    check_update_cancelled(stop);
    auto bytes = transport_(LatestReleaseUrl, 1024 * 1024, stop, {});
    check_update_cancelled(stop);
    if (bytes.empty()) {
        throw Error("GitHub returned an empty release response. Please try again later.");
    }

    if (bytes.size() > 1024 * 1024) {
        throw Error("The release response exceeds the size limit.");
    }

    try {
        return select_update(parse(bytes), current_version);
    } catch (const winrt::hresult_error&) {
        throw Error("GitHub returned incomplete release details. Please try again later.");
    }
}

std::unique_ptr<PreparedUpdate> UpdateClient::download(
    const UpdateRelease& release, std::stop_token stop, const UpdateProgress& progress) const {
    check_update_cancelled(stop);
    auto checksum = transport_(release.checksum.url, release.checksum.size, stop, {});
    check_update_cancelled(stop);
    auto installer = transport_(release.installer.url, release.installer.size, stop, progress);
    check_update_cancelled(stop);
    auto fingerprint = verify_update(release, installer, checksum);
    check_update_cancelled(stop);
    return std::make_unique<PreparedUpdate>(installer, std::move(fingerprint));
}
} // namespace piu