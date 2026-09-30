#include "platform.h"
#include <dpapi.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <cwctype>
#include <limits>

namespace piu {
std::wstring wide(std::string_view text) {
    if (text.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!count) throw Error("Invalid UTF-8.");
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}
std::string utf8(std::wstring_view text) { return winrt::to_string(text); }
[[noreturn]] void fail(const char* operation) {
    throw Error(std::string(operation) + " failed (Windows error " + std::to_string(GetLastError()) + ").");
}
std::string read(const fs::path& path, size_t limit) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) fail("Read file");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)) fail("Read file size");
    if (size.QuadPart < 0 || static_cast<uint64_t>(size.QuadPart) > limit) throw Error("File exceeds the size limit.");
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD got = 0;
    if (!ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr) || got != bytes.size()) fail("Read file");
    return bytes;
}
void atomic_write(const fs::path& path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    GUID guid{}; winrt::check_hresult(CoCreateGuid(&guid)); wchar_t unique[40]; StringFromGUID2(guid, unique, 40);
    fs::path temp = path.wstring() + L"." + unique + L".tmp";
    try {
        {
            Handle file(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (file.get() == INVALID_HANDLE_VALUE) fail("Write file");
            DWORD wrote = 0;
            if (!WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr) || wrote != bytes.size()) fail("Write file");
            if (!FlushFileBuffers(file.get())) fail("Flush file");
        }
        if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) fail("Replace file");
    } catch (...) { std::error_code ignored; fs::remove(temp, ignored); throw; }
}
uint64_t modified(const fs::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) fail("Read export time");
    return (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
}
uint64_t now() { FILETIME time{}; GetSystemTimeAsFileTime(&time); return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime; }
std::string iso_time(uint64_t ticks) {
    FILETIME time{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    SYSTEMTIME utc{};
    if (!FileTimeToSystemTime(&time, &utc)) fail("Convert play time");
    char text[40]{};
    sprintf_s(text, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds);
    return text;
}
std::string base64(std::string_view bytes) {
    DWORD count = 0;
    if (!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()),
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &count)) fail("Encode token");
    std::string result(count, '\0');
    if (!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()),
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, result.data(), &count)) fail("Encode token");
    result.resize(count); return result;
}
std::string protect(std::string_view token) {
    if (token.empty()) return {};
    DATA_BLOB input{static_cast<DWORD>(token.size()), reinterpret_cast<BYTE*>(const_cast<char*>(token.data()))}, output{};
    if (!CryptProtectData(&input, L"PIU Scores token", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) fail("Encrypt token");
    std::string result;
    try { result = base64({reinterpret_cast<char*>(output.pbData), output.cbData}); }
    catch (...) { LocalFree(output.pbData); throw; }
    SecureZeroMemory(output.pbData, output.cbData); LocalFree(output.pbData); return result;
}
std::string unprotect(std::string_view cipher) {
    if (cipher.empty()) return {};
    DWORD count = 0;
    if (!CryptStringToBinaryA(cipher.data(), static_cast<DWORD>(cipher.size()), CRYPT_STRING_BASE64, nullptr, &count, nullptr, nullptr)) fail("Decode token");
    std::vector<BYTE> bytes(count);
    if (!CryptStringToBinaryA(cipher.data(), static_cast<DWORD>(cipher.size()), CRYPT_STRING_BASE64, bytes.data(), &count, nullptr, nullptr)) fail("Decode token");
    DATA_BLOB input{count, bytes.data()}, output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) throw Error("The saved token belongs to another Windows user. Enter your token again.");
    std::string result(reinterpret_cast<char*>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData); LocalFree(output.pbData); return result;
}
std::string resource(int id) {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC entry = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!entry) fail("Find embedded resource");
    HGLOBAL data = LoadResource(module, entry);
    const char* bytes = static_cast<const char*>(LockResource(data));
    if (!bytes) fail("Load embedded resource");
    return {bytes, SizeofResource(module, entry)};
}
fs::path executable() { std::wstring path(32768, L'\0'); DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())); if (!length || length == path.size()) fail("Find executable"); path.resize(length); return path; }
fs::path known_folder(const GUID& id) {
    PWSTR path = nullptr;
    winrt::check_hresult(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path));
    fs::path result(path); CoTaskMemFree(path); return result;
}
bool game_running() {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.get() == INVALID_HANDLE_VALUE) fail("Check game processes");
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot.get(), &entry)) do { if (_wcsicmp(entry.szExeFile, L"XSanity.exe") == 0) return true; } while (Process32NextW(snapshot.get(), &entry));
    return false;
}
fs::path game_executable(const fs::path& root) { for (auto folder : {L"Program64", L"Program32"}) { auto path = root / folder / L"XSanity.exe"; if (fs::is_regular_file(path)) return path; } throw Error("Choose the XSanity folder containing Program64 or Program32 and Themes."); }
void launch(const fs::path& exe, std::wstring_view arguments, const fs::path& cwd) {
    std::wstring command = L"\"" + exe.wstring() + L"\" " + std::wstring(arguments);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &startup, &process)) fail("Start program");
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
}
std::optional<fs::path> browse_folder(HWND owner, const fs::path& initial) {
    auto dialog = winrt::create_instance<IFileOpenDialog>(CLSID_FileOpenDialog, CLSCTX_INPROC_SERVER);
    DWORD options = 0; winrt::check_hresult(dialog->GetOptions(&options));
    winrt::check_hresult(dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM));
    dialog->SetTitle(L"Choose your XSanity folder");
    if (fs::is_directory(initial)) { winrt::com_ptr<IShellItem> item; if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(item.put())))) dialog->SetFolder(item.get()); }
    HRESULT result = dialog->Show(owner); if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return std::nullopt;
    winrt::check_hresult(result); winrt::com_ptr<IShellItem> item; winrt::check_hresult(dialog->GetResult(item.put()));
    PWSTR path = nullptr; winrt::check_hresult(item->GetDisplayName(SIGDN_FILESYSPATH, &path)); fs::path chosen(path); CoTaskMemFree(path); return chosen;
}
std::string normalized(std::string_view text) {
    auto input = wide(text); if (input.empty()) return {};
    int length = NormalizeString(NormalizationKC, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (!length) fail("Normalize chart title");
    std::wstring result(static_cast<size_t>(abs(length)), L'\0');
    length = NormalizeString(NormalizationKC, input.data(), static_cast<int>(input.size()), result.data(), static_cast<int>(result.size()));
    if (length <= 0) fail("Normalize chart title"); result.resize(static_cast<size_t>(length));
    std::wstring lower(result.size(), L'\0');
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, result.data(), length, lower.data(), length, nullptr, nullptr, 0)) fail("Normalize chart title");
    result.clear(); for (wchar_t c : lower) { WORD kind = 0; GetStringTypeW(CT_CTYPE1, &c, 1, &kind); if (kind & (C1_ALPHA | C1_DIGIT)) result += c; }
    return utf8(result);
}
void validate_mix(std::string_view mix) { if (mix != "Phoenix" && mix != "Phoenix2") throw Error("Choose Phoenix or Phoenix 2."); }
void validate_token(std::string_view token) { if (token.size() > 2048) throw Error("Token is too long."); for (unsigned char c : token) if (c < 32 || c == 127) throw Error("Invalid token."); wide(token); }
Object parse(std::string_view text) { try { return Object::Parse(winrt::to_hstring(text)); } catch (const winrt::hresult_error&) { throw Error("Invalid JSON."); } }
std::string encode(const Object& object) { return winrt::to_string(object.Stringify()); }
std::string str(const Object& object, std::wstring_view key, std::string fallback) { auto name = winrt::hstring(key); if (!object.HasKey(name) || object.GetNamedValue(name).ValueType() == json::JsonValueType::Null) return fallback; return winrt::to_string(object.GetNamedString(name)); }
int number(const Object& object, std::wstring_view key, int fallback) { auto name = winrt::hstring(key); if (!object.HasKey(name) || object.GetNamedValue(name).ValueType() == json::JsonValueType::Null) return fallback; double value = object.GetNamedNumber(name); if (value < INT_MIN || value > INT_MAX || value != static_cast<int>(value)) throw Error("Invalid integer."); return static_cast<int>(value); }
bool flag(const Object& object, std::wstring_view key, bool fallback) { return object.GetNamedBoolean(winrt::hstring(key), fallback); }
void put(Object& object, std::wstring_view key, std::string_view text) { object.Insert(winrt::hstring(key), Value::CreateStringValue(winrt::to_hstring(text))); }
void put(Object& object, std::wstring_view key, int value) { object.Insert(winrt::hstring(key), Value::CreateNumberValue(value)); }
void put(Object& object, std::wstring_view key, bool value) { object.Insert(winrt::hstring(key), Value::CreateBooleanValue(value)); }
}
