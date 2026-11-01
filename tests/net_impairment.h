#pragma once

// Network impairment relay for end-to-end tests over real MsQuic.
//
// A client connects to the relay's front port instead of the server; the
// relay forwards every UDP datagram to the server and back, applying per
// direction:
//   * random loss (independent per packet),
//   * bursty loss (Gilbert-Elliott: a "bad" state with its own loss rate that
//     is entered and left with fixed probabilities per packet),
//   * one-way delay plus uniform jitter (jitter reorders packets),
//   * a bandwidth cap: a token bucket draining a drop-tail queue.
// QUIC retransmits what is lost, so this exercises exactly what users see on
// bad Wi-Fi, mobile links and saturated uplinks: late, out-of-order frames,
// aborted streams and collapsing throughput.
//
// Deterministic: every direction has its own seeded PRNG.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <queue>
#include <random>
#include <thread>
#include <vector>

namespace parties::test {

struct LinkImpairment {
    double   loss          = 0.0;    // random loss probability per packet, 0..1
    // Gilbert-Elliott burst loss (0 = off): per-packet probability to enter
    // the bad state, to leave it, and the loss probability while in it.
    double   burst_enter   = 0.0;
    double   burst_leave   = 0.0;
    double   burst_loss    = 0.0;
    uint32_t delay_ms      = 0;      // one-way base delay
    uint32_t jitter_ms     = 0;      // extra uniform delay 0..jitter (reorders)
    double   rate_bps      = 0.0;    // bandwidth cap in bits/s (0 = unlimited)
    size_t   queue_bytes   = 256 * 1024;   // drop-tail queue in front of the cap
};

class ImpairedRelay {
public:
    struct DirectionStats {
        uint64_t forwarded = 0;
        uint64_t random_drops = 0;
        uint64_t burst_drops = 0;
        uint64_t queue_drops = 0;
    };

    ImpairedRelay(uint16_t front_port, uint16_t server_port, uint32_t seed = 1)
        : front_port_(front_port), server_port_(server_port),
          up_(seed * 2 + 1), down_(seed * 2 + 2) {}
    ~ImpairedRelay() { stop(); }

    bool start() {
        front_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        back_  = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (front_ == bad_sock() || back_ == bad_sock()) return false;
        // Large socket buffers: the relay itself must never be the bottleneck.
        int big = 8 * 1024 * 1024;
        for (sock_t s : {front_, back_}) {
            setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&big), sizeof(big));
            setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&big), sizeof(big));
        }
        const sockaddr_in front_addr = loopback(front_port_);
        const sockaddr_in back_addr  = loopback(0);
        if (bind(front_, reinterpret_cast<const sockaddr*>(&front_addr), sizeof(front_addr)) != 0 ||
            bind(back_, reinterpret_cast<const sockaddr*>(&back_addr), sizeof(back_addr)) != 0)
            return false;
        running_ = true;
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        if (front_ != bad_sock()) { close_sock(front_); front_ = bad_sock(); }
        if (back_ != bad_sock())  { close_sock(back_);  back_  = bad_sock(); }
    }

    // Client -> server.
    void set_uplink(const LinkImpairment& l)   { std::lock_guard<std::mutex> g(cfg_mutex_); up_.cfg = l; }
    // Server -> client.
    void set_downlink(const LinkImpairment& l) { std::lock_guard<std::mutex> g(cfg_mutex_); down_.cfg = l; }
    void set_both(const LinkImpairment& l)     { set_uplink(l); set_downlink(l); }
    void clear()                               { set_both(LinkImpairment{}); }

    DirectionStats uplink_stats() const   { std::lock_guard<std::mutex> g(cfg_mutex_); return up_.stats; }
    DirectionStats downlink_stats() const { std::lock_guard<std::mutex> g(cfg_mutex_); return down_.stats; }

private:
#ifdef _WIN32
    using sock_t = SOCKET;
    static sock_t bad_sock() { return INVALID_SOCKET; }
    static void close_sock(sock_t s) { closesocket(s); }
#else
    using sock_t = int;
    static sock_t bad_sock() { return -1; }
    static void close_sock(sock_t s) { ::close(s); }
