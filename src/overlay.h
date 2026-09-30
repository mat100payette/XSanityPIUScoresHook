#pragma once
#include "platform.h"

namespace piu {
class OverlayServer {
    std::function<std::string()> state_;
    std::string page_;
    SOCKET listener_ = INVALID_SOCKET;
    std::atomic<bool> stopping_{false};
    std::thread worker_;
    unsigned short port_ = 0;
    bool sockets_ = false;
    void serve(SOCKET client);

public:
    OverlayServer(std::function<std::string()> state, std::string page = resource(OverlayResource))
        : state_(std::move(state)), page_(std::move(page)) {
    }

    ~OverlayServer() {
        stop();
    }

    void start(bool enabled, unsigned short port = 8765);
    void stop();

    unsigned short port() const {
        return port_;
    }

    static int request_status(std::string_view header, unsigned short port, std::string& path);
};
} // namespace piu
