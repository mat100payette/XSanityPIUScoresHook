#include "engine.h"
#include "game_hook.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace piu {
namespace {
struct UploadFailure {
    std::string state;
    std::string message;
    std::string notice;
};

UploadFailure upload_play(Api& api, const std::string& token, const std::string& mix, const Play& play) {
    try {
        api.upload(token, mix, play);
        return {};
    } catch (const HttpError& error) {
        if (error.status == 401 || error.status == 403) {
            return {"queued", error.what(), "auth"};
        }

        if (error.status == 429) {
            return {"queued", error.what(), "retry"};
        }

        if (error.status == 400 || error.status == 404 || error.status == 409 || error.status == 422) {
            return {"rejected", error.what(), "rejected"};
        }

        return {"uncertain", error.what(), "uncertain"};
    } catch (...) {
        return {"uncertain", "Upload response lost. Checking PIU Scores before another upload.", "uncertain"};
    }
}
} // namespace

void Engine::load() {
    std::lock_guard lock(gate_);
    config_ = load_preferences(folder_);
    data_ = config_.sync ? load_store(folder_) : Store{};
    for (size_t i = 0; i < tokens_.size(); ++i) {
        try {
            tokens_[i] = unprotect(config_.accounts[i].protected_token);
        } catch (const Error& error) {
            account_status_[i] = error.what();
        }
    }
}

Preferences Engine::config() const {
    std::lock_guard lock(gate_);
    return config_;
}

Store Engine::store() const {
    std::lock_guard lock(gate_);
    return data_;
}

std::string Engine::token(size_t account) const {
    std::lock_guard lock(gate_);
    return tokens_.at(account);
}

std::string Engine::status(uint64_t clock) const {
    std::lock_guard lock(gate_);
    for (size_t i = 0; i < tokens_.size(); ++i) {
        if (config_.sync && !tokens_[i].empty() && config_.accounts[i].profile.empty()) {
            return "Add the XSanity profile name for Account " + std::to_string(i + 1) +
                   " in Account settings.";
        }
    }

    if ((!tokens_[0].empty() || !tokens_[1].empty()) && (config_.sync || config_.overlay)) {
        if (heartbeat_ == 0 || clock < heartbeat_ || clock - heartbeat_ > 150000000) {
            return "Waiting for XSanity exports. " + status_;
        }

        if (!export_error_.empty()) {
            return "XSanity exporter: " + export_error_;
        }
    }

    for (size_t i = 0; i < account_status_.size(); ++i) {
        if (!account_status_[i].empty() && account_status_[i] != "Connected") {
            return "Account " + std::to_string(i + 1) + ": " + account_status_[i];
        }
    }

    return status_;
}

int Engine::account_for(const std::string& profile) const {
    int matched = -1;
    for (size_t i = 0; i < config_.accounts.size(); ++i) {
        if (!tokens_[i].empty() && same_profile(profile, config_.accounts[i].profile)) {
            if (matched != -1) {
                return -1;
            }

            matched = static_cast<int>(i);
        }
    }

    return matched;
}

std::string Engine::player_status(uint64_t clock) const {
    std::lock_guard lock(gate_);
    if (!heartbeat_ || clock < heartbeat_ || clock - heartbeat_ > 150000000) {
        return "Waiting for XSanity profiles.";
    }

    std::string text;
    for (size_t i = 0; i < live_profiles_.size(); ++i) {
        if (i) {
            text += "\r\n";
        }

        text += "P" + std::to_string(i + 1) + ": ";
        const auto& profile = live_profiles_[i];
        if (profile.empty()) {
            text += "No named profile detected";
            continue;
        }

        text += profile + " — ";
        auto account = account_for(profile);
        if (same_profile(profile, live_profiles_[1 - i])) {
            text += "Duplicate profile; not syncing";
        } else if (account < 0) {
            text += "No matching account; not syncing";
        } else {
            text += "Account " + std::to_string(account + 1) + " matched";
            if (!account_status_[account].empty()) {
                text += " · " + account_status_[account];
            }
        }
    }

    return text;
}

