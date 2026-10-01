#include "support.h"
#include "overlay.h"

namespace piu::test {
std::string get(unsigned short port, const std::string& header) {
    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == INVALID_SOCKET) {
        throw Error("test socket");
    }

    struct Close {
        SOCKET socket;

        ~Close() {
            closesocket(socket);
        }
    } close{client};

    DWORD timeout = 2000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address))) {
        throw Error("test connect");
    }

    check(send(client, header.data(), static_cast<int>(header.size()), 0) == static_cast<int>(header.size()),
        "complete HTTP request sent");
    std::string response;
    char buffer[1024];
    for (;;) {
        int count = recv(client, buffer, sizeof(buffer), 0);
        if (count == SOCKET_ERROR) {
            throw Error("Overlay test receive failed or timed out: " + std::to_string(WSAGetLastError()));
        }

        if (count == 0) {
            break;
        }

        response.append(buffer, static_cast<size_t>(count));
    }

    return response;
}

void overlay_checks() {
    OverlayServer disabled([] {
        return "{}";
    });
    disabled.start(false, 0);
    check(disabled.port() == 0, "disabled overlay opens no listener");
    OverlayServer server([] {
        return R"({"playing":true,"song":"Song","difficulty":"S10","pb":950000})";
    });
    server.start(true, 0);
    auto port = server.port();
    auto header = " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n\r\n";
    check(get(port, "GET /state" + header).find("\"pb\":950000") != std::string::npos,
        "local listener serves authoritative state");
    check(get(port, "GET /overlay" + header).find("<small>PB</small>") != std::string::npos,
        "embedded overlay page is served");
    check(get(port, "POST /state" + header).starts_with("HTTP/1.1 403"), "OBS listener refuses writes");
    check(
        get(port, "GET /missing" + header).starts_with("HTTP/1.1 404"), "OBS listener rejects unknown route");
    check(get(port, "GET /state HTTP/1.1\r\nHost: evil.test\r\n\r\n").starts_with("HTTP/1.1 403"),
        "foreign Host cannot access local state");
    std::string path;
    check(OverlayServer::request_status(
              "GET /state HTTP/1.1\r\nHost: localhost:8765\r\nHost: evil.test\r\n\r\n", 8765, path) == 403,
        "duplicate Host rejected");
    server.stop();
    check(server.port() == 0, "overlay stops and releases its port");
    server.start(true, port);
    check(server.port() == port, "overlay can restart on released port");
}

} // namespace piu::test
