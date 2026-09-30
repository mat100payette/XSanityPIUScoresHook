#pragma once
#include "api.h"
#include <condition_variable>

namespace piu {
class Engine {
    fs::path folder_;
    Api& api_;
    mutable std::mutex gate_;
    std::mutex sync_gate_;
    Preferences config_;
    Store data_;
    std::string token_;
    std::string status_ = "Enter your PIU Scores token in Account settings.";
    std::vector<Chart> catalog_;
    std::map<std::string, Score> best_;
    GameChart current_;
    bool playing_ = false;
    uint64_t heartbeat_ = 0;
    unsigned generation_ = 0;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> sync_due_{true};
    std::condition_variable wake_;
    std::mutex wake_gate_;
    std::thread poll_thread_;
    std::thread sync_thread_;
    void complete(size_t index, const std::string& outcome);
    void replace_scores(const std::vector<Score>& scores);
    void poll_loop();
    void sync_loop();

public:
    Engine(fs::path folder, Api& api) : folder_(std::move(folder)), api_(api) {
    }

    ~Engine() {
        stop();
    }

    void load();
    Preferences config() const;
    Store store() const;
    std::string token() const;
    std::string status() const;
    void configure(const std::string& token, const std::string& mix);
    void capture(const Result& result, uint64_t written);
    void poll();
    void sync();
    void start();
    void stop();
    std::string state(uint64_t clock = now()) const;
};
} // namespace piu