void Engine::configure(const std::array<AccountInput, 2>& accounts, const std::string& mix) {
    validate_mix(mix);
    auto inputs = accounts;
    for (auto& account : inputs) {
        validate_token(account.token);
        account.profile = trim_profile(account.profile);
        if (account.profile.size() > 256 ||
            std::any_of(account.profile.begin(), account.profile.end(), [](unsigned char c) {
                return c < 32 || c == 127;
            })) {
            throw Error("Use a profile name of at most 256 UTF-8 bytes, without control characters.");
        }
    }

    if (same_profile(inputs[0].profile, inputs[1].profile)) {
        throw Error("Use a different XSanity profile name for each account.");
    }

    if (!inputs[0].token.empty() && inputs[0].token == inputs[1].token) {
        throw Error("Both accounts have the same API key. Enter each player's own key.");
    }

    std::lock_guard lock(gate_);
    auto next = config_;
    next.mix = mix;
    bool changed = mix != config_.mix;
    for (size_t i = 0; i < inputs.size(); ++i) {
        const auto& input = inputs[i];
        if ((config_.sync && !input.token.empty() && input.profile.empty()) ||
            (!input.profile.empty() && input.token.empty())) {
            throw Error("Enter both an XSanity profile name and API key, or leave both empty.");
        }

        bool account_changed = input.profile != config_.accounts[i].profile || input.token != tokens_[i];
        bool initial_binding = config_.accounts[i].profile.empty() && input.token == tokens_[i];
        if (((account_changed && !initial_binding) || mix != config_.mix) &&
            std::any_of(data_.pending.begin(), data_.pending.end(), [i](const auto& item) {
                return item.account == static_cast<int>(i);
            })) {
            throw Error(
                "Account " + std::to_string(i + 1) +
                " has pending uploads. Finish or discard them before changing its profile, key, or mix.");
        }

        changed |= account_changed;
        next.accounts[i] = {input.profile,
            input.token == tokens_[i] ? config_.accounts[i].protected_token : protect(input.token)};
    }

    if (!changed) {
        return;
    }

    next.capture_after = now();
    save_preferences(folder_, next);
    config_ = std::move(next);
    for (size_t i = 0; i < inputs.size(); ++i) {
        tokens_[i] = inputs[i].token;
        best_[i].clear();
        account_status_[i].clear();
        account_feedback_[i].clear();
    }

    ++generation_;
    catalog_.clear();
    playing_ = false;
    sync_due_ = true;
    status_ = "Account settings saved.";
    wake_.notify_all();
}

void Engine::capture(const Result& result, uint64_t written) {
    std::lock_guard lock(gate_);
    if (!config_.sync || result.side < 1 || result.side > 2 || result.id.empty() || result.id.size() > 128) {
        return;
    }

    feedback_.observe(result, "checking", written);
    auto receipt = data_.receipts.find(result.id);
    if (receipt != data_.receipts.end()) {
        feedback_.update(result.id, receipt->second);
        return;
    }

    auto pending = std::find_if(data_.pending.begin(), data_.pending.end(), [&](const auto& item) {
        return item.result.id == result.id;
    });
    if (pending != data_.pending.end()) {
        auto code = pending->state == "queued" ? account_feedback_[pending->account] : pending->state;
        feedback_.update(result.id, code.empty() ? "checking" : code);
        return;
    }

    int account = account_for(result.profile);
    if (result.ambiguous || account < 0 || written <= config_.capture_after) {
        feedback_.update(result.id, result.ambiguous ? "ambiguous" : account < 0 ? "unlinked" : "expired");
        return;
    }

    auto previous = data_;
    if (!result.eligible || result.score < 0 || result.score > 1000000) {
        data_.receipts[result.id] = "skipped";
        feedback_.update(result.id, "skipped");
        status_ = "Skipped a result with unsupported chart, modifiers, or autoplay.";
    } else {
        data_.pending.push_back({result, config_.mix, iso_time(written), "queued", account});
        feedback_.update(result.id, "checking");
        sync_due_ = true;
    }

    try {
        save_store(folder_, data_);
    } catch (...) {
        data_ = std::move(previous);
        feedback_.update(result.id, "storage");
        throw;
    }

    wake_.notify_all();
}

