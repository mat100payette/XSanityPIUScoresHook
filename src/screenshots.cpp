#include "screenshots.h"
#include "game_hook.h"
#include <algorithm>

namespace piu {
namespace {
std::wstring filename_part(std::string_view value) {
    auto text = wide(value);
    for (auto& c : text) {
        if (c < 32 || std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos) {
            c = L'_';
        }
    }
    if (text.size() > 55) {
        text.resize(55);
    }
    while (!text.empty() &&
           (text.back() == L' ' || text.back() == L'.' || (text.back() >= 0xD800 && text.back() <= 0xDBFF))) {
        text.pop_back();
    }
    return text.empty() ? L"Unknown" : text;
}

std::wstring shot_name(const Result& result, const std::string& stamp, const std::string& key) {
    auto date = wide(stamp.substr(0, 19));
    std::replace(date.begin(), date.end(), L':', L'-');
    return date + L" - " + filename_part(result.chart.title) + L" " + filename_part(result.chart.difficulty) +
           L" - " + filename_part(result.profile) + L" - " + std::to_wstring(result.score) +
           (result.broken ? L" failed" : L"") + L" - " + wide(key.substr(0, 12)) + L".png";
}

bool managed_key(std::wstring_view name) {
    return name.size() == 64 && name.find_first_not_of(L"0123456789abcdef") == std::wstring_view::npos;
}
} // namespace

void validate_screenshot_folder(const fs::path& folder, const fs::path& game_root, const fs::path& staging) {
    if (folder.empty() || !folder.is_absolute()) {
        throw Error("Choose an absolute screenshot folder.");
    }
    safe_path(folder);
    auto target = fs::weakly_canonical(folder);
    for (const auto& protected_folder : {game_root, staging}) {
        if (protected_folder.empty()) {
            continue;
        }
        auto base = fs::weakly_canonical(protected_folder).wstring();
        auto value = target.wstring();
        if (value.size() >= base.size() &&
            CompareStringOrdinal(value.c_str(),
                static_cast<int>(base.size()),
                base.c_str(),
                static_cast<int>(base.size()),
                TRUE) == CSTR_EQUAL &&
            (value.size() == base.size() || value[base.size()] == L'\\' || value[base.size()] == L'/')) {
            throw Error(
                "Choose a screenshot folder outside XSanity and the companion's temporary capture folder.");
        }
    }
    if (fs::exists(target) && !fs::is_directory(target)) {
        throw Error("Choose a folder, not a file.");
    }
}

void Screenshots::failed() noexcept {
    error_ = "Screenshot could not be saved. Check the folder and keep the game window visible at results.";
}

std::string Screenshots::error() const {
    std::lock_guard lock(gate_);
    return error_;
}

void Screenshots::persist(const std::string& key, const Shot& shot) {
    Object value;
    put(value, L"Destination", utf8(shot.destination.wstring()));
    put(value, L"Stamp", shot.stamp);
    put(value, L"Keeper", shot.keeper);
    put(value, L"Captured", shot.captured);
    Array candidates;
    for (const auto& result : shot.candidates) {
        candidates.Append(result_json(result));
    }
    value.Insert(L"Candidates", candidates);
    safe_path(staging_);
    atomic_write(staging_ / (wide(key) + L".json"), encode(value));
}

void Screenshots::erase(const std::string& key) {
    safe_path(staging_);
    for (const auto* extension : {L".png", L".json"}) {
        fs::remove(staging_ / (wide(key) + extension));
    }
    shots_.erase(key);
}

void Screenshots::finish(const std::string& key) {
    auto& shot = shots_.at(key);
    if (!shot.captured || shot.keeper.empty()) {
        return;
    }
    auto candidate = std::find_if(shot.candidates.begin(), shot.candidates.end(), [&](const auto& result) {
        return result.id == shot.keeper;
    });
    if (candidate == shot.candidates.end()) {
        throw Error("Screenshot candidate missing.");
    }
    safe_path(shot.destination);
    fs::create_directories(shot.destination);
    auto target = shot.destination / shot_name(*candidate, shot.stamp, key);
    auto source = staging_ / (wide(key) + L".png");
    safe_path(source);
    safe_path(target);
    auto bytes = read(source, 64 * 1024 * 1024);
    if (fs::exists(target)) {
        if (read(target, 64 * 1024 * 1024) != bytes) {
            throw Error("Screenshot filename already exists.");
        }
    } else {
        atomic_write(target, bytes);
    }
    erase(key);
    error_.clear();
}

void Screenshots::load(const Store& store, bool enabled) noexcept {
    std::lock_guard lock(gate_);
    try {
        safe_path(staging_);
        if (!fs::exists(staging_)) {
            return;
        }
        std::vector<fs::path> files;
        for (const auto& file : fs::directory_iterator(staging_)) {
            files.push_back(file.path());
        }
        for (const auto& path : files) {
            if (path.extension() != L".json" || !managed_key(path.stem().wstring())) {
                continue;
            }
            auto key = utf8(path.stem().wstring());
            if (!enabled) {
                erase(key);
                continue;
            }
            safe_path(path);
            auto value = parse(read(path));
            Shot shot;
            shot.destination = wide(str(value, L"Destination"));
            shot.stamp = str(value, L"Stamp");
            shot.keeper = str(value, L"Keeper");
            shot.captured = flag(value, L"Captured");
            for (const auto& item : value.GetNamedArray(L"Candidates")) {
                auto result = result_from_json(item.GetObject());
                if (result.id == shot.keeper ||
                    std::any_of(store.pending.begin(), store.pending.end(), [&](const auto& pending) {
                        return pending.result.id == result.id;
                    })) {
                    shot.candidates.push_back(std::move(result));
                }
            }
            if (!shot.captured || shot.candidates.empty()) {
                erase(key);
                continue;
            }
            shots_[key] = std::move(shot);
            finish(key);
        }
        for (const auto& path : files) {
            if (path.extension() == L".png" && managed_key(path.stem().wstring()) &&
                !shots_.contains(utf8(path.stem().wstring()))) {
                fs::remove(path);
            }
        }
    } catch (...) {
        failed();
    }
}

void Screenshots::prepare(
    const std::vector<Result>& results, const ScreenshotOptions& options, uint64_t written) noexcept {
    if (!options.enabled || results.empty()) {
        return;
    }
    std::lock_guard lock(gate_);
    try {
        std::string ids;
        for (const auto& result : results) {
            ids += result.id + "\n";
        }
        auto key = sha256(ids);
        if (shots_.contains(key)) {
            return;
        }
        Shot shot{results, options.folder, iso_time(written)};
        persist(key, shot);
        shots_[key] = std::move(shot);
    } catch (...) {
        failed();
    }
}

void Screenshots::update(const fs::path& root,
    const std::vector<Result>& results,
    bool evaluation,
    double age,
    const std::function<bool()>& still_current) noexcept {
    std::lock_guard lock(gate_);
    try {
        std::vector<std::string> keys;
        for (const auto& [key, shot] : shots_) {
            keys.push_back(key);
        }
        for (const auto& key : keys) {
            auto& shot = shots_.at(key);
            if (shot.captured) {
                finish(key);
                continue;
            }
            bool current =
                evaluation &&
                std::any_of(shot.candidates.begin(), shot.candidates.end(), [&](const auto& wanted) {
                    return std::any_of(results.begin(), results.end(), [&](const auto& value) {
                        return value.id == wanted.id;
                    });
                });
            if (!current) {
                erase(key);
                error_ = "Screenshot missed: the results screen closed before capture.";
                continue;
            }
            if (age < 3) {
                continue;
            }
            try {
                validate_screenshot_folder(shot.destination, root, staging_);
                auto png = capture_(root);
                if (still_current && !still_current()) {
                    erase(key);
                    error_ = "Screenshot missed: the game left this results screen during capture.";
                    continue;
                }
                if (png.empty()) {
                    throw Error("No capture frame.");
                }
                atomic_write(staging_ / (wide(key) + L".png"), png);
                shot.captured = true;
                persist(key, shot);
                finish(key);
            } catch (...) {
                erase(key);
                failed();
            }
        }
    } catch (...) {
        failed();
    }
}

void Screenshots::decide(const std::string& event, bool pb) noexcept {
    std::lock_guard lock(gate_);
    try {
        for (auto& [key, shot] : shots_) {
            auto found =
                std::find_if(shot.candidates.begin(), shot.candidates.end(), [&](const auto& result) {
                    return result.id == event;
                });
            if (found == shot.candidates.end()) {
                continue;
            }
            if (pb) {
                if (shot.keeper.empty()) {
                    shot.keeper = event;
                }
            } else if (shot.keeper != event) {
                shot.candidates.erase(found);
            }
            if (shot.candidates.empty()) {
                erase(key);
            } else {
                persist(key, shot);
                finish(key);
            }
            break;
        }
    } catch (...) {
        failed();
    }
}

void Screenshots::configure(const ScreenshotOptions& options) noexcept {
    if (!options.enabled) {
        clear();
        return;
    }
    std::lock_guard lock(gate_);
    try {
        for (auto& [key, shot] : shots_) {
            shot.destination = options.folder;
            persist(key, shot);
        }
        error_.clear();
    } catch (...) {
        failed();
    }
}

void Screenshots::clear() noexcept {
    std::lock_guard lock(gate_);
    try {
        while (!shots_.empty()) {
            erase(shots_.begin()->first);
        }
        error_.clear();
    } catch (...) {
        failed();
    }
}
} // namespace piu
