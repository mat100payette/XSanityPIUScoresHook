#pragma once
#include "model.h"

namespace piu {
// Only the latest attempt on each side is retained; never account or score data.
class ResultFeedback {
    struct Notice {
        std::string id;
        std::string code;
        uint64_t expires = 0;
    };

    std::array<Notice, 2> notices_;

public:
    void observe(const Result& result, std::string code, uint64_t written);
    void update(const std::string& id, const std::string& code);
    std::string snapshot(uint64_t clock = now()) const;
};
} // namespace piu
