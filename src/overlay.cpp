#include "overlay.h"
#include <algorithm>

namespace piu {
int OverlayServer::request_status(std::string_view header, unsigned short port, std::string& path) {
    auto line = header.find("\r\n"); if (line == std::string_view::npos) return 400;
    auto first = header.substr(0, line); if (!first.starts_with("GET ")) return 403;
    auto space = first.find(' ', 4); if (space == std::string_view::npos || first.substr(space) != " HTTP/1.1") return 400;
    path = first.substr(4, space - 4); std::string host; unsigned host_count = 0;
    for (size_t offset = line + 2; offset < header.size();) {
        auto end = header.find("\r\n", offset); if (end == std::string_view::npos || end == offset) break;
        auto row = header.substr(offset, end - offset); auto colon = row.find(':');
        if (colon != std::string_view::npos) {
            std::string key(row.substr(0, colon)); for (char& c : key) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            if (key == "host") { ++host_count; auto value = row.substr(colon + 1); auto begin = value.find_first_not_of(" \t"), last = value.find_last_not_of(" \t"); if (begin != std::string_view::npos) host = value.substr(begin, last - begin + 1); }
        }
        offset = end + 2;
    }
    if (host_count != 1 || (host != "127.0.0.1:" + std::to_string(port) && host != "localhost:" + std::to_string(port))) return 403;
    return path == "/" || path == "/overlay" || path == "/state" ? 200 : 404;
}
void OverlayServer::start(bool enabled, unsigned short port) {
    if (!enabled || worker_.joinable()) return;
    WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data)) throw Error("Could not initialize the OBS connection."); sockets_ = true;
    try {
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); if (listener_ == INVALID_SOCKET) throw Error("Could not create the OBS connection.");
        BOOL exclusive = TRUE; if (setsockopt(listener_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char*>(&exclusive), sizeof(exclusive))) throw Error("Could not protect the OBS port.");
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
        if (bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(listener_, 4)) throw Error("OBS port 8765 is already in use. Close the other companion or disable the overlay in setup.");
        int length = sizeof(address); if (getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length)) throw Error("Could not read the OBS port."); port_ = ntohs(address.sin_port); stopping_ = false;
        worker_ = std::thread([this] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            while (!stopping_) {
                fd_set ready; FD_ZERO(&ready); FD_SET(listener_, &ready); timeval timeout{0, 200000};
                if (select(0, &ready, nullptr, nullptr, &timeout) <= 0) continue;
                SOCKET client = accept(listener_, nullptr, nullptr); if (client == INVALID_SOCKET) continue;
                try { serve(client); } catch (...) { /* Closing a browser must not stop the companion. */ }
                closesocket(client);
            }
        });
    } catch (...) { stop(); throw; }
}
void OverlayServer::serve(SOCKET client) {
    DWORD timeout = 2000; setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout)); setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
    std::string header; char bytes[1024]; auto deadline = GetTickCount64() + 2000;
    while (header.find("\r\n\r\n") == std::string::npos) {
        if (header.size() >= 8192 || GetTickCount64() >= deadline || stopping_) return;
        int got = recv(client, bytes, static_cast<int>(sizeof(bytes)), 0); if (got <= 0) return; header.append(bytes, static_cast<size_t>(got));
    }
    std::string path; int code = request_status(header, port_, path); bool is_state = path == "/state";
    std::string body = code == 200 ? (is_state ? state_() : page_) : "Unavailable";
    std::string response = "HTTP/1.1 " + std::to_string(code) + (code == 200 ? " OK" : " Error") + "\r\nContent-Type: " + (is_state ? "application/json" : "text/html; charset=utf-8") +
        "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    size_t sent = 0;
    while (sent < response.size() && !stopping_) { int count = send(client, response.data() + sent, static_cast<int>(response.size() - sent), 0); if (count <= 0) break; sent += static_cast<size_t>(count); }
}
void OverlayServer::stop() { stopping_ = true; if (worker_.joinable()) worker_.join(); if (listener_ != INVALID_SOCKET) { closesocket(listener_); listener_ = INVALID_SOCKET; } if (sockets_) { WSACleanup(); sockets_ = false; } port_ = 0; }
}
