#pragma once
#include <winsock2.h>
#include <windows.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace piu {
namespace fs = std::filesystem;
namespace json = winrt::Windows::Data::Json;
using Object = json::JsonObject;
using Array = json::JsonArray;
using Value = json::JsonValue;

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Handle {
    HANDLE value_ = nullptr;

public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {
    }

    ~Handle() {
        if (value_ && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    HANDLE get() const {
        return value_;
    }
};

std::wstring wide(std::string_view text);
std::string utf8(std::wstring_view text);
[[noreturn]] void fail(const char* operation);
std::string read(const fs::path& path, size_t limit = 4 * 1024 * 1024);
void atomic_write(const fs::path& path, std::string_view bytes);
uint64_t modified(const fs::path& path);
uint64_t now();
std::string iso_time(uint64_t ticks);
std::string base64(std::string_view bytes);
std::string protect(std::string_view token);
std::string unprotect(std::string_view cipher);
std::string resource(int id);
fs::path executable();
fs::path known_folder(const GUID& id);
bool game_running();
fs::path game_executable(const fs::path& root);
void launch(const fs::path& exe, std::wstring_view arguments = L"", const fs::path& cwd = {});
std::optional<fs::path> browse_folder(HWND owner, const fs::path& initial);
std::string normalized(std::string_view text);
void validate_mix(std::string_view mix);
void validate_token(std::string_view token);
Object parse(std::string_view text);
std::string encode(const Object& object);
std::string str(const Object& object, std::wstring_view key, std::string fallback = "");
int number(const Object& object, std::wstring_view key, int fallback = 0);
bool flag(const Object& object, std::wstring_view key, bool fallback = false);
void put(Object& object, std::wstring_view key, std::string_view text);
void put(Object& object, std::wstring_view key, int value);
void put(Object& object, std::wstring_view key, bool value);

inline void put(Object& object, std::wstring_view key, const char* text) {
    put(object, key, std::string_view(text));
}

inline constexpr wchar_t AppMutex[] = L"Local\\PIUCompanion";
inline constexpr wchar_t StopEvent[] = L"Local\\XSanityPIUScoresHook.Stop";
inline constexpr wchar_t WindowClass[] = L"XSanityPIUScoresHook.Tray";
inline constexpr UINT LaunchGameMessage = WM_APP + 1;
inline constexpr int HookResource = 101, OverlayResource = 102, AppResource = 103;
} // namespace piu
