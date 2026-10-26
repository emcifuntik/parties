// End-to-end check of the real-time datagram backlog gate
// (parties/datagram_backlog.h) through a real Server, MsQuic and NetClient.
//
// Client D reaches the server through a UDP relay that can shape either
// direction to less than the voice offered to it (token bucket behind a
// drop-tail queue: a congested last mile). Client A talks to the server
// directly. Two scenarios:
//   downlink  A talks, the server -> D direction is congested: exercises the
//             server's per-peer gate (Session::dgram_backlog).
//   uplink    D talks, the D -> server direction is congested: exercises the
//             client's gate (NetClient::send_data). This is how a mute/deafen
//             update from a user with a bad uplink used to get stuck behind
//             their own voice.
// In both, D measures its control stream with KEEPALIVE_PING/PONG; the shaped
// direction carries the PONG (downlink) or the PING (uplink).
//
// MsQuic queues datagrams without bound and writes them ahead of all stream
// data. Without the gate the excess voice piles up inside the sending MsQuic
// connection: voice latency keeps growing for as long as the congestion lasts
// and the control stream starves behind it. With the gate the queue stays a
// couple of hundred milliseconds deep, voice latency stays bounded and the
// control messages keep getting through.
//
// Pass criteria per scenario. Generous: measured with the gate, voice p95 is
// 0.6-0.9 s and pongs keep coming; without it, after 10 s of congestion voice
// p95 is 2.1 s (uplink) to 3.6 s (downlink) and still growing, and the last
// ping has gone unanswered for ~7 s:
//   * p95 voice latency over the last 2 s of congestion < 1500 ms
//   * PONGs keep arriving in the second half of the congestion, and the ping
//     outstanding at the end has waited less than 5000 ms

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

#include <parties/crypto.h>
#include <parties/net_common.h>
#include <parties/protocol.h>
#include <parties/quic_common.h>
#include <parties/serialization.h>

#include <server/config.h>
#include <server/server.h>

#include <client/net_client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

using namespace parties;
using namespace parties::protocol;
using namespace parties::client;
using namespace parties::server;

namespace {

constexpr uint16_t SERVER_PORT = 17810;
constexpr uint16_t RELAY_PORT  = 17811;
constexpr int      TIMEOUT_MS  = 10000;

// Congested last mile: 8 KB/s behind half a second of drop-tail queue.
constexpr double LINK_BYTES_PER_SEC = 8000.0;
constexpr double LINK_BURST_BYTES   = 1500.0;
constexpr size_t LINK_QUEUE_BYTES   = 4000;

// Voice: 50 frames/s of 300 bytes, about twice what the link carries.
constexpr int    VOICE_INTERVAL_MS   = 20;
constexpr size_t VOICE_PAYLOAD_BYTES = 300;
constexpr int    CONGESTION_MS       = 10000;
constexpr int    MEASURE_TAIL_MS     = 2000;
constexpr int    PING_INTERVAL_MS    = 250;
constexpr int    SETTLE_MS           = 2000;   // between scenarios

constexpr int64_t MAX_P95_VOICE_LATENCY_US = 1'500'000;
constexpr int64_t MAX_PING_OVERDUE_US      = 5'000'000;
constexpr int     MIN_LATE_PONGS           = 1;   // answered in the second half of the congestion

#define LOG(...) do { std::fprintf(stderr, __VA_ARGS__); std::fflush(stderr); } while (0)
#define CHECK(cond, msg) do {                                   \
    if (!(cond)) { LOG("FAIL: %s (line %d)\n", msg, __LINE__); return false; } \
} while (0)

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

#ifdef _WIN32
using sock_t = SOCKET;
const sock_t BAD_SOCK = INVALID_SOCKET;
void close_sock(sock_t s) { closesocket(s); }
#else
using sock_t = int;
const sock_t BAD_SOCK = -1;
void close_sock(sock_t s) { close(s); }
#endif

sockaddr_in loopback(uint16_t port) {
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_port        = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}

enum class Shaped { None, Downlink, Uplink };

// One direction of the relay: a token bucket draining a drop-tail queue.
struct Shaper {
    std::deque<std::vector<uint8_t>> queue;
    size_t   queued_bytes = 0;
    double   tokens       = LINK_BURST_BYTES;
    int64_t  last_us      = 0;
    uint64_t drops        = 0;

    void push(const uint8_t* data, size_t len) {
        if (queued_bytes + len > LINK_QUEUE_BYTES) { ++drops; return; }
        queue.emplace_back(data, data + len);
        queued_bytes += len;
    }

