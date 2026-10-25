#include <parties/net_common.h>

#ifdef _WIN32
#  include <winsock2.h>
#endif

namespace parties {

bool net_init() {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return true;
#endif
}

void net_cleanup() {
#ifdef _WIN32
    WSACleanup();
#endif
}

}
