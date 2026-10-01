#include "support.h"
#include "api.h"

namespace piu::test {
void api_checks() {
    check(PiuScoresApi::allowed("https://piuscores.arroweclip.se/api/v2/charts?mix=Phoenix"),
        "official HTTPS API is allowed");
    for (auto url : {"http://piuscores.arroweclip.se/api/v2/charts",
             "https://evil.test/api/v2/charts",
             "https://piuscores.arroweclip.se.evil.test/api/v2/charts",
             "https://user@piuscores.arroweclip.se/api/v2/charts",
             "https://piuscores.arroweclip.se:444/api/v2/charts",
             "https://piuscores.arroweclip.se/api/v2/charts#fragment",
             "https://piuscores.arroweclip.se/account",
             "https://piuscores.arroweclip.se/api/v2/../account"}) {
        check(!PiuScoresApi::allowed(url), "unsafe API destination rejected before token transport");
    }

    int calls = 0;
    PiuScoresApi pages([&](const std::string&, const std::string&, const std::optional<Object>&) {
        ++calls;
        return parse(
            calls == 1
                ? R"({"data":[{"id":"a","songName":"Song","type":"Single","level":10}],"next":"https://piuscores.arroweclip.se/api/v2/charts?cursor=x"})"
                : R"({"data":[{"id":"b","songName":"Song","type":"Double","level":20}],"next":null})");
    });
    check(pages.catalog("test", "Phoenix").size() == 2 && calls == 2,
        "catalog cursor pages parsed with string chart IDs");
    calls = 0;
    PiuScoresApi malicious([&](const std::string&, const std::string&, const std::optional<Object>&) {
        ++calls;
        return parse(R"({"data":[],"next":"https://evil.test/api/v2/charts"})");
    });
    rejects(
        [&] {
            malicious.catalog("test", "Phoenix");
        },
        "foreign pagination URL rejected");
    check(calls == 1, "token never reaches foreign pagination transport");
    PiuScoresApi cycle([](const std::string& url, const std::string&, const std::optional<Object>&) {
        Object page;
        page.Insert(L"data", Array{});
        put(page, L"next", url);
        return page;
    });
    rejects(
        [&] {
            cycle.catalog("test", "Phoenix");
        },
        "pagination cycle rejected");
    PiuScoresApi legacy([](const std::string&, const std::string&, const std::optional<Object>&) {
        return parse(R"({"scoringModel":"legacy","data":[],"next":null})");
    });
    rejects(
        [&] {
            legacy.scores("test", "Phoenix");
        },
        "score envelope must use Phoenix model");
    PiuScoresApi upload([](const std::string& url, const std::string&, const std::optional<Object>& body) {
        check(url == "https://piuscores.arroweclip.se/api/v2/players/me/plays" && body &&
                  str(*body, L"source") == "xsanity-companion" && flag(*body, L"recordBrokenAsBest"),
            "POST uses documented route and source");
        auto play = body->GetNamedArray(L"plays").GetAt(0).GetObject();
        check(play.Size() == 4 && str(play, L"chartId") == "uuid" && number(play, L"score") == 999000,
            "no invented judgment or award fields uploaded");
        return parse(R"({"recorded":1})");
    });
    upload.upload("test", "Phoenix", {"uuid", 999000, false, iso_time(now())});
    PiuScoresApi plates([](const std::string&, const std::string&, const std::optional<Object>& body) {
        auto play = body->GetNamedArray(L"plays").GetAt(0).GetObject();
        if (flag(play, L"isBroken")) {
            check(!play.HasKey(L"award"), "failed stages never claim an award");
        } else {
            check(play.Size() == 5 && str(play, L"award") == "TG" && !play.HasKey(L"plate"),
                "TALENTED GAME uses the documented award field, not plate or invented counts");
        }

        return parse(R"({"recorded":1})");
    });
    plates.upload("test", "Phoenix2", {"uuid", 970218, false, iso_time(now()), "TG"});
    plates.upload("test", "Phoenix2", {"uuid", 970218, true, iso_time(now()), "TG"});
    PiuScoresApi unconfirmed([](const std::string&, const std::string&, const std::optional<Object>&) {
        return parse(R"({"recorded":0})");
    });
    rejects(
        [&] {
            unconfirmed.upload("test", "Phoenix", {});
        },
        "POST must confirm one recorded play");
}

} // namespace piu::test
