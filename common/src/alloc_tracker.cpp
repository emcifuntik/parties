// ═══════════════════════════════════════════════════════════════════════════
// Heap allocation tracker — see alloc_tracker.h for the overview.
//
// This TU must be compiled INTO THE EXECUTABLE (it is listed in the client's
// source list, not in parties_common): the MSVC/lld linker only prefers our
// operator new/delete over the CRT's when the defining object file is an
// explicit input, not pulled from a library on demand.
//
// Internals allocate exclusively with malloc/free — never new — so the
// overrides cannot recurse into themselves.
// ═══════════════════════════════════════════════════════════════════════════

#ifdef PARTIES_ALLOC_TRACKER

#include <parties/alloc_tracker.h>
#include <parties/log.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>

#pragma comment(lib, "dbghelp.lib")

namespace parties::alloctrack {
namespace {

// ── Allocation header ────────────────────────────────────────────────────────
// Prepended to every tracked block. 32 bytes keeps malloc's 16-byte user
// alignment intact. magic is XOR-mixed with the user pointer so a stale or
// foreign header can't match by accident.

constexpr std::uint64_t MAGIC = 0xA110C8ED'BEEFCAFEull;
constexpr std::size_t HEADER_SIZE = 32;

struct AllocHeader {
    std::uint64_t magic;     // MAGIC ^ (uintptr of user pointer)
    std::uint64_t size;      // exact requested size
    std::uint32_t stack_id;  // 1-based index into stack table, 0 = none
    std::uint32_t flags;     // FLAG_*
    std::uint64_t reserved;  // pad to 32
};
static_assert(sizeof(AllocHeader) == HEADER_SIZE);

constexpr std::uint32_t FLAG_ALIGNED = 1; // block from _aligned_offset_malloc

// ── Size buckets (powers of two) ─────────────────────────────────────────────

constexpr int BUCKET_COUNT = 24; // bucket i covers (2^(i+3), 2^(i+4)]: 16B .. >64MB
constexpr int bucket_for(std::size_t size) {
    int w = static_cast<int>(std::bit_width(size | 1)); // 1..64
    int b = w - 5;                                      // ≤16B → bucket 0
    if (size > (std::size_t{1} << (w - 1))) b += 1;     // not exact power of two
    return b < 0 ? 0 : (b >= BUCKET_COUNT ? BUCKET_COUNT - 1 : b);
}

struct BucketStats {
    std::atomic<std::int64_t> live_bytes;
    std::atomic<std::int64_t> live_count;
    std::atomic<std::uint64_t> total_allocs;
};
BucketStats g_buckets[BUCKET_COUNT]; // zero-init in BSS, valid before main()

std::atomic<std::int64_t> g_live_bytes;
std::atomic<std::int64_t> g_live_count;
std::atomic<std::uint64_t> g_total_allocs;
std::atomic<std::uint64_t> g_foreign_frees; // deletes of blocks we didn't allocate

// ── Call-stack table ─────────────────────────────────────────────────────────
// Fixed-size open-addressing hash table in static storage; entries are
// claimed once with a CAS and never removed. ~8 MB — debug tooling budget.

constexpr int MAX_FRAMES = 12;
constexpr int SKIP_FRAMES = 2;        // capture helper + operator new itself
constexpr std::uint32_t STACK_TABLE_SIZE = 32768; // power of two

struct StackEntry {
    std::atomic<std::uint64_t> key;   // stack hash, 0 = empty
    std::atomic<std::uint32_t> ready; // frames[] fully written
    void* frames[MAX_FRAMES];
    std::uint32_t frame_count;
    const char* tag;                  // PARTIES_MEM_SCOPE comment, may be null
    std::atomic<std::int64_t> live_bytes;
    std::atomic<std::int64_t> live_count;
    std::atomic<std::uint64_t> total_allocs;
    std::int64_t report_prev_live;    // only touched by the report thread
};
StackEntry g_stacks[STACK_TABLE_SIZE];
std::atomic<std::uint32_t> g_stack_table_full;

thread_local const char* t_tag = nullptr;

// Returns 1-based stack id, 0 if capture failed or the table is full.
std::uint32_t capture_stack() {
    void* frames[MAX_FRAMES];
    USHORT n = RtlCaptureStackBackTrace(SKIP_FRAMES, MAX_FRAMES, frames, nullptr);
    if (n == 0)
        return 0;

    std::uint64_t h = 0xcbf29ce484222325ull; // FNV-1a over the frame pointers
    for (USHORT i = 0; i < n; ++i) {
        h ^= reinterpret_cast<std::uint64_t>(frames[i]);
        h *= 0x100000001b3ull;
    }
    if (h == 0) h = 1;

    std::uint32_t idx = static_cast<std::uint32_t>(h) & (STACK_TABLE_SIZE - 1);
    for (std::uint32_t probe = 0; probe < 64; ++probe, idx = (idx + 1) & (STACK_TABLE_SIZE - 1)) {
        StackEntry& e = g_stacks[idx];
        std::uint64_t key = e.key.load(std::memory_order_acquire);
        if (key == h)
            return idx + 1;
        if (key == 0) {
            std::uint64_t expected = 0;
            if (e.key.compare_exchange_strong(expected, h, std::memory_order_acq_rel)) {
                std::memcpy(e.frames, frames, n * sizeof(void*));
                e.frame_count = n;
                e.tag = t_tag;
                e.ready.store(1, std::memory_order_release);
                return idx + 1;
            }
            if (expected == h) // lost the race to the same stack
                return idx + 1;
        }
    }
    g_stack_table_full.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

// ── Core alloc/free ──────────────────────────────────────────────────────────

void record_alloc(void* user, std::size_t size, std::uint32_t flags) {
    auto* h = reinterpret_cast<AllocHeader*>(static_cast<char*>(user) - HEADER_SIZE);
    h->magic = MAGIC ^ reinterpret_cast<std::uint64_t>(user);
    h->size = size;
    h->flags = flags;
    h->stack_id = capture_stack();
    h->reserved = 0;

    g_live_bytes.fetch_add(static_cast<std::int64_t>(size), std::memory_order_relaxed);
    g_live_count.fetch_add(1, std::memory_order_relaxed);
    g_total_allocs.fetch_add(1, std::memory_order_relaxed);

    BucketStats& b = g_buckets[bucket_for(size)];
    b.live_bytes.fetch_add(static_cast<std::int64_t>(size), std::memory_order_relaxed);
    b.live_count.fetch_add(1, std::memory_order_relaxed);
    b.total_allocs.fetch_add(1, std::memory_order_relaxed);

    if (h->stack_id) {
        StackEntry& e = g_stacks[h->stack_id - 1];
        e.live_bytes.fetch_add(static_cast<std::int64_t>(size), std::memory_order_relaxed);
        e.live_count.fetch_add(1, std::memory_order_relaxed);
        e.total_allocs.fetch_add(1, std::memory_order_relaxed);
    }
}

void* tracked_alloc(std::size_t size) {
    // size 0 must still return a unique pointer
    void* raw = std::malloc(size + HEADER_SIZE);
    if (!raw)
        return nullptr;
    void* user = static_cast<char*>(raw) + HEADER_SIZE;
    record_alloc(user, size, 0);
    return user;
}

void* tracked_alloc_aligned(std::size_t size, std::size_t align) {
    if (align <= 16)
        return tracked_alloc(size);
    // User block aligned to `align`, header in the 32 bytes right before it.
    void* user = _aligned_offset_malloc(size + HEADER_SIZE, align, HEADER_SIZE);
    if (!user)
        return nullptr;
    user = static_cast<char*>(user) + HEADER_SIZE;
    record_alloc(user, size, FLAG_ALIGNED);
    return user;
}

void tracked_free(void* user) {
    if (!user)
        return;
    auto* h = reinterpret_cast<AllocHeader*>(static_cast<char*>(user) - HEADER_SIZE);
    if (h->magic != (MAGIC ^ reinterpret_cast<std::uint64_t>(user))) {
        // Not ours: allocated before these overrides were linked in (shouldn't
        // happen — they're active from process start) or handed across a
        // module boundary. Pass through.
        g_foreign_frees.fetch_add(1, std::memory_order_relaxed);
        std::free(user);
        return;
    }

    const std::int64_t size = static_cast<std::int64_t>(h->size);
    g_live_bytes.fetch_sub(size, std::memory_order_relaxed);
    g_live_count.fetch_sub(1, std::memory_order_relaxed);

    BucketStats& b = g_buckets[bucket_for(h->size)];
    b.live_bytes.fetch_sub(size, std::memory_order_relaxed);
    b.live_count.fetch_sub(1, std::memory_order_relaxed);

    if (h->stack_id) {
        StackEntry& e = g_stacks[h->stack_id - 1];
        e.live_bytes.fetch_sub(size, std::memory_order_relaxed);
        e.live_count.fetch_sub(1, std::memory_order_relaxed);
    }

    h->magic = 0; // poison against double-free re-matching
    if (h->flags & FLAG_ALIGNED)
        _aligned_free(static_cast<char*>(user) - HEADER_SIZE);
    else
        std::free(static_cast<char*>(user) - HEADER_SIZE);
}

// ── Reporting ────────────────────────────────────────────────────────────────

std::int64_t g_prev_total_live;
std::int64_t g_prev_bucket_live[BUCKET_COUNT];
std::uint64_t g_prev_bucket_allocs[BUCKET_COUNT];
ULONGLONG g_prev_report_ticks;

bool g_sym_initialized = false;

void symbolize(void* addr, char* out, std::size_t out_size) {
    if (!g_sym_initialized) {
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        g_sym_initialized = true;
    }
    alignas(SYMBOL_INFO) char sym_buf[sizeof(SYMBOL_INFO) + 256];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(sym_buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;

    DWORD64 displacement = 0;
    if (SymFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(addr), &displacement, sym)) {
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD line_disp = 0;
        if (SymGetLineFromAddr64(GetCurrentProcess(), reinterpret_cast<DWORD64>(addr), &line_disp, &line)) {
            const char* file = std::strrchr(line.FileName, '\\');
            file = file ? file + 1 : line.FileName;
            std::snprintf(out, out_size, "%s (%s:%lu)", sym->Name, file, line.LineNumber);
        } else {
            std::snprintf(out, out_size, "%s+0x%llx", sym->Name, displacement);
        }
    } else {
        std::snprintf(out, out_size, "0x%p", addr);
    }
}

void format_bytes(std::int64_t v, char* out, std::size_t out_size) {
    const char* sign = v < 0 ? "-" : "";
    double a = static_cast<double>(v < 0 ? -v : v);
    if (a >= 1024.0 * 1024.0)
        std::snprintf(out, out_size, "%s%.2f MB", sign, a / (1024.0 * 1024.0));
    else if (a >= 1024.0)
        std::snprintf(out, out_size, "%s%.1f KB", sign, a / 1024.0);
    else
        std::snprintf(out, out_size, "%s%lld B", sign, static_cast<long long>(v < 0 ? -v : v));
}

std::mutex g_report_mutex; // dump_report is not reentrant (DbgHelp, prev-snapshots)

void do_dump_report(const char* reason) {
    std::lock_guard<std::mutex> lock(g_report_mutex);

    const ULONGLONG now = GetTickCount64();
    const double dt = g_prev_report_ticks ? (now - g_prev_report_ticks) / 1000.0 : 0.0;
    g_prev_report_ticks = now;

    const std::int64_t live = g_live_bytes.load(std::memory_order_relaxed);
    const std::int64_t live_n = g_live_count.load(std::memory_order_relaxed);
    const std::int64_t delta = live - g_prev_total_live;
    g_prev_total_live = live;

    char buf1[32], buf2[32];
    format_bytes(live, buf1, sizeof(buf1));
    format_bytes(delta, buf2, sizeof(buf2));
    LOG_INFO("[alloctrack] ===== report ({}) =====", reason);
    if (dt > 0.0) {
        char rate[32];
        format_bytes(static_cast<std::int64_t>(delta / dt), rate, sizeof(rate));
        LOG_INFO("[alloctrack] live: {} in {} blocks | delta {} in {:.1f}s = {}/s | foreign frees {}",
                 buf1, live_n, buf2, dt, rate,
                 g_foreign_frees.load(std::memory_order_relaxed));
    } else {
        LOG_INFO("[alloctrack] live: {} in {} blocks | foreign frees {}",
                 buf1, live_n, g_foreign_frees.load(std::memory_order_relaxed));
    }

    // Per-bucket: only buckets with movement or significant live volume.
    for (int i = 0; i < BUCKET_COUNT; ++i) {
        const std::int64_t blive = g_buckets[i].live_bytes.load(std::memory_order_relaxed);
        const std::int64_t bn = g_buckets[i].live_count.load(std::memory_order_relaxed);
        const std::uint64_t ballocs = g_buckets[i].total_allocs.load(std::memory_order_relaxed);
        const std::int64_t bdelta = blive - g_prev_bucket_live[i];
        const std::uint64_t balloc_delta = ballocs - g_prev_bucket_allocs[i];
        g_prev_bucket_live[i] = blive;
        g_prev_bucket_allocs[i] = ballocs;
        if (blive < 64 * 1024 && bdelta == 0)
            continue;
        const std::uint64_t hi = std::uint64_t{16} << i;
        format_bytes(blive, buf1, sizeof(buf1));
        format_bytes(bdelta, buf2, sizeof(buf2));
        LOG_INFO("[alloctrack]   bucket ≤{:>8} : live {:>10} ({} blocks) | Δ {:>10} | {} allocs in period",
                 hi, buf1, bn, buf2, balloc_delta);
    }

    // Top stacks by live-byte growth since last report — the leak shows here.
    struct Grower { std::uint32_t idx; std::int64_t delta; std::int64_t live; };
    Grower top[15]{};
    int top_n = 0;
    for (std::uint32_t i = 0; i < STACK_TABLE_SIZE; ++i) {
        StackEntry& e = g_stacks[i];
        if (!e.key.load(std::memory_order_acquire) || !e.ready.load(std::memory_order_acquire))
            continue;
        const std::int64_t elive = e.live_bytes.load(std::memory_order_relaxed);
        const std::int64_t edelta = elive - e.report_prev_live;
        e.report_prev_live = elive;
        if (edelta <= 0)
            continue;
        // insertion sort into the small top array
        int pos = top_n < 15 ? top_n : 14;
        if (top_n < 15) ++top_n;
        else if (top[14].delta >= edelta) continue;
        while (pos > 0 && top[pos - 1].delta < edelta) {
            top[pos] = top[pos - 1];
            --pos;
        }
        top[pos] = {i, edelta, elive};
    }

    if (top_n > 0)
        LOG_INFO("[alloctrack] top call stacks by live-byte growth this period:");
    char sym[320];
    for (int t = 0; t < top_n; ++t) {
        StackEntry& e = g_stacks[top[t].idx];
        format_bytes(top[t].delta, buf1, sizeof(buf1));
        format_bytes(top[t].live, buf2, sizeof(buf2));
        LOG_INFO("[alloctrack]  #{:>2}: Δ {} (live {}, {} blocks, {} allocs total){}{}",
                 t + 1, buf1, buf2,
                 e.live_count.load(std::memory_order_relaxed),
                 e.total_allocs.load(std::memory_order_relaxed),
                 e.tag ? " tag=" : "", e.tag ? e.tag : "");
        const std::uint32_t n_show = e.frame_count < 8 ? e.frame_count : 8;
        for (std::uint32_t f = 0; f < n_show; ++f) {
            symbolize(e.frames[f], sym, sizeof(sym));
            LOG_INFO("[alloctrack]        {}", sym);
        }
    }
    LOG_INFO("[alloctrack] ===== end report =====");
}

// ── Report thread ────────────────────────────────────────────────────────────

std::thread* g_report_thread;
std::mutex g_thread_mutex;
std::condition_variable g_thread_cv;
bool g_thread_stop = false;

} // namespace

void start_reporting(unsigned interval_seconds) {
    std::lock_guard<std::mutex> lock(g_thread_mutex);
    if (g_report_thread)
        return;
    g_thread_stop = false;
    g_report_thread = new std::thread([interval_seconds] {
        std::unique_lock<std::mutex> lock(g_thread_mutex);
        while (!g_thread_cv.wait_for(lock, std::chrono::seconds(interval_seconds),
                                     [] { return g_thread_stop; })) {
            lock.unlock();
            do_dump_report("periodic");
            lock.lock();
        }
    });
    LOG_INFO("[alloctrack] tracking active, reporting every {}s", interval_seconds);
}

void stop_reporting() {
    std::thread* t = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_thread_mutex);
        g_thread_stop = true;
        t = g_report_thread;
        g_report_thread = nullptr;
    }
    g_thread_cv.notify_all();
    if (t) {
        t->join();
        delete t;
        do_dump_report("final");
    }
}