    template <typename Send>
    void release(int64_t now, Send&& send) {
        if (last_us == 0) last_us = now;
        tokens = (std::min)(LINK_BURST_BYTES,
                            tokens + static_cast<double>(now - last_us) * LINK_BYTES_PER_SEC / 1e6);
        last_us = now;
        while (!queue.empty() && tokens >= static_cast<double>(queue.front().size())) {
            const auto& packet = queue.front();
            send(packet.data(), packet.size());
            tokens -= static_cast<double>(packet.size());
            queued_bytes -= packet.size();
            queue.pop_front();
        }
    }
};

// UDP relay: client <-> RELAY_PORT | ephemeral port <-> server.
class Relay {
public:
    ~Relay() { stop(); }

    bool start() {
        front_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        back_  = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (front_ == BAD_SOCK || back_ == BAD_SOCK) return false;
        const sockaddr_in front_addr = loopback(RELAY_PORT);
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
        if (front_ != BAD_SOCK) { close_sock(front_); front_ = BAD_SOCK; }
        if (back_ != BAD_SOCK)  { close_sock(back_);  back_  = BAD_SOCK; }
    }

    void set_shaping(Shaped s) { shaped_ = s; }
    uint64_t drops() const { return drops_; }

private:
    void run() {
        const sockaddr_in server = loopback(SERVER_PORT);
        sockaddr_in client{};
        bool have_client = false;
        Shaper down, up;
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
            timeval tv{ 0, 1000 };
            const int nfds = static_cast<int>((std::max)(front_, back_)) + 1;   // ignored on Windows
            const int ready = select(nfds, &rd, nullptr, nullptr, &tv);
            const Shaped shaped = shaped_;

            if (ready > 0 && FD_ISSET(front_, &rd)) {
                sockaddr_in from{};
                socklen_t from_len = sizeof(from);
                const int n = recvfrom(front_, reinterpret_cast<char*>(buf.data()),
                                       static_cast<int>(buf.size()), 0,
                                       reinterpret_cast<sockaddr*>(&from), &from_len);
                if (n > 0) {
                    client = from;
                    have_client = true;
                    if (shaped == Shaped::Uplink || !up.queue.empty())
                        up.push(buf.data(), static_cast<size_t>(n));
                    else
                        to_server(buf.data(), static_cast<size_t>(n));
                }
            }
            if (ready > 0 && FD_ISSET(back_, &rd)) {
                const int n = recvfrom(back_, reinterpret_cast<char*>(buf.data()),
                                       static_cast<int>(buf.size()), 0, nullptr, nullptr);
                if (n > 0 && have_client) {
                    if (shaped == Shaped::Downlink || !down.queue.empty())
                        down.push(buf.data(), static_cast<size_t>(n));
                    else
                        to_client(buf.data(), static_cast<size_t>(n));
                }
            }

            const int64_t t = now_us();
            down.release(t, to_client);
            up.release(t, to_server);
            drops_ = down.drops + up.drops;
        }
    }

    sock_t front_ = BAD_SOCK;
    sock_t back_  = BAD_SOCK;
    std::atomic<bool>     running_{false};
    std::atomic<Shaped>   shaped_{Shaped::None};
    std::atomic<uint64_t> drops_{0};
    std::thread thread_;
};

