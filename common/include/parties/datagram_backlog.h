#pragma once

// Bounds how long real-time datagrams (voice, VOICE2, screen-share audio) can
// wait inside MsQuic's datagram send queue. Shared by the client transport and
// the server's per-session fan-out. No transport dependencies.
//
// Why: MsQuic keeps every datagram handed to DatagramSend until it can be
// written into a packet. There is no expiry and no length limit, and queued
// datagrams are framed together with the connection-level control frames,
// i.e. ahead of ALL stream data. When a path's congestion window collapses
// below the offered datagram rate (heavy loss, bufferbloat, a rebound NAT
// waiting for path validation) the queue therefore grows without bound: voice
// arrives later and later, and every reliable stream (the control stream with
// mute/deafen updates and keepalive pongs, video, file transfers) starves
// behind it. ACKs bypass congestion control, so the connection itself never
// times out and nothing recovers on its own.
//
// The sender asks admit() before each media datagram and drops it while the
// queue is too long or its head has waited too long. To the receiver a drop
// looks like ordinary packet loss (jitter buffer FEC/PLC) and end-to-end
// latency stays bounded. Bounding the queue alone does not unblock the
// streams: while the offered media rate exceeds what the path carries, every
// datagram admitted into an empty queue again goes out first. So once a drop
// episode has drained the queue, media is held off for a little longer and the
// stream data that waited behind it gets the path.
//
// Accounting covers EVERY datagram on the connection, media or not:
//   on_queued()      before DatagramSend (MsQuic may report SENT before the
//                    call returns); on_send_failed() if the call failed;
//   on_dequeued()    on the first DATAGRAM_SEND_STATE_CHANGED that takes the
//                    datagram out of the queue: SENT (written into a packet;
//                    pass how long it waited) or a final state that was never
//                    preceded by SENT (CANCELED; pass a negative wait).
//
// Thread-safety: lock-free, every member may be called from any thread. A race
// between the sender and the MsQuic worker only makes the estimate briefly
// approximate (at worst one extra drop or admission).
//
// Implementation: header-only. Tests: tests/datagram_backlog_test.cpp

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace parties {

struct DatagramBacklogLimits {
    int32_t max_queued   = 32;        // datagrams handed to MsQuic, not yet written
    int64_t max_delay_us = 200'000;   // estimated wait of the oldest queued datagram
    int64_t holdoff_us   = 100'000;   // media refused after a drop episode drained the queue
};

class DatagramSendBacklog {
public:
    DatagramSendBacklog() = default;
    explicit DatagramSendBacklog(DatagramBacklogLimits limits) : limits_(limits) {}

    DatagramSendBacklog(const DatagramSendBacklog&) = delete;
    DatagramSendBacklog& operator=(const DatagramSendBacklog&) = delete;

    // True when one more media datagram may be queued at `now_us`. A refusal
    // is counted in dropped().
    bool admit(int64_t now_us) {
        if (now_us < holdoff_until_us_.load(std::memory_order_relaxed))
            return refuse();
        const int32_t queued = queued_.load(std::memory_order_relaxed);
        if (queued <= 0) {
            // First chance after a drop episode emptied the queue: give the
            // stream data that waited behind it the path before media
            // competes for it again.
            if (dropping_.exchange(false, std::memory_order_relaxed) && limits_.holdoff_us > 0) {
                holdoff_until_us_.store(now_us + limits_.holdoff_us, std::memory_order_relaxed);
                return refuse();
            }
            return true;
        }
        if (queued < limits_.max_queued && head_wait_us(now_us) < limits_.max_delay_us) {
            // Recovered without draining (e.g. a burst hit the count cap):
            // the path kept up, no holdoff needed.
            if (dropping_.load(std::memory_order_relaxed))
                dropping_.store(false, std::memory_order_relaxed);
            return true;
        }
        dropping_.store(true, std::memory_order_relaxed);
        return refuse();
    }

    void on_queued(int64_t now_us) {
        // Entering an empty queue makes this datagram the head: its wait
        // starts now and the previous busy period's wait no longer applies.
        if (queued_.fetch_add(1, std::memory_order_relaxed) <= 0) {
            last_progress_us_.store(now_us, std::memory_order_relaxed);
            last_wait_us_.store(0, std::memory_order_relaxed);
        }
    }

    // DatagramSend failed synchronously: nothing was queued.
    void on_send_failed() { leave(); }

    // A datagram left the queue at `now_us` after waiting `waited_us` (SENT),
    // or without being sent (waited_us < 0, e.g. CANCELED).
    void on_dequeued(int64_t now_us, int64_t waited_us) {
        last_progress_us_.store(now_us, std::memory_order_relaxed);
        if (waited_us >= 0) last_wait_us_.store(waited_us, std::memory_order_relaxed);
        leave();
    }

    // Connection gone: MsQuic has indicated (or will never indicate) every
    // datagram it held. Keeps dropped(), which is cumulative.
    void reset() {
        queued_.store(0, std::memory_order_relaxed);
        last_progress_us_.store(0, std::memory_order_relaxed);
        last_wait_us_.store(0, std::memory_order_relaxed);
        holdoff_until_us_.store(0, std::memory_order_relaxed);
        dropping_.store(false, std::memory_order_relaxed);
    }

    int32_t queued() const { return queued_.load(std::memory_order_relaxed); }

    // Lower bound on how long the oldest queued datagram has been waiting: the
    // wait of the last one written, or the time since the queue last made
    // progress (a stalled queue reports no SENT at all). 0 when empty.
    int64_t head_wait_us(int64_t now_us) const {
        if (queued_.load(std::memory_order_relaxed) <= 0) return 0;
        const int64_t stalled = now_us - last_progress_us_.load(std::memory_order_relaxed);
        return (std::max)(last_wait_us_.load(std::memory_order_relaxed), stalled);
    }

    // Media datagrams refused by admit() since construction.
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

    const DatagramBacklogLimits& limits() const { return limits_; }

private:
    bool refuse() {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    void leave() {
        // The queue drained: whatever congestion made it wait is over.
        if (queued_.fetch_sub(1, std::memory_order_relaxed) <= 1)
            last_wait_us_.store(0, std::memory_order_relaxed);
    }

    const DatagramBacklogLimits limits_{};
    std::atomic<int32_t>  queued_{0};
    std::atomic<int64_t>  last_progress_us_{0};   // last dequeue, or entry into an empty queue
    std::atomic<int64_t>  last_wait_us_{0};       // wait of the last datagram written
    std::atomic<int64_t>  holdoff_until_us_{0};   // media refused until then
    std::atomic<bool>     dropping_{false};       // refused since the queue last drained
    std::atomic<uint64_t> dropped_{0};
};

} // namespace parties
