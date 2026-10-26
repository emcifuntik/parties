#pragma once

#include <cstdint>
#include <optional>

namespace parties {

enum class DatagramPriority : uint8_t { Normal, AheadOfVideo };

class NetLibrary {
public:
    static std::optional<NetLibrary> open();
    NetLibrary(NetLibrary&& other) noexcept;
    NetLibrary& operator=(NetLibrary&&) = delete;
    NetLibrary(const NetLibrary&) = delete;
    NetLibrary& operator=(const NetLibrary&) = delete;
    ~NetLibrary();

private:
    NetLibrary() = default;
    bool owned_ = true;
};

}