size_t Engine::pending_count(size_t account) const {
    std::lock_guard lock(gate_);
    return static_cast<size_t>(
        std::count_if(data_.pending.begin(), data_.pending.end(), [account](const auto& item) {
            return item.account == static_cast<int>(account);
        }));
}

void Engine::discard_pending(size_t account) {
    std::lock_guard lock(gate_);
    if (account >= config_.accounts.size()) {
        throw Error("Invalid account.");
    }

    if (std::any_of(data_.pending.begin(), data_.pending.end(), [](const auto& item) {
            return item.state == "sending";
        })) {
        throw Error("An upload is in progress. Wait for it to finish before discarding pending scores.");
    }

    auto next = data_;
    for (const auto& item : next.pending) {
        if (item.account == static_cast<int>(account)) {
            next.receipts[item.result.id] = "discarded";
        }
    }

    std::erase_if(next.pending, [account](const auto& item) {
        return item.account == static_cast<int>(account);
    });
    save_store(folder_, next);
    for (const auto& item : data_.pending) {
        if (item.account == static_cast<int>(account)) {
            feedback_.update(item.result.id, "discarded");
        }
    }

    data_ = std::move(next);
    ++generation_;
}

void Engine::poll() {
    Preferences config = this->config();
    if (config.game_root.empty()) {
        return;
    }

    publish_feedback();
    auto frame = feed_ ? feed_(config.game_root) : mailbox_.poll(config.game_root);
    if (!frame) {
        return;
    }

    auto packet = parse(frame->payload);
    auto current = packet.GetNamedObject(L"current");
    {
        std::lock_guard lock(gate_);
        if (config.game_root != config_.game_root) {
            return;
        }

        current_ = config.overlay ? game_chart(current) : GameChart{};
        export_error_ = str(current, L"error");
        if (flag(current, L"initializing")) {
            export_error_ = "Hook loaded; waiting for initialization.";
        }

        playing_ = config.overlay && flag(current, L"playing") && export_error_.empty();
        heartbeat_ = frame->observed;
        live_profiles_ = {};
        for (const auto& value : packet.GetNamedArray(L"players", Array())) {
            auto player = value.GetObject();
            auto side = number(player, L"side");
            if (side >= 1 && side <= 2) {
                live_profiles_[side - 1] = str(player, L"profile");
            }
        }
    }

    if (config.sync && (packet.HasKey(L"results") || packet.HasKey(L"result"))) {
        auto completed = packet.GetNamedNumber(L"completed");
        auto age = frame->clock - completed;
        if (std::isfinite(age) && completed >= 0 && age >= 0 && age <= 60) {
            auto elapsed = static_cast<uint64_t>(age * 10000000);
            if (elapsed <= frame->observed) {
                if (packet.HasKey(L"results")) {
                    auto results = packet.GetNamedArray(L"results");
                    if (results.Size() > 2) {
                        throw Error("Too many player results in mailbox.");
                    }

                    std::vector<Result> completed_results;
                    for (const auto& value : results) {
                        completed_results.push_back(result_from_json(value.GetObject()));
                    }

                    if (completed_results.size() == 2 &&
                        (same_profile(completed_results[0].profile, completed_results[1].profile) ||
                            completed_results[0].side == completed_results[1].side)) {
                        for (auto& result : completed_results) {
                            result.ambiguous = true;
                        }
                    }

                    for (const auto& result : completed_results) {
                        capture(result, frame->observed - elapsed);
                    }
                } else {
                    capture(result_from_json(packet.GetNamedObject(L"result")), frame->observed - elapsed);
                }
            }
        }
    }
}

