#pragma once
#include "platform.h"

namespace piu {
struct Preferences {
    std::string mix = "Phoenix", protected_token;
    fs::path game_root;
    bool sync = true, overlay = false;
    uint64_t capture_after = 0;
};
struct GameChart { std::string title, type, difficulty; int level = 0; };
struct Chart { std::string id, title, type; int level = 0; std::string title_key; };
struct Score { std::string chart_id; std::optional<int> score; bool broken = false; };
struct Result { std::string id; GameChart chart; int score = 0; bool broken = false, eligible = false; };
struct Pending { Result result; std::string mix, played_at, state = "queued"; };
struct Store { std::vector<Pending> pending; std::map<std::string, std::string> receipts; };
struct Play { std::string chart_id; int score = 0; bool broken = false; std::string played_at; };
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
}
