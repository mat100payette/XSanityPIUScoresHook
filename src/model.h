#pragma once
#include "platform.h"
#include <array>

namespace piu {
struct PlayerAccount {
    std::string profile;
    std::string protected_token;
};

struct AccountInput {
    std::string profile;
    std::string token;
};

struct Preferences {
    std::string mix = "Phoenix";
    std::array<PlayerAccount, 2> accounts;
    fs::path game_root;
    bool sync = true;
    bool overlay = false;
    uint64_t capture_after = 0;
};

struct GameChart {
    std::string title;
    std::string type;
    std::string difficulty;
    int level = 0;
};

struct Chart {
    std::string id;
    std::string title;
    std::string type;
    int level = 0;
    std::string title_key;
};

struct Score {
    std::string chart_id;
    std::optional<int> score;
    bool broken = false;
};

struct Result {
    std::string id;
    GameChart chart;
    int score = 0;
    bool broken = false;
    bool eligible = false;
    std::string plate;
    std::string profile;
    int side = 0;
    bool ambiguous = false;
    std::string skip_reason;
};

struct Pending {
    Result result;
    std::string mix;
    std::string played_at;
    std::string state = "queued";
    int account = 0;
};

struct Store {
    std::vector<Pending> pending;
    std::map<std::string, std::string> receipts;
};

struct Play {
    std::string chart_id;
    int score = 0;
    bool broken = false;
    std::string played_at;
    std::string plate;
};

std::string trim_profile(std::string_view name);
bool same_profile(std::string_view left, std::string_view right);
Preferences load_preferences(const fs::path& folder);
void save_preferences(const fs::path& folder, const Preferences& config);
Store load_store(const fs::path& folder);
void save_store(const fs::path& folder, const Store& store);
GameChart game_chart(const Object& object);
Result result_from_json(const Object& object);
Object result_json(const Result& result);
Object play_json(const Play& play);
bool better(const Result& candidate, const Score* previous);
std::string match(const GameChart& game, const std::vector<Chart>& catalog);
} // namespace piu
