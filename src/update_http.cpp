#include "update.h"
#include "version.h"
#include <winhttp.h>
#include <array>

namespace piu {
namespace {
class Internet {
    HINTERNET value_;

public:
    explicit Internet(HINTERNET value) : value_(value) {
        if (!value_) {
            fail("Connect to GitHub");
        }
    }

    ~Internet() {
        if (value_) {
            WinHttpCloseHandle(value_);
        }
    }

    Internet(const Internet&) = delete;

    HINTERNET get() const {
        return value_;
    }
};

struct Address {
    std::wstring host, route;
};

Address address(const std::string& url) {
    auto text = wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = parts.dwUserNameLength =
        parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (text.find_first_of(L"\r\n\\#") != std::wstring::npos || text.find(L'\0') != std::wstring::npos ||
        !WinHttpCrackUrl(text.c_str(), static_cast<DWORD>(text.size()), 0, &parts) ||
        parts.nScheme != INTERNET_SCHEME_HTTPS || parts.nPort != INTERNET_DEFAULT_HTTPS_PORT ||
        parts.dwUserNameLength || parts.dwPasswordLength) {
        throw Error("GitHub returned an unsafe download address.");
    }

    Address result{{parts.lpszHostName, parts.dwHostNameLength}, {parts.lpszUrlPath, parts.dwUrlPathLength}};
    bool api = result.host == L"api.github.com" &&
               result.route == L"/repos/mat100payette/XSanityPIUScoresHook/releases/latest";
    bool release = result.host == L"github.com" &&
                   result.route.starts_with(L"/mat100payette/XSanityPIUScoresHook/releases/download/");
    bool asset = result.host == L"release-assets.githubusercontent.com" ||
                 result.host == L"objects.githubusercontent.com";
    if (!api && !release && !asset) {
        throw Error("GitHub returned an unexpected download host or repository.");
    }

    result.route.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    return result;
}

// One async operation at a time. The worker waits on completion or cancellation;
// no other thread closes a request while an API call is starting.
class Request {
    Handle done_{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    Handle closed_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    HINTERNET request_ = nullptr;
    std::atomic<DWORD> error_{0}, received_{0};
    std::array<char, 16384> buffer_{};

    static void CALLBACK callback(HINTERNET, DWORD_PTR context, DWORD status, void* info, DWORD size) {
        auto self = reinterpret_cast<Request*>(context);
        if (!self) {
            return;
        }

        if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
            SetEvent(self->closed_.get());
            return;
        }

        if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
            self->error_ = static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError;
        } else if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) {
            self->received_ = size;
        } else if (status != WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE &&
                   status != WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE) {
            return;
        }

        SetEvent(self->done_.get());
    }

public:
    Request(HINTERNET connection, const std::wstring& route) {
        if (!done_.get() || !closed_.get()) {
            fail("Create update events");
        }

        request_ = WinHttpOpenRequest(connection,
            L"GET",
            route.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE);
        if (!request_) {
            fail("Open GitHub request");
        }

        DWORD_PTR context = reinterpret_cast<DWORD_PTR>(this);
        if (!WinHttpSetOption(request_, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)) ||
            WinHttpSetStatusCallback(request_,
                callback,
                WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE | WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE |
                    WINHTTP_CALLBACK_FLAG_READ_COMPLETE | WINHTTP_CALLBACK_FLAG_REQUEST_ERROR |
                    WINHTTP_CALLBACK_FLAG_HANDLES,
                0) == WINHTTP_INVALID_STATUS_CALLBACK) {
            WinHttpCloseHandle(request_);
            request_ = nullptr;
            fail("Configure GitHub request");
        }
    }

    ~Request() {
        if (request_) {
            WinHttpCloseHandle(request_);
            // HANDLE_CLOSING is the last callback; its context must stay alive until then.
            WaitForSingleObject(closed_.get(), INFINITE);
        }
    }

    HINTERNET get() const {
        return request_;
    }

    void wait(BOOL started, HANDLE cancelled) {
        if (!started && GetLastError() != ERROR_IO_PENDING) {
            fail("Contact GitHub");
        }

        HANDLE events[]{cancelled, done_.get()};
        DWORD result = WaitForMultipleObjects(2, events, FALSE, 30000);
        if (result == WAIT_OBJECT_0) {
            throw UpdateCancelled{};
        }

        if (result == WAIT_TIMEOUT) {
            throw Error("GitHub took too long to respond. Please try again.");
        }

        if (result != WAIT_OBJECT_0 + 1) {
            fail("Wait for GitHub");
        }

        if (error_) {
            throw Error("Could not reach GitHub (Windows error " + std::to_string(error_.load()) +
                        "). Check your connection and try again.");
        }
    }

    std::string_view read_chunk(HANDLE cancelled) {
        wait(WinHttpReadData(request_, buffer_.data(), static_cast<DWORD>(buffer_.size()), nullptr),
            cancelled);
        return {buffer_.data(), received_.load()};
    }
};

