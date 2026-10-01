#include "result_feedback.h"

namespace piu {
void ResultFeedback::observe(const Result& result, std::string code, uint64_t written) {
    if (result.side < 1 || result.side > 2 || result.id.empty() || result.id.size() > 128 ||
        result.id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
            std::string::npos) {
        return;
    }

    auto& notice = notices_[result.side - 1];
    if (notice.id != result.id) {
        notice = {result.id, std::move(code), written + 1200000000};
    }
}

void ResultFeedback::update(const std::string& id, const std::string& code) {
    for (auto& notice : notices_) {
        if (notice.id == id) {
            notice.code = code;
        }
    }
}

std::string ResultFeedback::snapshot(uint64_t clock) const {
    std::string text = "PIUCOMPANION 1\n";
    for (const auto& notice : notices_) {
        if (!notice.id.empty() && clock < notice.expires) {
            text += notice.id + "\t" + notice.code + "\n";
        }
    }

    return text;
}
} // namespace piu
