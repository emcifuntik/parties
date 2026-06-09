#pragma once

// ═══════════════════════════════════════════════════════════════════════════
// Heap allocation tracker — leak hunting instrumentation.
//
// When compiled with PARTIES_ALLOC_TRACKER, alloc_tracker.cpp (which must be
// compiled directly into the executable, not a static lib, so the linker
// prefers its operator new/delete over the CRT's) replaces the global
// new/delete with a tracking allocator:
//
//   - Every allocation gets a 32-byte header recording its exact size and
//     the ID of the (truncated) call stack that made it.
//   - Live bytes/counts are accounted per power-of-two size bucket and per
//     unique call stack.
//   - A background thread periodically logs a report: total heap growth
//     rate, per-bucket growth, and the top call stacks ranked by live-byte
//     growth since the previous report — symbolized via DbgHelp. A steady
//     leak shows up as a stack whose live bytes grow linearly forever.
//
// Coverage: all C++ new/delete in the executable and statically linked libs
// (RmlUi, Opus, MsQuic, spdlog, ...). NOT covered: plain malloc in C code
// (rnnoise, dav1d, miniaudio), HeapAlloc/VirtualAlloc (GPU drivers, MFT,
// system DLLs). If the report shows no growth but the process grows, the
// leak is in one of those — different tool (ETW heap trace / driver).
//
// Usage:
//   configure: cmake --preset default -DPARTIES_ALLOC_TRACKER=ON
//   runtime:   parties::alloctrack::start_reporting(10) early in main();
//              reports go to the normal client log every 10 s.
//   tagging:   PARTIES_MEM_SCOPE("voice_rx"); annotates every stack first
//              observed inside the scope with a human-readable comment.
// ═══════════════════════════════════════════════════════════════════════════

#include <cstddef>
#include <cstdint>

namespace parties::alloctrack {

#ifdef PARTIES_ALLOC_TRACKER

// Start the periodic report thread. Safe to call once, early in main().
void start_reporting(unsigned interval_seconds);

// Stop the report thread (call before log_shutdown so the final report
// still reaches the log).
void stop_reporting();

// Emit one report immediately on the calling thread.
void dump_report(const char* reason);

// Total live tracked bytes right now.
std::uint64_t live_bytes();

// Thread-local allocation tag ("comment"). Stacks first seen while a tag is
// active carry it in reports. Use via PARTIES_MEM_SCOPE below.
struct TagScope {
    explicit TagScope(const char* tag) noexcept;
    ~TagScope() noexcept;
private:
    const char* prev_;
};

#define PARTIES_MEM_SCOPE(tag) ::parties::alloctrack::TagScope _parties_mem_scope_##__LINE__(tag)

#else // !PARTIES_ALLOC_TRACKER — everything compiles away

inline void start_reporting(unsigned) {}
inline void stop_reporting() {}
inline void dump_report(const char*) {}
inline std::uint64_t live_bytes() { return 0; }

#define PARTIES_MEM_SCOPE(tag) ((void)0)

#endif

} // namespace parties::alloctrack
