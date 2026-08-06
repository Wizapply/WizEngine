#include "PortScan.h"
#include "Log.h"

#include <cstdio>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#define WIZ_INVALID_SOCKET INVALID_SOCKET
#define WIZ_CLOSE_SOCKET closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#define WIZ_INVALID_SOCKET (-1)
#define WIZ_CLOSE_SOCKET close
#endif

namespace wizengine {

namespace {

#ifdef _WIN32
// httplib also calls WSAStartup, but the counts are reference-counted by
// Winsock, so an extra pair here is safe and keeps this file self-contained.
struct WinsockInit {
    WinsockInit() {
        WSADATA data;
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockInit() {
        if (ok) WSACleanup();
    }
    bool ok = false;
};
#endif

}  // namespace

bool portIsFree(int port) {
    if (port < 1 || port > 65535) return false;
#ifdef _WIN32
    static WinsockInit winsock;
    if (!winsock.ok) return true;  // cannot check: let the server try
#endif

    const socket_t sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock == WIZ_INVALID_SOCKET) return true;  // cannot check

    // Deliberately NOT setting SO_REUSEADDR: the question is whether something
    // is listening here, and on POSIX that option would let the bind succeed
    // against a socket in TIME_WAIT and report a busy port as free.
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));

    const bool free_ =
        ::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    WIZ_CLOSE_SOCKET(sock);
    return free_;
}

int findFreePortRun(int start, int count, int maxProbe) {
    if (count < 1) count = 1;
    for (int base = start; base < start + maxProbe; ++base) {
        if (base + count - 1 > 65535) break;
        bool allFree = true;
        for (int i = 0; i < count; ++i) {
            if (!portIsFree(base + i)) {
                allFree = false;
                break;
            }
        }
        if (allFree) return base;
    }
    LOGW("net", "no run of %d free ports found near %d - trying %d anyway",
                count, start, start);
    return start;
}

}  // namespace wizengine
