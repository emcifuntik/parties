#pragma once

#include <msquic.h>

#include <optional>

namespace parties {

class QuicLibrary {
public:
    static std::optional<QuicLibrary> open();
    QuicLibrary(QuicLibrary&& other) noexcept;
    QuicLibrary& operator=(QuicLibrary&&) = delete;
    QuicLibrary(const QuicLibrary&) = delete;
    QuicLibrary& operator=(const QuicLibrary&) = delete;
    ~QuicLibrary();

    const QUIC_API_TABLE& api() const { return *api_; }

private:
    explicit QuicLibrary(const QUIC_API_TABLE* api) : api_(api) {}
    const QUIC_API_TABLE* api_;
};

const QUIC_API_TABLE* quic_api();

inline QUIC_BUFFER make_alpn() {
    static uint8_t alpn[] = "parties";
    return { 7, alpn };
}

}
