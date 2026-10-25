#include <parties/net_common.h>

#include <utility>

#ifdef _WIN32
#  include <winsock2.h>
#endif

namespace parties {

std::optional<NetLibrary> NetLibrary::open() {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return std::nullopt;
#endif
    return NetLibrary();
}

NetLibrary::NetLibrary(NetLibrary&& other) noexcept
    : owned_(std::exchange(other.owned_, false)) {}

NetLibrary::~NetLibrary() {
#ifdef _WIN32
    if (owned_) WSACleanup();
#endif
}

}