void dump_report(const char* reason) {
    do_dump_report(reason);
}

std::uint64_t live_bytes() {
    const std::int64_t v = g_live_bytes.load(std::memory_order_relaxed);
    return v > 0 ? static_cast<std::uint64_t>(v) : 0;
}

TagScope::TagScope(const char* tag) noexcept : prev_(t_tag) { t_tag = tag; }
TagScope::~TagScope() noexcept { t_tag = prev_; }

} // namespace parties::alloctrack

// ═══════════════════════════════════════════════════════════════════════════
// Global operator new/delete overrides — active from process start.
// ═══════════════════════════════════════════════════════════════════════════

namespace {

void* alloc_or_handler(std::size_t size) {
    for (;;) {
        if (void* p = parties::alloctrack::tracked_alloc(size))
            return p;
        std::new_handler handler = std::get_new_handler();
        if (!handler)
            throw std::bad_alloc();
        handler();
    }
}

void* alloc_aligned_or_handler(std::size_t size, std::align_val_t al) {
    for (;;) {
        if (void* p = parties::alloctrack::tracked_alloc_aligned(size, static_cast<std::size_t>(al)))
            return p;
        std::new_handler handler = std::get_new_handler();
        if (!handler)
            throw std::bad_alloc();
        handler();
    }
}

} // namespace

