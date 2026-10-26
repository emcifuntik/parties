// Offline, deterministic tests for the header-only real-time datagram backlog
// gate in parties/datagram_backlog.h.

#include <parties/datagram_backlog.h>

#include <cstdint>
#include <iostream>

using parties::DatagramBacklogLimits;
using parties::DatagramSendBacklog;

namespace {

constexpr int64_t T0 = 5'000'000;
constexpr int64_t MS = 1000;
constexpr DatagramBacklogLimits LIMITS{
    .max_queued = 8, .max_delay_us = 200 * MS, .holdoff_us = 100 * MS };

bool fail(const char* msg) {
    std::cerr << msg << "\n";
    return false;
}

// An idle connection admits everything and reports nothing queued.
bool test_empty_admits() {
    DatagramSendBacklog b(LIMITS);
    if (!b.admit(T0)) return fail("empty: must admit");
    if (b.queued() != 0 || b.head_wait_us(T0) != 0)
        return fail("empty: nothing queued, no wait");
    if (b.dropped() != 0) return fail("empty: no drops");
    return true;
}

// Healthy flow: each datagram leaves the queue within a millisecond.
bool test_healthy_flow_never_drops() {
    DatagramSendBacklog b(LIMITS);
    for (int i = 0; i < 1000; ++i) {
        const int64_t t = T0 + i * 20 * MS;
        if (!b.admit(t)) return fail("healthy: a fresh datagram must be admitted");
        b.on_queued(t);
        b.on_dequeued(t + MS, MS);
    }
    if (b.queued() != 0) return fail("healthy: queue must be empty");
    if (b.dropped() != 0) return fail("healthy: no drops");
    return true;
}

// A short wait for the congestion window (a few ms behind video) is normal and
// must not cost voice.
bool test_short_wait_admits() {
    DatagramSendBacklog b(LIMITS);
    b.on_queued(T0);
    b.on_queued(T0 + 5 * MS);
    b.on_dequeued(T0 + 30 * MS, 30 * MS);        // one written after 30 ms, one still queued
    if (!b.admit(T0 + 40 * MS)) return fail("short wait: must admit");
    return true;
}

// Stalled path (outage, unvalidated path): no SENT at all. Admission stops once
// the head has waited max_delay, which bounds the stale burst after recovery.
bool test_stall_stops_admission() {
    DatagramSendBacklog b(LIMITS);
    b.on_queued(T0);
    if (!b.admit(T0 + 100 * MS)) return fail("stall: 100 ms must still admit");
    b.on_queued(T0 + 100 * MS);
    if (b.head_wait_us(T0 + 199 * MS) != 199 * MS)
        return fail("stall: head wait counts from the queue's last progress");
    if (b.admit(T0 + 200 * MS)) return fail("stall: 200 ms without progress must drop");
    if (b.dropped() != 1) return fail("stall: the refusal must be counted");

    // Recovery: the backlog drains, the stream data that waited behind it gets
    // a holdoff's worth of the path, then media flows again.
    b.on_dequeued(T0 + 900 * MS, 900 * MS);
    if (b.admit(T0 + 901 * MS)) return fail("stall: a 900 ms-old datagram still queued must keep dropping");
    b.on_dequeued(T0 + 901 * MS, 801 * MS);
    if (b.queued() != 0) return fail("stall: queue must be drained");
    if (b.admit(T0 + 902 * MS)) return fail("stall: the first chance after draining must start the holdoff");
    if (b.admit(T0 + 1001 * MS)) return fail("stall: media must hold off for holdoff_us");
    if (!b.admit(T0 + 1002 * MS)) return fail("stall: media resumes after the holdoff");
    b.on_queued(T0 + 1002 * MS);
    if (!b.admit(T0 + 1003 * MS))
        return fail("stall: the previous busy period's wait must not leak into a new one");
    return true;
}

// Standing queue on a slow path: datagrams keep leaving (recent progress) but
// each one waited too long. Admission stops until the queue drains.
bool test_standing_queue_drops() {
    DatagramSendBacklog b(LIMITS);
    for (int i = 0; i < 4; ++i) b.on_queued(T0 + i * 20 * MS);
    b.on_dequeued(T0 + 260 * MS, 260 * MS);
    if (b.admit(T0 + 261 * MS)) return fail("standing: a 260 ms queueing delay must drop");
    b.on_dequeued(T0 + 262 * MS, 242 * MS);
    b.on_dequeued(T0 + 263 * MS, 223 * MS);
    b.on_dequeued(T0 + 264 * MS, 204 * MS);
    if (b.admit(T0 + 265 * MS)) return fail("standing: draining after drops must start the holdoff");
    if (!b.admit(T0 + 365 * MS)) return fail("standing: media resumes after the holdoff");
    return true;
}

// Without a holdoff a drained queue admits at once.
bool test_no_holdoff() {
    DatagramSendBacklog b(DatagramBacklogLimits{
        .max_queued = 8, .max_delay_us = 200 * MS, .holdoff_us = 0 });
    b.on_queued(T0);
    if (b.admit(T0 + 250 * MS)) return fail("no holdoff: a stale queue must still drop");
    b.on_dequeued(T0 + 260 * MS, 260 * MS);
    if (!b.admit(T0 + 261 * MS)) return fail("no holdoff: a drained queue must admit at once");
    return true;
}

// Count cap: a burst that fills the queue faster than one flush drains it.
bool test_count_cap() {
    DatagramSendBacklog b(LIMITS);
    for (int i = 0; i < LIMITS.max_queued; ++i) {
        if (!b.admit(T0)) return fail("cap: below the cap must admit");
        b.on_queued(T0);
    }
    if (b.admit(T0)) return fail("cap: a full queue must drop even with no wait yet");
    b.on_dequeued(T0 + MS, MS);
    if (!b.admit(T0 + MS)) return fail("cap: one slot freed must admit");
    for (int i = 0; i < LIMITS.max_queued - 1; ++i) b.on_dequeued(T0 + 2 * MS, 2 * MS);
    if (b.queued() != 0) return fail("cap: queue must be drained");
    if (!b.admit(T0 + 3 * MS))
        return fail("cap: a burst the path kept up with must not trigger a holdoff");
    return true;
}

// A failed DatagramSend queued nothing; a cancel leaves without a wait sample.
bool test_failed_and_canceled() {
    DatagramSendBacklog b(LIMITS);
    b.on_queued(T0);
    b.on_send_failed();
    if (b.queued() != 0) return fail("failed: must undo on_queued");
    b.on_queued(T0);
    b.on_queued(T0);
    b.on_dequeued(T0 + 500 * MS, -1);             // canceled: progress, but no wait sample
    if (b.queued() != 1) return fail("canceled: must leave the queue");
    if (b.head_wait_us(T0 + 500 * MS) != 0)
        return fail("canceled: must not report a wait it never measured");
    return true;
}

// A new connection starts with an empty queue; the drop count is cumulative so
// callers can log deltas across reconnects.
bool test_reset() {
    DatagramSendBacklog b(LIMITS);
    b.on_queued(T0);
    b.on_queued(T0);
    (void)b.admit(T0 + 300 * MS);
    const uint64_t drops = b.dropped();
    b.reset();
    if (b.queued() != 0 || b.head_wait_us(T0 + 300 * MS) != 0)
        return fail("reset: queue must be empty");
    if (b.dropped() != drops) return fail("reset: dropped() is cumulative");
    if (!b.admit(T0 + 301 * MS)) return fail("reset: must admit");
    return true;
}

} // namespace

int main() {
    bool ok = true;
    ok &= test_empty_admits();
    ok &= test_healthy_flow_never_drops();
    ok &= test_short_wait_admits();
    ok &= test_stall_stops_admission();
    ok &= test_standing_queue_drops();
    ok &= test_no_holdoff();
    ok &= test_count_cap();
    ok &= test_failed_and_canceled();
    ok &= test_reset();
    if (!ok) return 1;
    std::cout << "datagram backlog tests passed\n";
    return 0;
}
