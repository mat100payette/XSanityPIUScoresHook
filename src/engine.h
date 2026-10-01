#pragma once
#include "api.h"
#include "mailbox.h"
#include <condition_variable>

namespace piu {
class Engine {
    fs::path folder_;
    Api& api_;
    MailboxReader mailbox_;
    ExportFeed feed_;
    mutable std::mutex gate_;
    std::mutex sync_gate_;
    Preferences config_;
    Store data_;
    std::array<std::string, 2> tokens_;
    std::array<std::string, 2> account_status_;
    std::array<std::string, 2> live_profiles_;
    std::string status_ = "Enter your XSanity profile and PIU Scores API key in Account settings.";
    std::vector<Chart> catalog_;
    std::array<std::map<std::string, Score>, 2> best_;
    GameChart current_;
    bool playing_ = false;
    uint64_t heartbeat_ = 0;
    std::string export_error_;
    unsigned generation_ = 0;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> sync_due_{true};
    std::condition_variable wake_;
    std::mutex wake_gate_;
    std::thread poll_thread_;
    std::thread sync_thread_;
    void complete(size_t index, const std::string& outcome);
    void replace_scores(size_t account, const std::vector<Score>& scores);
    void sync_account(size_t account);
    int account_for(const std::string& profile) const;
    void poll_loop();
    void sync_loop();

public:
    Engine(fs::path folder, Api& api, ExportFeed feed = {})
        : folder_(std::move(folder)), api_(api), feed_(std::move(feed)) {
    }

    ~Engine() {
        stop();
    }

    void load();
    Preferences config() const;
    Store store() const;
    std::string token(size_t account = 0) const;
    std::string player_status(uint64_t clock = now()) const;
    std::string status(uint64_t clock = now()) const;
    void configure(const std::array<AccountInput, 2>& accounts, const std::string& mix);
    void capture(const Result& result, uint64_t written);
    size_t pending_count(size_t account) const;
    void discard_pending(size_t account);
    void poll();
    void sync();
    void start();
    void stop();
    std::string state(uint64_t clock = now()) const;
};
} // namespace piu
