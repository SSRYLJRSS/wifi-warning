#include "core/http_probe.h"

#include <winsock2.h>

namespace ww {

// v1.9: RAII guard for Winsock — avoids redundant WSAStartup/WSACleanup per call.
// Winsock allows multiple WSAStartup calls (refcount-based), but cleaning up per
// probe is wasteful. The guard ensures cleanup on scope exit.
struct WsaGuard {
    WsaGuard() { WSAStartup(MAKEWORD(2, 2), &data_); }
    ~WsaGuard() { WSACleanup(); }
    WsaGuard(const WsaGuard&) = delete;
    WsaGuard& operator=(const WsaGuard&) = delete;
    WSADATA data_{};
};

bool isLocalHttpAlive(int port, int timeoutMs) {
    WsaGuard wssock;
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return false;
    DWORD timeout = static_cast<DWORD>(timeoutMs);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<u_short>(port));
    bool ok = connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != SOCKET_ERROR;
    closesocket(sock);
    return ok;
}

}
