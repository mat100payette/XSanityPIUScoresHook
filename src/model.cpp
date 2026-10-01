#include "model.h"
#include <algorithm>

namespace piu {
std::string trim_profile(std::string_view name) {
    auto first = name.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }

    return std::string(name.substr(first, name.find_last_not_of(" \t\r\n") - first + 1));
}

bool same_profile(std::string_view left, std::string_view right) {
    auto a = wide(trim_profile(left)), b = wide(trim_profile(right));
    return !a.empty() && !b.empty() && CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

Preferences load_preferences(const fs::path& folder) {
    Preferences config;
    if (!fs::exists(folder / L"settings.json")) {
        return config;
    }

    auto data = parse(read(folder / L"settings.json"));
    config.mix = str(data, L"Mix", "Phoenix");
    validate_mix(config.mix);
    if (data.HasKey(L"Accounts")) {
        auto accounts = data.GetNamedArray(L"Accounts");
        if (accounts.Size() > config.accounts.size()) {
            throw Error("Unsupported number of player accounts.");
        }

        for (uint32_t i = 0; i < accounts.Size(); ++i) {
            auto account = accounts.GetAt(i).GetObject();
            config.accounts[i] = {str(account, L"Profile"), str(account, L"ProtectedToken")};
        }
    } else {
        // Preserve the existing account; require its game profile before capturing new plays.
        config.accounts[0].protected_token = str(data, L"ProtectedToken");
    }

    config.game_root = wide(str(data, L"GameRoot"));
    config.sync = flag(data, L"SyncEnabled", true);
    config.overlay = flag(data, L"OverlayEnabled", false);
    auto cutoff = str(data, L"CaptureAfter", "0");
    config.capture_after = std::stoull(cutoff);
    return config;
}

void save_preferences(const fs::path& folder, const Preferences& config) {
    Object data;
    put(data, L"Mix", config.mix);
    Array accounts;
    for (const auto& account : config.accounts) {
        Object row;
        put(row, L"Profile", account.profile);
        put(row, L"ProtectedToken", account.protected_token);
        accounts.Append(row);
    }

    data.Insert(L"Accounts", accounts);
    put(data, L"GameRoot", utf8(config.game_root.wstring()));
    put(data, L"SyncEnabled", config.sync);
    put(data, L"OverlayEnabled", config.overlay);
    put(data, L"CaptureAfter", std::to_string(config.capture_after));
    atomic_write(folder / L"settings.json", encode(data));
}

GameChart game_chart(const Object& data) {
    return {str(data, L"title"), str(data, L"type"), str(data, L"difficulty"), number(data, L"level")};
}

Result result_from_json(const Object& data) {
    return {str(data, L"id"),
        game_chart(data),
        number(data, L"score"),
        flag(data, L"broken"),
        flag(data, L"eligible"),
        str(data, L"plate"),
        str(data, L"profile"),
        number(data, L"side"),
        flag(data, L"ambiguous"),
        str(data, L"skip_reason")};
}

Object result_json(const Result& result) {
    Object data;
    put(data, L"id", result.id);
    put(data, L"title", result.chart.title);
    put(data, L"type", result.chart.type);
    put(data, L"difficulty", result.chart.difficulty);
    put(data, L"level", result.chart.level);
    put(data, L"score", result.score);
    put(data, L"broken", result.broken);
    put(data, L"eligible", result.eligible);
    put(data, L"profile", result.profile);
    put(data, L"side", result.side);
    put(data, L"ambiguous", result.ambiguous);
    if (!result.skip_reason.empty()) {
        put(data, L"skip_reason", result.skip_reason);
    }

    if (!result.plate.empty() && !result.broken) {
        put(data, L"plate", result.plate);
    }

    return data;
}

Object play_json(const Play& play) {
    Object data;
    put(data, L"chartId", play.chart_id);
    put(data, L"score", play.score);
    put(data, L"isBroken", play.broken);
    put(data, L"playedAt", play.played_at);
    if (!play.plate.empty() && !play.broken) {
        put(data, L"award", play.plate);
    }

    return data;
}

Store load_store(const fs::path& folder) {
    Store store;
    if (!fs::exists(folder / L"uploads.json")) {
        return store;
    }

    auto data = parse(read(folder / L"uploads.json", 16 * 1024 * 1024));
    for (auto value : data.GetNamedArray(L"Pending", Array())) {
        auto item = value.GetObject();
        Pending pending;
        pending.result = result_from_json(item.GetNamedObject(L"Result"));
        pending.result.id = str(item, L"Id", pending.result.id);
        pending.mix = str(item, L"Mix");
        validate_mix(pending.mix);
        pending.played_at = str(item, L"PlayedAt");
        pending.state = str(item, L"State", "queued");
        pending.account = number(item, L"Account");
        if (pending.account < 0 || pending.account > 1) {
            throw Error("Pending upload has an invalid account.");
        }

        if (pending.state == "sending") {
            pending.state = "uncertain";
        }

        store.pending.push_back(std::move(pending));
    }

    for (auto pair : data.GetNamedObject(L"Receipts", Object())) {
        store.receipts.emplace(winrt::to_string(pair.Key()), winrt::to_string(pair.Value().GetString()));
    }

    return store;
}

void save_store(const fs::path& folder, const Store& store) {
    Object data;
    Array pending;
    Object receipts;
    for (const auto& item : store.pending) {
        Object row;
        put(row, L"Id", item.result.id);
        put(row, L"Mix", item.mix);
        put(row, L"PlayedAt", item.played_at);
        put(row, L"State", item.state);
        put(row, L"Account", item.account);
        row.Insert(L"Result", result_json(item.result));
        pending.Append(row);
    }

    for (const auto& [id, outcome] : store.receipts) {
        put(receipts, wide(id), outcome);
    }

    data.Insert(L"Pending", pending);
    data.Insert(L"Receipts", receipts);
    atomic_write(folder / L"uploads.json", encode(data));
}

bool better(const Result& candidate, const Score* previous) {
    if (!previous) {
        return true;
    }

    if (candidate.broken != previous->broken) {
        return !candidate.broken;
    }

    return !previous->score || candidate.score > *previous->score;
}

std::string match(const GameChart& game, const std::vector<Chart>& catalog) {
    if (game.type != "Single" && game.type != "Double") {
        return {};
    }

    std::string label = (game.type == "Single" ? "S" : "D") + std::to_string(game.level);
    std::string difficulty = game.difficulty;
    for (char& c : difficulty) {
        c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    }

    auto begin = difficulty.find_first_not_of(" \t\r\n"), end = difficulty.find_last_not_of(" \t\r\n");
    if (begin == std::string::npos || difficulty.substr(begin, end - begin + 1) != label) {
        return {};
    }

    auto key = normalized(game.title);
    if (key.empty()) {
        return {};
    }

    std::string id;
    unsigned matches = 0;
    for (const auto& chart : catalog) {
        if (chart.type == game.type && chart.level == game.level &&
            (chart.title_key.empty() ? normalized(chart.title) : chart.title_key) == key) {
            id = chart.id;
            ++matches;
        }
    }

    return matches == 1 ? id : "";
}
} // namespace piu
