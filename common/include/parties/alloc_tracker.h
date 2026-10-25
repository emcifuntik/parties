#pragma once

#include <cstddef>
#include <cstdint>

namespace parties::alloctrack {

#ifdef PARTIES_ALLOC_TRACKER

void start_reporting(unsigned interval_seconds);

void stop_reporting();

void dump_report(const char* reason);

std::uint64_t live_bytes();

struct TagScope {
    explicit TagScope(const char* tag) noexcept;
    ~TagScope() noexcept;
private:
    const char* prev_;
};

#define PARTIES_MEM_SCOPE(tag) ::parties::alloctrack::TagScope _parties_mem_scope_##__LINE__(tag)

#else

inline void start_reporting(unsigned) {}
inline void stop_reporting() {}
inline void dump_report(const char*) {}
inline std::uint64_t live_bytes() { return 0; }

#define PARTIES_MEM_SCOPE(tag) ((void)0)

#endif

}