bool wait_for_message(NetClient& client, ControlMessageType type,
                      std::vector<uint8_t>& payload, int timeout_ms = TIMEOUT_MS) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto msg = client.incoming().try_pop(); msg && msg->type == type) {
            payload = std::move(msg->payload);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

bool wait_for_connect(NetClient& client) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(TIMEOUT_MS);
    while (std::chrono::steady_clock::now() < deadline) {
        if (client.is_connected()) return true;
        if (client.connect_failed()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// Connects, authenticates and joins channel 1; returns the user id (0 on failure).
uint32_t connect_and_join(NetClient& client, uint16_t port, const char* mnemonic, const char* name) {
    SecretKey sk{};
    PublicKey pk{};
    if (!derive_keypair(mnemonic, sk, pk)) return 0;
    if (!client.connect("127.0.0.1", port) || !wait_for_connect(client)) return 0;

    const auto now = static_cast<uint64_t>(std::time(nullptr));
    BinaryWriter sign_buf;
    sign_buf.write_bytes(pk.data(), pk.size());
    sign_buf.write_string(name);
    sign_buf.write_u64(now);
    Signature sig{};
    if (!ed25519_sign(sign_buf.data().data(), sign_buf.data().size(), sk, pk, sig)) return 0;

    BinaryWriter auth;
    auth.write_u16(PROTOCOL_VERSION);
    auth.write_bytes(pk.data(), pk.size());
    auth.write_string(name);
    auth.write_u64(now);
    auth.write_bytes(sig.data(), sig.size());
    auth.write_string("");
    std::vector<uint8_t> payload;
    if (!client.send_message(ControlMessageType::AUTH_IDENTITY, auth.data().data(), auth.data().size()) ||
        !wait_for_message(client, ControlMessageType::AUTH_RESPONSE, payload))
        return 0;
    BinaryReader r(payload.data(), payload.size());
    const uint32_t user_id = r.read_u32();

    BinaryWriter join;
    join.write_u32(1);
    join.write_u8(0);
    join.write_u8(0);
    if (!client.send_message(ControlMessageType::CHANNEL_JOIN, join.data().data(), join.data().size()) ||
        !wait_for_message(client, ControlMessageType::CHANNEL_USER_LIST, payload))
        return 0;
    return user_id;
}

int64_t percentile(std::vector<int64_t> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[static_cast<size_t>(p * static_cast<double>(v.size() - 1))];
}

// Voice latency samples recorded by whichever client receives the talker.
struct VoiceProbe {
    std::mutex mutex;
    struct Sample { int64_t at_us; int64_t latency_us; };
    std::vector<Sample> samples;
    std::atomic<uint32_t> talker{0};

    void on_data(const uint8_t* data, size_t len) {
        // [VOICE][sender(4)][seq(2)][sent_us(8)][padding]
        if (len < 1 + 4 + 2 + 8 || data[0] != VOICE_PACKET_TYPE) return;
        uint32_t sender = 0;
        std::memcpy(&sender, data + 1, 4);
        if (sender != talker.load()) return;
        int64_t sent_us = 0;
        std::memcpy(&sent_us, data + 7, 8);
        const int64_t t = now_us();
        std::lock_guard<std::mutex> lock(mutex);
        samples.push_back({ t, t - sent_us });
    }
};

// Congests one direction for CONGESTION_MS while `talker` sends voice and the
// relayed client pings over its control stream.
bool run_scenario(const char* name, Shaped shaped, Relay& relay,
                  NetClient& talker, uint32_t talker_id,
                  NetClient& relayed, VoiceProbe& probe) {
    LOG("[%s] congesting for %d ms...\n", name, CONGESTION_MS);
    {
        std::lock_guard<std::mutex> lock(probe.mutex);
        probe.samples.clear();
    }
    probe.talker = talker_id;
    while (relayed.incoming().try_pop()) {}

    relay.set_shaping(shaped);
    const uint64_t drops_before = relay.drops();
    const int64_t start = now_us();
    const int64_t end   = start + static_cast<int64_t>(CONGESTION_MS) * 1000;

    std::atomic<bool> talking{true};
    std::thread voice([&] {
        std::vector<uint8_t> pkt(1 + 2 + VOICE_PAYLOAD_BYTES, 0);
        pkt[0] = VOICE_PACKET_TYPE;
        uint16_t seq = 0;
        auto next = std::chrono::steady_clock::now();
        while (talking) {
            std::memcpy(pkt.data() + 1, &seq, 2);
            const int64_t t = now_us();
            std::memcpy(pkt.data() + 3, &t, 8);
            talker.send_data(pkt.data(), pkt.size(), /*priority=*/true);
            ++seq;
            next += std::chrono::milliseconds(VOICE_INTERVAL_MS);
            std::this_thread::sleep_until(next);
        }
    });

    // One ping outstanding at a time, as the app does.
    const int64_t second_half = start + static_cast<int64_t>(CONGESTION_MS) * 500;
    std::vector<int64_t> pong_rtts;
    int late_pongs = 0;
    int64_t ping_sent = 0;
    int64_t next_ping = start;
    while (now_us() < end) {
        const int64_t t = now_us();
        if (ping_sent == 0 && t >= next_ping) {
            if (relayed.send_message(ControlMessageType::KEEPALIVE_PING, nullptr, 0))
                ping_sent = t;
            next_ping = t + PING_INTERVAL_MS * 1000;
        }
        while (auto msg = relayed.incoming().try_pop()) {
            if (msg->type == ControlMessageType::KEEPALIVE_PONG && ping_sent != 0) {
                const int64_t arrived = now_us();
                pong_rtts.push_back(arrived - ping_sent);
                if (arrived >= second_half) ++late_pongs;
                ping_sent = 0;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const int64_t overdue_ping_us = ping_sent != 0 ? now_us() - ping_sent : 0;

    talking = false;
    voice.join();
    relay.set_shaping(Shaped::None);

    std::vector<int64_t> tail;
    size_t delivered = 0;
    {
        std::lock_guard<std::mutex> lock(probe.mutex);
        for (const auto& s : probe.samples) {
            if (s.at_us < start || s.at_us > end) continue;
            ++delivered;
            if (s.at_us >= end - static_cast<int64_t>(MEASURE_TAIL_MS) * 1000)
                tail.push_back(s.latency_us);
        }
    }
    const int64_t p50 = percentile(tail, 0.50);
    const int64_t p95 = percentile(tail, 0.95);
    const int64_t max_pong = pong_rtts.empty() ? 0
        : *std::max_element(pong_rtts.begin(), pong_rtts.end());

    LOG("[%s] voice frames sent ~%d, delivered %zu; last %d ms: %zu frames, "
        "latency p50 %lld ms, p95 %lld ms\n",
        name, CONGESTION_MS / VOICE_INTERVAL_MS, delivered, MEASURE_TAIL_MS, tail.size(),
        static_cast<long long>(p50 / 1000), static_cast<long long>(p95 / 1000));
    LOG("[%s] pongs answered %zu (%d in the second half), slowest %lld ms, "
        "unanswered ping overdue %lld ms; relay drop-tail drops %llu\n",
        name, pong_rtts.size(), late_pongs, static_cast<long long>(max_pong / 1000),
        static_cast<long long>(overdue_ping_us / 1000),
        static_cast<unsigned long long>(relay.drops() - drops_before));

    CHECK(!tail.empty(), "voice keeps getting through during congestion");
    CHECK(p95 < MAX_P95_VOICE_LATENCY_US, "voice latency stays bounded under congestion");
    CHECK(late_pongs >= MIN_LATE_PONGS,
          "the control stream keeps flowing for the whole congestion");
    CHECK(overdue_ping_us < MAX_PING_OVERDUE_US,
          "control messages are not starved behind queued voice");
    return true;
}

bool run_test(Relay& relay) {
    NetClient direct;    // A
    NetClient relayed;   // D, behind the relay
    VoiceProbe at_relayed, at_direct;
    relayed.on_data_received = [&](const uint8_t* d, size_t n) { at_relayed.on_data(d, n); };
    direct.on_data_received  = [&](const uint8_t* d, size_t n) { at_direct.on_data(d, n); };

    const uint32_t direct_id = connect_and_join(direct, SERVER_PORT,
        "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about",
        "direct");
    CHECK(direct_id != 0, "direct client connects, authenticates and joins");
    const uint32_t relayed_id = connect_and_join(relayed, RELAY_PORT,
        "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo wrong", "relayed");
    CHECK(relayed_id != 0, "relayed client connects, authenticates and joins");
    LOG("[setup] direct=%u relayed=%u joined channel 1\n", direct_id, relayed_id);

    // Both scenarios always run so a failure report shows each direction.
    const bool downlink_ok =
        run_scenario("downlink", Shaped::Downlink, relay, direct, direct_id, relayed, at_relayed);
    std::this_thread::sleep_for(std::chrono::milliseconds(SETTLE_MS));   // let both directions drain
    const bool uplink_ok =
        run_scenario("uplink", Shaped::Uplink, relay, relayed, relayed_id, relayed, at_direct);
    const bool ok = downlink_ok && uplink_ok;

    direct.disconnect();
    relayed.disconnect();
    return ok;
}

} // namespace

int main() {
    LOG("=== Parties voice congestion test ===\n");
    if (!crypto_init() || !net_init() || quic_init() == nullptr) {
        LOG("FAIL: subsystem init\n");
        return 1;
    }

    const auto tmp = fs::temp_directory_path() / "parties_voice_congestion_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    Config cfg;
    cfg.listen_ip   = "127.0.0.1";
    cfg.port        = SERVER_PORT;
    cfg.cert_file   = (tmp / "server.der").string();
    cfg.key_file    = (tmp / "server.key.der").string();
    cfg.db_path     = (tmp / "test.db").string();
    cfg.max_clients = 8;

    Server server;
    bool ok = server.start(cfg);
    if (!ok) LOG("FAIL: server start\n");
    std::thread server_thread;
    if (ok) {
        server_thread = std::thread([&] { server.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    Relay relay;
    if (ok && !relay.start()) {
        LOG("FAIL: relay start\n");
        ok = false;
    }
    if (ok) ok = run_test(relay);

    relay.stop();
    server.stop();
    if (server_thread.joinable()) server_thread.join();
    quic_cleanup();
    net_cleanup();
    crypto_cleanup();
    std::error_code ec;
    fs::remove_all(tmp, ec);

    LOG(ok ? "=== PASSED ===\n" : "=== FAILED ===\n");
    return ok ? 0 : 1;
}