std::wstring header(HINTERNET request, DWORD field) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, field, nullptr, nullptr, &size, nullptr);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size > 16384) {
        throw Error("GitHub returned an invalid response header.");
    }

    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, field, nullptr, value.data(), &size, nullptr)) {
        fail("Read GitHub response");
    }

    value.resize(size / sizeof(wchar_t));
    return value;
}
} // namespace

bool allowed_update_url(const std::string& url) {
    try {
        address(url);
        return true;
    } catch (...) {
        return false;
    }
}

std::string update_http(
    const std::string& url, size_t limit, std::stop_token stop, const UpdateProgress& progress) {
    Internet session(WinHttpOpen(L"XSanityPIUScoresHook/" PIU_VERSION_W,
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        WINHTTP_FLAG_ASYNC));
    if (!WinHttpSetTimeouts(session.get(), 10000, 10000, 10000, 10000)) {
        fail("Set update timeouts");
    }

    Handle cancelled(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!cancelled.get()) {
        fail("Create update cancellation event");
    }

    std::stop_callback cancel(stop, [&] {
        SetEvent(cancelled.get());
    });
    auto next = url;
    const auto deadline = GetTickCount64() + 120000;
    for (int redirects = 0; redirects <= 3; ++redirects) {
        check_update_cancelled(stop);
        auto target = address(next);
        Internet connection(
            WinHttpConnect(session.get(), target.host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
        Request request(connection.get(), target.route);
        DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
        DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
        if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) ||
            !WinHttpSetOption(
                request.get(), WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon, sizeof(autologon))) {
            fail("Configure update transport");
        }

        constexpr wchar_t Headers[] =
            L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
        request.wait(WinHttpSendRequest(request.get(),
                         Headers,
                         static_cast<DWORD>(std::size(Headers) - 1),
                         WINHTTP_NO_REQUEST_DATA,
                         0,
                         0,
                         reinterpret_cast<DWORD_PTR>(&request)),
            cancelled.get());
        check_update_cancelled(stop);
        request.wait(WinHttpReceiveResponse(request.get(), nullptr), cancelled.get());
        DWORD status = 0, size = sizeof(status);
        if (!WinHttpQueryHeaders(request.get(),
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                nullptr,
                &status,
                &size,
                nullptr)) {
            fail("Read GitHub status");
        }

        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            next = utf8(header(request.get(), WINHTTP_QUERY_LOCATION));
            continue;
        }

        if (status == 404 && next == LatestReleaseUrl) {
            throw Error("No public release is available on GitHub. The repository may be private, "
                        "or its release has not been published yet.");
        }

        if (status == 403 || status == 429) {
            throw Error("GitHub temporarily limited update requests. Please try again later.");
        }

        if (status != 200) {
            throw Error("GitHub could not provide the update (HTTP " + std::to_string(status) +
                        "). Please try again later.");
        }

        std::string bytes;
        for (;;) {
            check_update_cancelled(stop);
            if (GetTickCount64() > deadline) {
                throw Error("The update download timed out. Please try again.");
            }

            auto chunk = request.read_chunk(cancelled.get());
            if (chunk.empty()) {
                return bytes;
            }

            if (bytes.size() + chunk.size() > limit) {
                throw Error("The update download exceeds its expected size.");
            }

            bytes.append(chunk);
            if (progress) {
                progress(bytes.size(), limit);
            }
        }
    }

    throw Error("GitHub redirected the download too many times. Please try again later.");
}
} // namespace piu