void Engine::publish_feedback() {
    fs::path path;
    std::string text;
    {
        std::lock_guard lock(gate_);
        if (!config_.sync || config_.game_root.empty()) {
            return;
        }

        path = GameHook::exports(config_.game_root) / L"status.txt";
        text = feedback_.snapshot();
    }

    if (text == published_feedback_ && path == feedback_path_) {
        return;
    }

    try {
        safe_path(path);
        atomic_write(path, text);
        published_feedback_ = std::move(text);
        feedback_path_ = std::move(path);
    } catch (...) {
        // Notifications must never prevent capture or uploads. Retry on the next poll.
    }
}

void Engine::replace_scores(size_t account, const std::vector<Score>& scores) {
    best_[account].clear();
    for (const auto& score : scores) {
        best_[account].insert_or_assign(score.chart_id, score);
    }
}

void Engine::complete(size_t index, const std::string& outcome) {
    auto previous = data_;
    auto id = data_.pending[index].result.id;
    data_.receipts[id] = outcome;
    data_.pending.erase(data_.pending.begin() + static_cast<ptrdiff_t>(index));
    try {
        save_store(folder_, data_);
    } catch (...) {
        data_ = std::move(previous);
        throw;
    }

    feedback_.update(id, outcome);
}

void Engine::sync() {
    std::lock_guard serial(sync_gate_);
    std::exception_ptr first_error;
    for (size_t account = 0; account < tokens_.size(); ++account) {
        try {
            sync_account(account);
        } catch (...) {
            auto error = std::current_exception();
            std::string code = "retry";
            try {
                std::rethrow_exception(error);
            } catch (const HttpError& failure) {
                if (failure.status == 401 || failure.status == 403) {
                    code = "auth";
                }
            } catch (...) {
            }

            if (!first_error) {
                first_error = std::current_exception();
            }

            std::lock_guard lock(gate_);
            account_status_[account] = "PIU Scores unavailable; will retry";
            account_feedback_[account] = code;
            for (const auto& item : data_.pending) {
                if (item.account == static_cast<int>(account) && item.state == "queued") {
                    feedback_.update(item.result.id, code);
                }
            }
        }
    }

    if (first_error) {
        std::rethrow_exception(first_error);
    }
}

