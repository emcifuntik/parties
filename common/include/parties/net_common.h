#pragma once

#include <optional>

namespace parties {

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
