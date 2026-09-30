#include "api.h"
#include "version.h"
#include <winhttp.h>
#include <set>

namespace piu {
namespace {
constexpr char Origin[] = "https://piuscores.arroweclip.se";
constexpr char PlaysRoute[] = "/api/v2/players/me/plays";
struct Internet {
    HINTERNET value;
    explicit Internet(HINTERNET handle) : value(handle) { if (!value) fail("Connect to PIU Scores"); }
    ~Internet() { WinHttpCloseHandle(value); }
    Internet(const Internet&) = delete;
};
bool split(const std::wstring& url, URL_COMPONENTS& parts) {
    parts = {}; parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUserNameLength = parts.dwPasswordLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    return WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts) != FALSE;
}
Object http(const std::string& url, const std::string& token, const std::optional<Object>& body) {
    auto address = wide(url); URL_COMPONENTS parts{}; if (!split(address, parts)) throw Error("Invalid API URL.");
    Internet session(WinHttpOpen(L"PiuCompanion/" PIU_VERSION_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!WinHttpSetTimeouts(session.value, 15000, 15000, 15000, 15000)) fail("Set network timeouts");
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength), route(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) route.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    Internet connection(WinHttpConnect(session.value, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
    Internet request(WinHttpOpenRequest(connection.value, body ? L"POST" : L"GET", route.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled))) fail("Disable API redirects");
    std::wstring headers = L"Accept: application/json\r\nAuthorization: Basic " + wide(base64("companion:" + token)) + L"\r\n";
    std::string content;
    if (body) { content = encode(*body); headers += L"Content-Type: application/json\r\n"; }
    if (!WinHttpSendRequest(request.value, headers.c_str(), static_cast<DWORD>(headers.size()), content.empty() ? WINHTTP_NO_REQUEST_DATA : content.data(),
        static_cast<DWORD>(content.size()), static_cast<DWORD>(content.size()), 0) || !WinHttpReceiveResponse(request.value, nullptr)) fail("Receive API response");
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) fail("Read API status");
    if (status != 200) throw HttpError(static_cast<int>(status));
    std::string response;
    for (;;) {
        DWORD available = 0; if (!WinHttpQueryDataAvailable(request.value, &available)) fail("Read API response");
        if (!available) break;
        if (response.size() + available > 8 * 1024 * 1024) throw Error("API response exceeds the size limit.");
        auto offset = response.size(); response.resize(offset + available); DWORD got = 0;
        if (!WinHttpReadData(request.value, response.data() + offset, available, &got)) fail("Read API response");
        response.resize(offset + got); if (!got) break;
    }
    return parse(response);
}
}
PiuScoresApi::PiuScoresApi(Transport transport) : transport_(transport ? std::move(transport) : http) {}
bool PiuScoresApi::allowed(const std::string& url) {
    try {
        auto address = wide(url); URL_COMPONENTS parts{}; if (!split(address, parts)) return false;
        std::wstring host(parts.lpszHostName, parts.dwHostNameLength), path(parts.lpszUrlPath, parts.dwUrlPathLength);
        return parts.nScheme == INTERNET_SCHEME_HTTPS && parts.nPort == INTERNET_DEFAULT_HTTPS_PORT && _wcsicmp(host.c_str(), L"piuscores.arroweclip.se") == 0 &&
            parts.dwUserNameLength == 0 && parts.dwPasswordLength == 0 && path.starts_with(L"/api/v2/") && path.find(L'%') == std::wstring::npos &&
            path.find(L"..") == std::wstring::npos && path.find(L'\\') == std::wstring::npos && address.find(L'#') == std::wstring::npos;
    } catch (...) { return false; }
}
Object PiuScoresApi::request(const std::string& url, const std::string& token, const std::optional<Object>& body) {
    if (!allowed(url)) throw Error("Unexpected API URL."); validate_token(token);
    if (body && url != std::string(Origin) + PlaysRoute) throw Error("Unexpected API write.");
    return transport_(url, token, body);
}
std::vector<Object> PiuScoresApi::pages(const std::string& route, const std::string& token, const std::string& mix, bool score_page) {
    validate_mix(mix); std::string url = std::string(Origin) + route + "?mix=" + mix + "&limit=500";
    std::set<std::string> seen; std::vector<Object> rows;
    while (!url.empty()) {
        if (!seen.insert(url).second || seen.size() > 100) throw Error("Invalid API pagination.");
        auto page = request(url, token); if (score_page && str(page, L"scoringModel") != "phoenix") throw Error("PIU Scores returned an unsupported scoring model.");
        for (auto row : page.GetNamedArray(L"data")) rows.push_back(row.GetObject());
        url = str(page, L"next");
    }
    return rows;
}
std::vector<Chart> PiuScoresApi::catalog(const std::string& token, const std::string& mix) {
    std::vector<Chart> charts;
    for (const auto& row : pages("/api/v2/charts", token, mix, false)) charts.push_back({str(row, L"id"), str(row, L"songName"), str(row, L"type"), number(row, L"level")});
    return charts;
}
std::vector<Score> PiuScoresApi::scores(const std::string& token, const std::string& mix) {
    std::vector<Score> scores;
    for (const auto& row : pages("/api/v2/players/me/scores", token, mix, true)) {
        Score score; score.chart_id = str(row, L"chartId"); score.broken = flag(row, L"isBroken");
        if (row.HasKey(L"score") && row.GetNamedValue(L"score").ValueType() != json::JsonValueType::Null) score.score = number(row, L"score");
        scores.push_back(std::move(score));
    }
    return scores;
}
void PiuScoresApi::upload(const std::string& token, const std::string& mix, const Play& play) {
    validate_mix(mix); Object body; put(body, L"mix", mix); put(body, L"source", "xsanity-companion"); put(body, L"recordBrokenAsBest", true);
    Array plays; plays.Append(play_json(play)); body.Insert(L"plays", plays);
    if (number(request(std::string(Origin) + PlaysRoute, token, body), L"recorded") != 1) throw Error("Could not confirm the upload.");
}
}
