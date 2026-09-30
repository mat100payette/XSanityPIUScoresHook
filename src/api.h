#pragma once
#include "model.h"

namespace piu {
class HttpError : public Error {
public:
    int status;
    explicit HttpError(int code) : Error("PIU Scores returned HTTP " + std::to_string(code) + "."), status(code) {}
};
class Api {
public:
    virtual ~Api() = default;
    virtual std::vector<Chart> catalog(const std::string& token, const std::string& mix) = 0;
    virtual std::vector<Score> scores(const std::string& token, const std::string& mix) = 0;
    virtual void upload(const std::string& token, const std::string& mix, const Play& play) = 0;
};
using Transport = std::function<Object(const std::string&, const std::string&, const std::optional<Object>&)>;
class PiuScoresApi final : public Api {
    Transport transport_;
    std::vector<Object> pages(const std::string& route, const std::string& token, const std::string& mix, bool scores);
    Object request(const std::string& url, const std::string& token, const std::optional<Object>& body = std::nullopt);
public:
    explicit PiuScoresApi(Transport transport = {});
    static bool allowed(const std::string& url);
    std::vector<Chart> catalog(const std::string& token, const std::string& mix) override;
    std::vector<Score> scores(const std::string& token, const std::string& mix) override;
    void upload(const std::string& token, const std::string& mix, const Play& play) override;
};
}
