#include "support.h"
#include "update.h"
#include "preview.h"
#include "game_hook.h"
#include "maintenance.h"
#include "ui.h"
#include "version.h"
#include <iostream>

using namespace piu;

namespace piu::test {
namespace {
std::string payload() {
    std::string bytes(256, '\0');
    bytes.replace(0, 2, "MZ");
    bytes[60] = 64;
    bytes.replace(64, 4, "PE\0\0", 4);
    bytes[68] = 0x64;
    bytes[69] = static_cast<char>(0x86);
    bytes[88] = 0x0b;
    bytes[89] = 0x02;
    return bytes;
}

struct ReleaseFixture {
    std::string version, bytes = payload(), checksum;
    Object metadata;
    UpdateRelease release;

    explicit ReleaseFixture(std::string value = "1.2.3") : version(std::move(value)) {
        auto name = "XSanityPIUScoresHook-v" + version + "-win-x64-setup.exe";
        checksum = sha256(bytes) + "  " + name + "\n";
        put(metadata, L"tag_name", "v" + version);
        put(metadata, L"draft", false);
        put(metadata, L"prerelease", false);
        Array assets;
        for (bool hash : {false, true}) {
            Object asset;
            auto file = name + (hash ? ".sha256" : "");
            put(asset, L"name", file);
            put(asset, L"state", "uploaded");
            put(asset,
                L"browser_download_url",
                "https://github.com/mat100payette/XSanityPIUScoresHook/releases/download/v" + version + "/" +
                    file);
            put(asset, L"size", static_cast<int>(hash ? checksum.size() : bytes.size()));
            assets.Append(asset);
        }

        metadata.Insert(L"assets", assets);
        release = *select_update(metadata, "0.0.0");
    }

