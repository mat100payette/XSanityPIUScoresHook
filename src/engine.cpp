#include "engine.h"
#include <algorithm>
#include <chrono>

namespace piu {
void Engine::load() {
    std::lock_guard lock(gate_); config_ = load_preferences(folder_); data_ = config_.sync ? load_store(folder_) : Store{};
    try { token_ = unprotect(config_.protected_token); } catch (const Error& error) { status_ = error.what(); }
}
Preferences Engine::config() const { std::lock_guard lock(gate_); return config_; }
Store Engine::store() const { std::lock_guard lock(gate_); return data_; }
std::string Engine::token() const { std::lock_guard lock(gate_); return token_; }
std::string Engine::status() const { std::lock_guard lock(gate_); return status_; }
void Engine::configure(const std::string& token, const std::string& mix) {
    validate_token(token); validate_mix(mix);
    std::lock_guard lock(gate_);
    if (std::any_of(data_.pending.begin(), data_.pending.end(), [](const auto& item) { return item.state == "sending"; }) ||
        (!data_.pending.empty() && mix != config_.mix)) throw Error("Finish pending uploads before changing the mix. Keep the same PIU Scores account when replacing your token.");
    auto next = config_; next.mix = mix; next.protected_token = protect(token);
    if ((token_ != token || config_.mix != mix) && data_.pending.empty()) { next.capture_after = now(); }
    save_preferences(folder_, next); config_ = std::move(next); token_ = token;
    ++generation_; catalog_.clear(); best_.clear(); playing_ = false; heartbeat_ = 0; sync_due_ = true;
    status_ = token.empty() ? "No token configured." : "Connecting to PIU Scores..."; wake_.notify_all();
}
void Engine::capture(const Result& result, uint64_t written) {
    std::lock_guard lock(gate_);
    if (!config_.sync || token_.empty() || written <= config_.capture_after || result.id.empty() || result.id.size() > 128 ||
        data_.receipts.contains(result.id) || std::any_of(data_.pending.begin(), data_.pending.end(), [&](const auto& item) { return item.result.id == result.id; })) return;
    auto previous = data_;
    if (!result.eligible || result.score < 0 || result.score > 1000000) { data_.receipts[result.id] = "skipped"; status_ = "Skipped a result with unsupported chart, modifiers, or autoplay."; }
    else { data_.pending.push_back({result, config_.mix, iso_time(written), "queued"}); sync_due_ = true; }
    try { save_store(folder_, data_); } catch (...) { data_ = std::move(previous); throw; }
    wake_.notify_all();
}
void Engine::poll() {
    Preferences config = this->config(); if (config.game_root.empty()) return;
    auto exports = config.game_root / L"Save" / L"PiuCompanion";
    auto current = exports / L"current.json";
    if (config.overlay && fs::exists(current)) {
        auto time = modified(current); auto data = parse(read(current, 65536)); auto chart = game_chart(data); bool playing = flag(data, L"playing");
        std::lock_guard lock(gate_);
        if (config.game_root == config_.game_root && time == modified(current)) { current_ = std::move(chart); playing_ = playing; heartbeat_ = time; }
    }
    auto result = exports / L"result.json";
    if (config.sync && fs::exists(result)) {
        auto time = modified(result); auto data = result_from_json(parse(read(result, 65536)));
        if (time == modified(result) && config.game_root == this->config().game_root) capture(data, time);
    }
}
void Engine::replace_scores(const std::vector<Score>& scores) { best_.clear(); for (const auto& score : scores) best_.insert_or_assign(score.chart_id, score); }
void Engine::complete(size_t index, const std::string& outcome) {
    auto previous = data_; data_.receipts[data_.pending[index].result.id] = outcome; data_.pending.erase(data_.pending.begin() + static_cast<ptrdiff_t>(index));
    try { save_store(folder_, data_); } catch (...) { data_ = std::move(previous); throw; }
}
void Engine::sync() {
    std::lock_guard serial(sync_gate_);
    std::string token, mix; unsigned generation; bool need_catalog;
    {
        std::lock_guard lock(gate_); if (token_.empty() || (!config_.sync && !config_.overlay)) return;
        token = token_; mix = config_.mix; generation = generation_; need_catalog = catalog_.empty();
    }
    auto charts = need_catalog ? api_.catalog(token, mix) : std::vector<Chart>{}; auto scores = api_.scores(token, mix);
    for (auto& chart : charts) chart.title_key = normalized(chart.title);
    {
        std::lock_guard lock(gate_); if (generation != generation_) return;
        if (need_catalog) catalog_ = std::move(charts); replace_scores(scores);
        status_ = config_.sync ? "Connected. PB syncing is enabled." : "Connected. OBS overlay only.";
        if (!config_.sync) return;
    }
    size_t index = 0;
    for (;;) {
        Play play; std::string event;
        {
            std::lock_guard lock(gate_); if (generation != generation_ || stopping_ || index >= data_.pending.size()) break;
            auto& item = data_.pending[index]; if (item.mix != mix) { ++index; continue; }
            auto id = match(item.result.chart, catalog_);
            if (id.empty()) { status_ = "Chart not matched: " + item.result.chart.title; complete(index, "unmatched"); continue; }
            auto found = best_.find(id); const Score* previous = found == best_.end() ? nullptr : &found->second;
            if (!better(item.result, previous)) { complete(index, "covered"); continue; }
            if (item.state != "queued") { status_ = "An upload needs checking on PIU Scores. It will not be resubmitted automatically."; ++index; continue; }
            item.state = "sending"; try { save_store(folder_, data_); } catch (...) { item.state = "queued"; throw; }
            event = item.result.id; play = {id, item.result.score, item.result.broken, item.played_at};
        }
        std::string failure_state, failure_message;
        try { api_.upload(token, mix, play); }
        catch (const HttpError& error) {
            bool retryable = error.status == 401 || error.status == 403 || error.status == 429;
            bool rejected = error.status == 400 || error.status == 404 || error.status == 409 || error.status == 422;
            failure_state = retryable ? "queued" : rejected ? "rejected" : "uncertain"; failure_message = error.what();
        }
        catch (...) { failure_state = "uncertain"; failure_message = "Upload response lost. Checking PIU Scores before another upload."; }
        {
            std::lock_guard lock(gate_);
            if (index >= data_.pending.size() || data_.pending[index].result.id != event) throw Error("Upload queue changed unexpectedly.");
            if (!failure_state.empty()) { data_.pending[index].state = failure_state; save_store(folder_, data_); status_ = failure_message; break; }
            complete(index, "accepted");
        }
        // A failed refresh cannot undo a confirmed POST or change its receipt to uncertain.
        auto refreshed = api_.scores(token, mix);
        std::lock_guard lock(gate_); if (generation != generation_) return; replace_scores(refreshed);
    }
}
std::string Engine::state(uint64_t clock) const {
    std::lock_guard lock(gate_);
    bool playing = config_.overlay && playing_ && clock >= heartbeat_ && clock - heartbeat_ < 40000000;
    std::string id = playing ? match(current_, catalog_) : ""; auto found = best_.find(id);
    Object state; put(state, L"playing", playing); put(state, L"song", playing ? current_.title : ""); put(state, L"difficulty", playing ? current_.difficulty : "");
    if (found != best_.end() && found->second.score) put(state, L"pb", *found->second.score); else state.Insert(L"pb", Value::CreateNullValue());
    return encode(state);
}
void Engine::start() {
    if (poll_thread_.joinable()) return; stopping_ = false;
    poll_thread_ = std::thread([this] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        while (!stopping_) {
            try { poll(); } catch (...) { /* Partial game writes are retried on the next tick. */ }
            std::unique_lock lock(wake_gate_); wake_.wait_for(lock, std::chrono::milliseconds(200), [this] { return stopping_.load(); });
        }
    });
    sync_thread_ = std::thread([this] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        auto next = std::chrono::steady_clock::now();
        while (!stopping_) {
            if (sync_due_.exchange(false) || std::chrono::steady_clock::now() >= next) {
                try { sync(); } catch (...) { std::lock_guard lock(gate_); status_ = "PIU Scores unavailable. Will retry automatically."; }
                next = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            }
            std::unique_lock lock(wake_gate_); wake_.wait_for(lock, std::chrono::milliseconds(250), [this] { return stopping_.load(); });
        }
    });
}
void Engine::stop() { stopping_ = true; wake_.notify_all(); if (poll_thread_.joinable()) poll_thread_.join(); if (sync_thread_.joinable()) sync_thread_.join(); }
}