void* operator new(std::size_t size) { return alloc_or_handler(size); }
void* operator new[](std::size_t size) { return alloc_or_handler(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return parties::alloctrack::tracked_alloc(size);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return parties::alloctrack::tracked_alloc(size);
}
void* operator new(std::size_t size, std::align_val_t al) { return alloc_aligned_or_handler(size, al); }
void* operator new[](std::size_t size, std::align_val_t al) { return alloc_aligned_or_handler(size, al); }
void* operator new(std::size_t size, std::align_val_t al, const std::nothrow_t&) noexcept {
    return parties::alloctrack::tracked_alloc_aligned(size, static_cast<std::size_t>(al));
}
void* operator new[](std::size_t size, std::align_val_t al, const std::nothrow_t&) noexcept {
    return parties::alloctrack::tracked_alloc_aligned(size, static_cast<std::size_t>(al));
}

void operator delete(void* p) noexcept { parties::alloctrack::tracked_free(p); }
void operator delete[](void* p) noexcept { parties::alloctrack::tracked_free(p); }
void operator delete(void* p, std::size_t) noexcept { parties::alloctrack::tracked_free(p); }
void operator delete[](void* p, std::size_t) noexcept { parties::alloctrack::tracked_free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { parties::alloctrack::tracked_free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { parties::alloctrack::tracked_free(p); }
void operator delete(void* p, std::align_val_t) noexcept { parties::alloctrack::tracked_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { parties::alloctrack::tracked_free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { parties::alloctrack::tracked_free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { parties::alloctrack::tracked_free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { parties::alloctrack::tracked_free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { parties::alloctrack::tracked_free(p); }

#endif // PARTIES_ALLOC_TRACKER