    UpdateClient client() const {
        return UpdateClient(
            [this](
                const std::string& url, size_t limit, std::stop_token stop, const UpdateProgress& progress) {
                check_update_cancelled(stop);
                if (url == LatestReleaseUrl) {
                    return encode(metadata);
                }

                if (url == release.checksum.url) {
                    check(limit == checksum.size(), "checksum request is bounded");
                    return checksum;
                }

                check(url == release.installer.url, "download uses exact release asset");
                check(limit == bytes.size(), "installer request is bounded");
                if (progress) {
                    progress(bytes.size(), limit);
                }

                return bytes;
            });
    }
};

void metadata_checks() {
    check(sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "SHA-256 matches standard vector");
    check(sha256("a\r\n") != sha256("a\n"), "binary integrity does not normalize line endings");
    ReleaseFixture newer("0.10.0");
    check(select_update(newer.metadata, "0.9.9").has_value(), "numeric version comparison");
    check(!select_update(newer.metadata, "0.10.0"), "same version is not an update");
    check(!select_update(newer.metadata, "1.0.0"), "older release cannot downgrade installed build");
    for (const auto* bad : {"0.1", "0.1.2.3", "-1.2.3", "1.2.3-beta", "01.2.3", "1.2.65535", "x.y.z"}) {
        auto release = parse(encode(newer.metadata));
        put(release, L"tag_name", std::string("v") + bad);
        rejects(
            [&] {
                select_update(release, "0.0.0");
            },
            "invalid release version rejected");
    }

    for (const auto* key : {L"draft", L"prerelease"}) {
        auto release = parse(encode(newer.metadata));
        put(release, key, true);
        rejects(
            [&] {
                select_update(release, "0.0.0");
            },
            "unpublished and prerelease builds rejected");
    }

    for (int failure = 0; failure < 7; ++failure) {
        auto release = parse(encode(newer.metadata));
        auto assets = release.GetNamedArray(L"assets");
        auto asset = assets.GetAt(0).GetObject();
        switch (failure) {
        case 0:
            assets.RemoveAt(1);
            break;
        case 1:
            assets.Append(asset);
            break;
        case 2:
            put(asset, L"browser_download_url", "https://example.com/setup.exe");
            break;
        case 3:
            put(asset, L"state", "new");
            break;
        case 4:
            put(asset, L"size", -1);
            break;
        case 5:
            put(asset, L"size", static_cast<int>(MaxInstallerSize + 1));
            break;
        case 6:
            asset.Insert(L"size", Value::CreateNumberValue(1.5));
            break;
        }

        rejects(
            [&] {
                select_update(release, "0.0.0");
            },
            "incomplete or ambiguous release rejected");
    }

    for (const auto& url : {std::string(LatestReleaseUrl),
             newer.release.installer.url,
             std::string(
                 "https://release-assets.githubusercontent.com/github-production-release-asset/123?a=b"),
             std::string("https://objects.githubusercontent.com/github-production-release-asset/123")}) {
        check(allowed_update_url(url), "official release and CDN addresses accepted");
    }

    for (const auto* url : {"http://github.com/mat100payette/XSanityPIUScoresHook/releases/download/v1/a",
             "https://github.com/another/repo/releases/download/v1/a",
             "https://api.github.com/user",
             "https://github.com.evil.example/setup.exe",
             "https://user@release-assets.githubusercontent.com/a",
             "https://release-assets.githubusercontent.com:444/a",
             "https://release-assets.githubusercontent.com/a#fragment",
             "https://release-assets.githubusercontent.com/a\r\nOther: header",
             "file:///C:/setup.exe"}) {
        check(!allowed_update_url(url), "unsafe update addresses refused");
    }
}

void download_checks() {
    ReleaseFixture fixture;
    check(verify_update(fixture.release, fixture.bytes, fixture.checksum) == sha256(fixture.bytes),
        "published checksum verifies the exact installer");
    auto bad = fixture.bytes;
    bad.back() = 'x';
    rejects(
        [&] {
            verify_update(fixture.release, bad, fixture.checksum);
        },
        "checksum mismatch refused");
    rejects(
        [&] {
            verify_update(fixture.release, fixture.bytes.substr(1), fixture.checksum);
        },
        "truncated download refused");
    auto checksum = fixture.checksum;
    checksum[checksum.size() - 2] = 'x';
    rejects(
        [&] {
            verify_update(fixture.release, fixture.bytes, checksum);
        },
        "checksum filename must match");
    auto invalid = fixture.bytes;
    invalid[0] = 'x';
    auto hash = sha256(invalid) + "  " + fixture.release.installer.name + "\n";
    rejects(
        [&] {
            verify_update(fixture.release, invalid, hash);
        },
        "nonexecutable payload refused");

    auto client = fixture.client();
    check(client.check("0.0.0")->version == fixture.version, "client checks published version");
    fs::path staged;
    bool progress = false;
    {
        auto update = client.download(fixture.release, {}, [&](size_t received, size_t total) {
            progress = received == total && total == fixture.bytes.size();
        });
        staged = update->file();
        check(is_maintenance_directory(staged.parent_path()) && read(staged) == fixture.bytes,
            "verified installer staged outside game using maintenance cleanup contract");
        check(progress, "download reports progress");
        rejects(
            [&] {
                update->open([](const fs::path&, std::wstring_view) {
                    throw Error("launch failed");
                });
            },
            "launch failure is reported");
    }

    check(!fs::exists(staged.parent_path()), "cancelled or failed handoff removes its temporary folder");
    {
        auto update = client.download(fixture.release);
        staged = update->file();
        atomic_write(staged, "modified");
        bool opened = false;
        rejects(
            [&] {
                update->open([&](const fs::path&, std::wstring_view) {
                    opened = true;
                });
            },
            "changed installer rejected immediately before launch");
        check(!opened, "unverified bytes are never executed");
    }

    check(!fs::exists(staged.parent_path()), "changed payload cleaned up");
    {
        auto update = client.download(fixture.release);
        staged = update->file();
        update->open([&](const fs::path& file, std::wstring_view args) {
            check(file == staged && args == L"--maintenance-copy", "handoff reuses normal maintenance setup");
            Handle overwrite(CreateFileW(
                file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
            check(
                overwrite.get() == INVALID_HANDLE_VALUE, "verified installer locked through process launch");
        });
    }

    check(fs::exists(staged), "successful handoff leaves cleanup to the running installer");
    safe_path(staged);
    fs::remove(staged);
    fs::remove(staged.parent_path());

    std::stop_source stopped;
    stopped.request_stop();
    rejects(
        [&] {
            client.check("0.0.0", stopped.get_token());
        },
        "cancel before check");
    rejects(
        [&] {
            client.download(fixture.release, stopped.get_token());
        },
        "cancel before download");
    std::stop_source mid;
    int calls = 0;
    UpdateClient cancelled([&](const std::string&, size_t, std::stop_token, const UpdateProgress&) {
        ++calls;
        mid.request_stop();
        return fixture.checksum;
    });
    rejects(
        [&] {
            cancelled.download(fixture.release, mid.get_token());
        },
        "cancel between checksum and binary");
    check(calls == 1, "cancellation prevents subsequent requests");
    UpdateClient offline(
        [](const std::string&, size_t, std::stop_token, const UpdateProgress&) -> std::string {
            throw Error("offline");
        });
    rejects(
        [&] {
            offline.check("0.0.0");
        },
        "connection failure reported");
    UpdateClient unpublished([](const std::string&, size_t, std::stop_token, const UpdateProgress&) {
        return std::string{};
    });
    rejects(
        [&] {
            unpublished.check("0.0.0");
        },
        "missing release must not report up to date");
}

enum class DialogCase {
    Install,
    CancelCheck,
    ShutdownCheck,
    Offline,
    Current,
    Decline,
    CancelDownload,
    ShutdownDownload
};

// Drive the real native dialog with fake downloads. Never launch an executable.
struct DialogDriver {
    HWND active = nullptr;
    HANDLE shutdown;
    DialogCase mode;
    bool clicked = false, cancelled = false, timed_out = false, captured = false;
    std::exception_ptr error;
    ULONGLONG deadline = GetTickCount64() + 10000;
    static inline DialogDriver* current = nullptr;

    static bool has_button(HWND dialog, const wchar_t* label) {
        struct Search {
            const wchar_t* label;
            bool found = false;
        } search{label};

        EnumChildWindows(
            dialog,
            [](HWND child, LPARAM data) -> BOOL {
                auto& search = *reinterpret_cast<Search*>(data);
                wchar_t text[256]{};
                GetWindowTextW(child, text, 256);
                std::wstring caption(text);
                std::erase(caption, L'&');
                if (caption == search.label && IsWindowVisible(child)) {
                    search.found = true;
                }

                return TRUE;
            },
            reinterpret_cast<LPARAM>(&search));
        return search.found;
    }

    void capture(const wchar_t* name) {
        save_window_preview(active, executable().parent_path() / name, true, true);
    }

    void drive() {
        if (GetTickCount64() > deadline) {
            timed_out = true;
            SetEvent(shutdown);
            return;
        }

        if (!active) {
            return;
        }

        if ((mode == DialogCase::CancelCheck || mode == DialogCase::ShutdownCheck) && !cancelled) {
            cancelled = true;
            if (mode == DialogCase::CancelCheck) {
                PostMessageW(active, TDM_CLICK_BUTTON, IDCANCEL, 0);
            } else {
                SetEvent(shutdown);
            }
        } else if (has_button(active, L"Download and open setup") && !clicked) {
            clicked = true;
            if (mode == DialogCase::Decline) {
                PostMessageW(active, TDM_CLICK_BUTTON, IDCANCEL, 0);
            } else {
                capture(L"update-available.png");
                PostMessageW(active, TDM_CLICK_BUTTON, 100, 0);
            }
        } else if (clicked && !captured && !has_button(active, L"Download and open setup") &&
                   !has_button(active, L"Close")) {
            captured = true;
            capture(L"update-downloading.png");
            if (mode == DialogCase::CancelDownload) {
                PostMessageW(active, TDM_CLICK_BUTTON, IDCANCEL, 0);
            } else if (mode == DialogCase::ShutdownDownload) {
                SetEvent(shutdown);
            }
        } else if (has_button(active, L"Close")) {
            capture(mode == DialogCase::Offline ? L"update-error.png" : L"update-current.png");
            PostMessageW(active, TDM_CLICK_BUTTON, IDCLOSE, 0);
        }
    }

    static void CALLBACK timer(HWND, UINT, UINT_PTR, DWORD) {
        try {
            current->drive();
        } catch (...) {
            current->error = std::current_exception();
            SetEvent(current->shutdown);
        }
    }
};

void dialog_checks() {
    for (auto mode : {DialogCase::Install,
             DialogCase::CancelCheck,
             DialogCase::ShutdownCheck,
             DialogCase::Offline,
             DialogCase::Current,
             DialogCase::Decline,
             DialogCase::CancelDownload,
             DialogCase::ShutdownDownload}) {
        ReleaseFixture fixture(mode == DialogCase::Current ? "0.0.1" : "60000.0.0");
        std::atomic<bool> cancelled = false;
        int downloads = 0;
        UpdateClient client([&](const std::string& url, size_t, std::stop_token stop, const UpdateProgress&) {
            bool downloading = url == fixture.release.installer.url;
            if (downloading) {
                ++downloads;
            }

            if (mode == DialogCase::CancelCheck || mode == DialogCase::ShutdownCheck ||
                ((mode == DialogCase::CancelDownload || mode == DialogCase::ShutdownDownload) &&
                    downloading)) {
                while (!stop.stop_requested()) {
                    Sleep(10);
                }

                cancelled = true;
                throw UpdateCancelled{};
            }

            if (mode == DialogCase::Offline) {
                throw Error("Simulated offline connection.");
            }

            return url == LatestReleaseUrl               ? encode(fixture.metadata)
                   : url == fixture.release.checksum.url ? fixture.checksum
                                                         : fixture.bytes;
        });
        Handle shutdown(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        DialogDriver driver{nullptr, shutdown.get(), mode};
        DialogDriver::current = &driver;
        auto timer = SetTimer(nullptr, 0, 50, DialogDriver::timer);
        if (!timer) {
            fail("Start update dialog test timer");
        }

        bool opened = false;
        fs::path staged;
        try {
            show_updates(
                nullptr, shutdown.get(), driver.active, client, [&](const fs::path& file, std::wstring_view) {
                    opened = true;
                    staged = file;
                });
        } catch (...) {
            KillTimer(nullptr, timer);
            DialogDriver::current = nullptr;
            throw;
        }

        KillTimer(nullptr, timer);
        DialogDriver::current = nullptr;
        if (driver.error) {
            std::rethrow_exception(driver.error);
        }

        if (driver.timed_out) {
            throw Error("Update dialog timed out in test mode " + std::to_string(static_cast<int>(mode)));
        }

        check(!driver.active, "native update dialog releases its window");
        check(opened == (mode == DialogCase::Install), "only confirmed successful download opens setup");
        check(cancelled == (mode == DialogCase::CancelCheck || mode == DialogCase::ShutdownCheck ||
                               mode == DialogCase::CancelDownload || mode == DialogCase::ShutdownDownload),
            "cancel and companion shutdown stop checking and downloading");
        check(downloads == ((mode == DialogCase::Install || mode == DialogCase::CancelDownload ||
                                mode == DialogCase::ShutdownDownload)
                                   ? 1
                                   : 0),
            "downloads require confirmation and a newer release");
        if (!staged.empty()) {
            safe_path(staged);
            fs::remove(staged);
            fs::remove(staged.parent_path());
        }
    }
}
} // namespace

void update_checks() {
    metadata_checks();
    download_checks();
    dialog_checks();
}

int update_network_check() {
    UpdateClient client;
    auto release = client.check("0.0.0");
    if (!release) {
        throw Error("No public release is available for the read-only network check.");
    }

    auto prepared = client.download(*release);
    std::cout << "Verified public release " << release->version
              << " using the production GitHub transport. Installer was not executed.\n";
    return 0;
}
} // namespace piu::test