#endif

    static int64_t now_us() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    static sockaddr_in loopback(uint16_t port) {
        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_port        = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return a;
    }

    struct Pending {
        int64_t release_us;
        uint64_t order;              // FIFO among equal release times
        std::vector<uint8_t> data;
        bool operator>(const Pending& o) const {
            return release_us != o.release_us ? release_us > o.release_us : order > o.order;
        }
    };

    struct Direction {
        explicit Direction(uint32_t seed) : rng(seed) {}
        LinkImpairment cfg;
        DirectionStats stats;
        std::mt19937_64 rng;
        bool bad_state = false;
        // Bandwidth cap: drop-tail queue drained by a token bucket.
        std::deque<std::vector<uint8_t>> rate_queue;
        size_t  rate_queued = 0;
        double  tokens = 0.0;
        int64_t tokens_at = 0;
        // Delay line (after the cap).
        std::priority_queue<Pending, std::vector<Pending>, std::greater<Pending>> delay;
        uint64_t order = 0;

        double uniform() { return std::uniform_real_distribution<double>(0.0, 1.0)(rng); }

        // A packet entered this direction.
        void ingest(const uint8_t* data, size_t len, int64_t now) {
            if (cfg.burst_enter > 0.0) {
                if (bad_state ? uniform() < cfg.burst_leave : uniform() < cfg.burst_enter)
                    bad_state = !bad_state;
                if (bad_state && uniform() < cfg.burst_loss) { ++stats.burst_drops; return; }
            }
            if (cfg.loss > 0.0 && uniform() < cfg.loss) { ++stats.random_drops; return; }
            if (cfg.rate_bps > 0.0) {
                if (rate_queued + len > cfg.queue_bytes) { ++stats.queue_drops; return; }
                rate_queue.emplace_back(data, data + len);
                rate_queued += len;
                return;
            }
            schedule(std::vector<uint8_t>(data, data + len), now);
        }

        void schedule(std::vector<uint8_t>&& packet, int64_t now) {
            int64_t release = now + static_cast<int64_t>(cfg.delay_ms) * 1000;
            if (cfg.jitter_ms > 0)
                release += static_cast<int64_t>(uniform() * cfg.jitter_ms * 1000.0);
            delay.push(Pending{release, order++, std::move(packet)});
        }

        template <typename Send>
        void pump(int64_t now, Send&& send) {
            if (cfg.rate_bps > 0.0) {
                const double burst = (std::max)(3000.0, cfg.rate_bps / 8.0 * 0.005);   // 5 ms
                if (tokens_at == 0) tokens_at = now;
                tokens = (std::min)(burst, tokens + static_cast<double>(now - tokens_at) * cfg.rate_bps / 8e6);
                tokens_at = now;
                while (!rate_queue.empty() && tokens >= static_cast<double>(rate_queue.front().size())) {
                    tokens -= static_cast<double>(rate_queue.front().size());
                    rate_queued -= rate_queue.front().size();
                    schedule(std::move(rate_queue.front()), now);
                    rate_queue.pop_front();
                }
            } else {
                // Cap removed while packets were queued: release them all.
                while (!rate_queue.empty()) {
                    rate_queued -= rate_queue.front().size();
                    schedule(std::move(rate_queue.front()), now);
                    rate_queue.pop_front();
                }
                tokens_at = 0;
            }
            while (!delay.empty() && delay.top().release_us <= now) {
                const Pending& p = delay.top();
                send(p.data.data(), p.data.size());
                ++stats.forwarded;
                delay.pop();
            }
        }
    };

    void run() {
        const sockaddr_in server = loopback(server_port_);
        sockaddr_in client{};
        bool have_client = false;
        std::vector<uint8_t> buf(65536);

        auto to_client = [&](const uint8_t* data, size_t len) {
            sendto(front_, reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
                   reinterpret_cast<const sockaddr*>(&client), sizeof(client));
        };
        auto to_server = [&](const uint8_t* data, size_t len) {
            sendto(back_, reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
                   reinterpret_cast<const sockaddr*>(&server), sizeof(server));
        };

        while (running_) {
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(front_, &rd);
            FD_SET(back_, &rd);
            timeval tv{0, 500};
            const int nfds = static_cast<int>((std::max)(front_, back_)) + 1;   // ignored on Windows
            const int ready = select(nfds, &rd, nullptr, nullptr, &tv);
            const int64_t t = now_us();

            std::lock_guard<std::mutex> g(cfg_mutex_);
            if (ready > 0 && FD_ISSET(front_, &rd)) {
                for (;;) {
                    sockaddr_in from{};
                    socklen_t from_len = sizeof(from);
                    const int n = recvfrom(front_, reinterpret_cast<char*>(buf.data()),
                                           static_cast<int>(buf.size()), 0,
                                           reinterpret_cast<sockaddr*>(&from), &from_len);
                    if (n <= 0) break;
                    client = from;
                    have_client = true;
                    up_.ingest(buf.data(), static_cast<size_t>(n), t);
                    if (!readable(front_)) break;
                }
            }
            if (ready > 0 && FD_ISSET(back_, &rd)) {
                for (;;) {
                    const int n = recvfrom(back_, reinterpret_cast<char*>(buf.data()),
                                           static_cast<int>(buf.size()), 0, nullptr, nullptr);
                    if (n <= 0) break;
                    if (have_client) down_.ingest(buf.data(), static_cast<size_t>(n), t);
                    if (!readable(back_)) break;
                }
            }
            up_.pump(t, to_server);
            if (have_client) down_.pump(t, to_client);
        }
    }

    static bool readable(sock_t s) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        timeval tv{0, 0};
        return select(static_cast<int>(s) + 1, &rd, nullptr, nullptr, &tv) > 0;
    }

    uint16_t front_port_;
    uint16_t server_port_;
    sock_t front_ = bad_sock();
    sock_t back_  = bad_sock();
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex cfg_mutex_;
    Direction up_;
    Direction down_;
};

} // namespace parties::test