void Engine::sync_account(size_t account) {
    std::string token;
    std::string mix;
    unsigned generation;
    bool need_catalog;
    {
        std::lock_guard lock(gate_);
        if (tokens_[account].empty() || (!config_.sync && (!config_.overlay || account != 0))) {
            return;
        }

        token = tokens_[account];
        mix = config_.mix;
        generation = generation_;
        need_catalog = catalog_.empty();
    }

    auto charts = need_catalog ? api_.catalog(token, mix) : std::vector<Chart>{};
    auto scores = api_.scores(token, mix);
    for (auto& chart : charts) {
        chart.title_key = normalized(chart.title);
    }
    {
        std::lock_guard lock(gate_);
        if (generation != generation_) {
            return;
        }

        if (need_catalog) {
            catalog_ = std::move(charts);
        }

        replace_scores(account, scores);
        account_status_[account] = "Connected";
        account_feedback_[account].clear();
        status_ = config_.sync ? "Connected. PB syncing is enabled." : "Connected. OBS overlay only.";
        if (!config_.sync) {
            return;
        }
    }

    size_t index = 0;
    for (;;) {
        Play play;
        std::string event;
        {
            std::lock_guard lock(gate_);
            if (generation != generation_ || stopping_ || index >= data_.pending.size()) {
                break;
            }

            auto& item = data_.pending[index];
            if (item.mix != mix || item.account != static_cast<int>(account)) {
                ++index;
                continue;
            }

            auto id = match(item.result.chart, catalog_);
            if (id.empty()) {
                status_ = "Chart not matched: " + item.result.chart.title;
                complete(index, "unmatched");
                continue;
            }

            auto found = best_[account].find(id);
            const Score* previous = found == best_[account].end() ? nullptr : &found->second;
            if (!better(item.result, previous)) {
                complete(index, "covered");
                continue;
            }

            if (item.state != "queued") {
                account_status_[account] =
                    "An upload needs checking on PIU Scores. It will not be resubmitted automatically.";
                status_ = account_status_[account];
                ++index;
                continue;
            }

            item.state = "sending";
            try {
                save_store(folder_, data_);
            } catch (...) {
                item.state = "queued";
                throw;
            }

            feedback_.update(item.result.id, "sending");
            event = item.result.id;
            play = {id, item.result.score, item.result.broken, item.played_at, item.result.plate};
        }

        auto failure = upload_play(api_, token, mix, play);
        {
            std::lock_guard lock(gate_);
            if (index >= data_.pending.size() || data_.pending[index].result.id != event) {
                throw Error("Upload queue changed unexpectedly.");
            }

            if (!failure.state.empty()) {
                data_.pending[index].state = failure.state;
                save_store(folder_, data_);
                feedback_.update(event, failure.notice);
                account_feedback_[account] = failure.notice;
                account_status_[account] = failure.message;
                status_ = failure.message;
                break;
            }

            complete(index, "accepted");
        }

        // A failed refresh cannot undo a confirmed POST or change its receipt to uncertain.
        auto refreshed = api_.scores(token, mix);
        std::lock_guard lock(gate_);
        if (generation != generation_) {
            return;
        }

        replace_scores(account, refreshed);
    }
}

std::string Engine::state(uint64_t clock) const {
    std::lock_guard lock(gate_);
    bool playing = config_.overlay && playing_ && clock >= heartbeat_ && clock - heartbeat_ < 40000000;
    std::string id = playing ? match(current_, catalog_) : "";
    auto found = best_[0].find(id);
    Object state;
    put(state, L"playing", playing);
    put(state, L"song", playing ? current_.title : "");
    put(state, L"difficulty", playing ? current_.difficulty : "");
    if (found != best_[0].end() && found->second.score) {
        put(state, L"pb", *found->second.score);
    } else {
        state.Insert(L"pb", Value::CreateNullValue());
    }

    return encode(state);
}

void Engine::poll_loop() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    while (!stopping_) {
        try {
            poll();
        } catch (...) {
            // Incomplete mailbox snapshots are retried on the next tick.
        }

        std::unique_lock lock(wake_gate_);
        wake_.wait_for(lock, std::chrono::milliseconds(200), [this] {
            return stopping_.load();
        });
    }
}

void Engine::sync_loop() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    auto next = std::chrono::steady_clock::now();
    while (!stopping_) {
        if (sync_due_.exchange(false) || std::chrono::steady_clock::now() >= next) {
            try {
                sync();
            } catch (...) {
                std::lock_guard lock(gate_);
                status_ = "PIU Scores unavailable. Will retry automatically.";
            }

            next = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        }

        std::unique_lock lock(wake_gate_);
        wake_.wait_for(lock, std::chrono::milliseconds(250), [this] {
            return stopping_.load();
        });
    }
}

void Engine::start() {
    if (poll_thread_.joinable()) {
        return;
    }

    stopping_ = false;
    poll_thread_ = std::thread([this] {
        poll_loop();
    });
    sync_thread_ = std::thread([this] {
        sync_loop();
    });
}

void Engine::stop() {
    stopping_ = true;
    wake_.notify_all();
    if (poll_thread_.joinable()) {
        poll_thread_.join();
    }

    if (sync_thread_.joinable()) {
        sync_thread_.join();
    }
}
} // namespace piu
