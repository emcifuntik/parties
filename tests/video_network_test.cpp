// End-to-end screen-share test over real MsQuic, a real Server and an
// impaired network: what a viewer actually sees, frame by frame.
//
//   sharer --[relay S: uplink impairment]--> server --[relay V: downlink impairment]--> viewer V
//                                                   \-------------------------------> viewer D (direct)
//
// The sharer runs the production send path: VideoSendController (admission +
// bitrate adaptation, fed with the connection's statistics), per-frame QUIC
// streams and keyframes on request (PLI, 1 s cooldown, plus the long
// VIDEO_KEYFRAME_INTERVAL_MS safety interval, as the real encoders). Its
// "encoder" is a CBR model: every frame is bitrate / fps bytes, a keyframe 3
// frames' worth. Viewers run the production receive path (VideoReceiveRouter:
// reorder buffers + paced keyframe requests) into a decoder model that needs
// a keyframe to start and every delta in order.
//
// Per scenario, after a warm-up, we measure at viewer V (and D):
//   fps       frames the decoder could use per second
//   freeze    longest gap between two decoded frames
//   jank      decoded frames that arrived more than 100 ms after the previous one
//   bursts    100 ms windows that delivered 4+ frames at once (frames clumped behind a stall)
//   latency   send -> decode, p50 / p95
//   keyframes forced by requests, and the sender's bitrate at the end
// and compare against thresholds that a healthy real-time stream meets.
//
// Usage: parties_video_network_test [scenario-name ...]   (default: all)

#include "net_impairment.h"

#include <parties/crypto.h>
#include <parties/net_common.h>
#include <parties/protocol.h>
#include <parties/quic_common.h>
#include <parties/serialization.h>
#include <parties/video_common.h>
#include <parties/video_receive_router.h>
#include <parties/video_send_controller.h>

#include <server/config.h>
#include <server/server.h>

#include <client/net_client.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

using namespace parties;
using namespace parties::protocol;
using namespace parties::client;
using namespace parties::server;
using parties::test::ImpairedRelay;
using parties::test::LinkImpairment;

namespace {

constexpr uint16_t SERVER_PORT       = 17830;
constexpr uint16_t SHARER_RELAY_PORT = 17831;
constexpr uint16_t VIEWER_RELAY_PORT = 17832;
constexpr int      TIMEOUT_MS        = 10000;
constexpr uint32_t FPS               = 60;
const bool VERBOSE = std::getenv("VNT_DEBUG") != nullptr;   // per-second sender stats + debug logs

#define LOG(...) do { std::fprintf(stderr, __VA_ARGS__); std::fflush(stderr); } while (0)
#define CHECK(cond, msg) do {                                   \
    if (!(cond)) { LOG("FAIL: %s (line %d)\n", msg, __LINE__); return false; } \
} while (0)

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

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

// ── Sharer: production send controller + CBR encoder model ─────────────────

class Sharer {
public:
    explicit Sharer(NetClient& net) : net_(net) {
        net_.on_video_bytes_completed = [this](VideoSource, size_t bytes) { ctl_.on_completed(bytes); };
        net_.on_data_received = [this](const uint8_t* d, size_t n) {
            if (n >= 6 && d[0] == VIDEO_CONTROL_TYPE && d[1] == VIDEO_CTL_PLI)
                pli_pending_.store(true, std::memory_order_relaxed);
        };
    }
    ~Sharer() { stop(); }

    void start(uint32_t target_bps) {
        VideoSendControllerConfig cfg;
        cfg.target_bitrate_bps = target_bps;
        ctl_.reset(cfg);
        bitrate_ = target_bps;
        running_ = true;
        thread_ = std::thread([this] { run(); });
    }
    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        net_.on_video_bytes_completed = nullptr;
        net_.on_data_received = nullptr;
    }

    // Snapshot for the report (approximate, read across threads).
    uint32_t bitrate() const { return bitrate_.load(); }
    uint32_t forced_keyframes() const { return forced_keyframes_.load(); }
    uint32_t frames_sent() const { return frames_sent_.load(); }
    uint32_t admission_skips() const { return admission_skips_.load(); }
    void reset_counters() { forced_keyframes_ = 0; frames_sent_ = 0; admission_skips_ = 0; }

private:
    void run() {
        const int64_t interval = 1'000'000 / FPS;
        int64_t next = now_us();
        int64_t last_stats = 0, last_key = 0, last_forced = 0;
        uint32_t seq = 0;
        std::vector<uint8_t> payload;
        while (running_) {
            const int64_t now = now_us();
            if (now - last_stats >= 50'000) {
                last_stats = now;
                const auto st = net_.connection_stats();
                if (st.valid)
                    ctl_.on_connection_stats(now, st.rtt_us,
                                             st.suspected_lost_packets, st.spurious_lost_packets,
                                             st.sent_packets);
            }
            ctl_.tick(now);
            if (auto b = ctl_.take_bitrate_update()) bitrate_ = std::clamp(*b, VIDEO_MIN_BITRATE, VIDEO_MAX_BITRATE);
            if (VERBOSE && now - last_report_ >= 1'000'000) {
                last_report_ = now;
                const auto s = ctl_.stats();
                const auto st = net_.connection_stats();
                LOG("  sender: bitrate %u kbps goodput %u kbps outstanding %lld B queue %u ms loss %.3f rtt %u/%u us cwnd %u admission drops %u\n",
                    s.bitrate_bps / 1000, s.goodput_bps / 1000, static_cast<long long>(s.outstanding_bytes),
                    s.queue_delay_ms, s.loss_ratio, st.min_rtt_us, st.rtt_us, st.congestion_window,
                    s.admission_drops);
            }

            if (ctl_.admit_frame(now)) {
                bool key = seq == 0 ||
                           now - last_key >= int64_t{VIDEO_KEYFRAME_INTERVAL_MS} * 1000 ||
                           ctl_.take_keyframe_request();
                if (pli_pending_.load(std::memory_order_relaxed) &&
                    now - last_forced >= int64_t{VIDEO_KEYFRAME_REQUEST_COOLDOWN_MS} * 1000) {
                    pli_pending_ = false;
                    last_forced = now;
                    if (!key) ++forced_keyframes_;
                    key = true;
                }
                if (key) last_key = now;
                const size_t frame_bytes = (std::max)(size_t{200}, static_cast<size_t>(bitrate_.load() / 8 / FPS));
                payload.assign(key ? frame_bytes * 3 : frame_bytes, 0x42);
                std::memcpy(payload.data(), &now, sizeof(now));   // send time, for latency

                VideoFrameHeader hdr;
                hdr.frame_seq = seq;
                hdr.timestamp = static_cast<uint32_t>(now / 1000);   // capture time, as AppCore stamps it
                hdr.flags = static_cast<uint8_t>((key ? VIDEO_FLAG_KEYFRAME : 0) | VIDEO_FLAG_CAPTURE_TIME);
                hdr.width = 1920;
                hdr.height = 1080;
                hdr.codec = static_cast<uint8_t>(VideoCodecId::AV1);
                uint8_t h[VIDEO_FRAME_HEADER_SIZE];
                hdr.write(h);
                const size_t accounted = NetClient::frame_stream_prefix_bytes(VideoSource::Screen) +
                                         sizeof(h) + payload.size();
                ctl_.on_enqueued(accounted);
                if (!net_.send_video_frame_stream(h, sizeof(h), payload.data(), payload.size())) {
                    ctl_.on_completed(accounted);
                    ctl_.on_send_failed(accounted);
                }
                ++seq;
                ++frames_sent_;
            } else {
                ++admission_skips_;
            }
            next += interval;
            const int64_t sleep = next - now_us();
            if (sleep > 0) std::this_thread::sleep_for(std::chrono::microseconds(sleep));
            else if (sleep < -interval) next = now_us();
        }
    }

    NetClient& net_;
    VideoSendController ctl_;
    std::atomic<bool> running_{false};
    std::atomic<bool> pli_pending_{false};
    std::atomic<uint32_t> bitrate_{0};
    std::atomic<uint32_t> forced_keyframes_{0};
    std::atomic<uint32_t> frames_sent_{0};
    std::atomic<uint32_t> admission_skips_{0};
    int64_t last_report_ = 0;
    std::thread thread_;
};

// ── Viewer: production receive router + decoder model ───────────────────────

class Viewer {
public:
    Viewer(NetClient& net, const char* name)
        : net_(net), name_(name),
          router_([this](uint32_t stream, const VideoFrameHeader& hdr, std::vector<uint8_t>&& f) {
                      on_deliver(stream, hdr, f);
                  },
                  [this](uint32_t stream) { send_pli(stream); }) {
        net_.on_video_stream_frame = [this](uint32_t s, std::vector<uint8_t>&& f) {
            router_.on_frame(s, std::move(f), now_us());
        };
        net_.on_video_stream_header = [this](uint32_t s, const VideoFrameHeader& h) {
            router_.on_frame_started(s, h, now_us());
        };
        net_.on_video_stream_aborted = [this](uint32_t s, uint32_t seq, bool have) {
            ++aborted_;
            router_.on_frame_aborted(s, seq, have, now_us());
        };
        running_ = true;
        ticker_ = std::thread([this] {
            int64_t last_rtt = 0;
            while (running_) {
                const int64_t now = now_us();
                if (now - last_rtt >= 1'000'000) {
                    last_rtt = now;
                    const auto st = net_.connection_stats();
                    if (st.valid) router_.set_rtt(st.rtt_us);
                }
                router_.tick(now, [this](uint32_t) { return !have_ref_.load(); });
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
    }
    ~Viewer() {
        running_ = false;
        if (ticker_.joinable()) ticker_.join();
        net_.on_video_stream_frame = nullptr;
        net_.on_video_stream_header = nullptr;
        net_.on_video_stream_aborted = nullptr;
    }

    void watch(uint32_t stream) {
        stream_ = stream;
        router_.open(stream, now_us());
        BinaryWriter w;
        w.write_u32(stream);
        w.write_u8(1);
        net_.send_message(ControlMessageType::SCREEN_SHARE_VIEW, w.data().data(), w.data().size());
    }

    struct Report {
        double  fps = 0;
        int64_t freeze_ms = 0;
        int     jank = 0;
        int     bursts = 0;
        int64_t p50_ms = 0, p95_ms = 0;
        size_t  decoded = 0;
        uint64_t requests = 0;   // keyframe requests this viewer sent (whole run)
        uint32_t aborted = 0;    // frame streams the server aborted toward it (whole run)
    };
    // Frames decoded in [from, to).
    Report report(int64_t from, int64_t to) {
        Report r;
        // The router delivers frames under its own lock into on_deliver(),
        // which takes mutex_: never hold mutex_ while calling the router.
        r.requests = router_.stats(stream_).requests_sent;
        r.aborted = aborted_.load();
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<int64_t> lat;
        int64_t prev = from;
        std::vector<int64_t> times;
        for (const auto& d : decoded_) {
            if (d.at < from || d.at >= to) continue;
            times.push_back(d.at);
            lat.push_back(d.latency);
            r.freeze_ms = (std::max)(r.freeze_ms, (d.at - prev) / 1000);
            if (d.at - prev > 100'000 && prev != from) ++r.jank;
            prev = d.at;
        }
        r.freeze_ms = (std::max)(r.freeze_ms, (to - prev) / 1000);
        r.decoded = times.size();
        r.fps = static_cast<double>(times.size()) / (static_cast<double>(to - from) / 1e6);
        for (int64_t w = from; w < to; w += 100'000) {
            const auto n = std::count_if(times.begin(), times.end(),
                                         [&](int64_t t) { return t >= w && t < w + 100'000; });
            if (n >= 4 + FPS / 10) ++r.bursts;   // 6 at 60 fps is on time; 10+ is a clump
        }
        r.p50_ms = percentile(lat, 0.50) / 1000;
        r.p95_ms = percentile(lat, 0.95) / 1000;
        return r;
    }
    const char* name() const { return name_; }

private:
    void on_deliver(uint32_t stream, const VideoFrameHeader& hdr, const std::vector<uint8_t>& f) {
        const int64_t now = now_us();
        bool ok;
        if (hdr.keyframe()) ok = true;
        else ok = have_ref_.load() && hdr.frame_seq == last_seq_ + 1;
        if (!ok) {
            have_ref_ = false;
            router_.request_keyframe(stream, now);
            return;
        }
        have_ref_ = true;
        last_seq_ = hdr.frame_seq;
        int64_t sent = 0;
        if (f.size() >= VIDEO_FRAME_HEADER_SIZE + sizeof(sent))
            std::memcpy(&sent, f.data() + VIDEO_FRAME_HEADER_SIZE, sizeof(sent));
        std::lock_guard<std::mutex> lock(mutex_);
        decoded_.push_back({now, now - sent});
    }

    void send_pli(uint32_t stream) {
        uint8_t pkt[6];
        pkt[0] = VIDEO_CONTROL_TYPE;
        pkt[1] = VIDEO_CTL_PLI;
        std::memcpy(pkt + 2, &stream, 4);
        net_.send_video(pkt, sizeof(pkt), true);
    }

    struct Decoded { int64_t at; int64_t latency; };
    NetClient& net_;
    const char* name_;
    VideoReceiveRouter router_;
    std::atomic<bool> running_{false};
    std::atomic<bool> have_ref_{false};
    std::atomic<uint32_t> aborted_{0};
    uint32_t last_seq_ = 0;   // decode order (deliver runs under the router's lock)
    uint32_t stream_ = 0;
    std::thread ticker_;
    std::mutex mutex_;
    std::vector<Decoded> decoded_;
};

// ── Scenarios ───────────────────────────────────────────────────────────────

struct Expect {
    double  min_fps;
    int64_t max_freeze_ms;
    int64_t max_p95_ms;
    int     max_bursts;           // per measured interval
    uint32_t max_forced_keyframes;
};

struct Scenario {
    const char* name;
    uint32_t target_bps;
    LinkImpairment sharer_up;     // sharer -> server
    LinkImpairment viewer_down;   // server -> viewer V
    LinkImpairment viewer_up;     // viewer V -> server (its PLIs)
    Expect v;                     // viewer V
    Expect d;                     // viewer D (direct link: must stay healthy)
    int    warmup_ms = 3000;
    int    measure_ms = 10000;
};

LinkImpairment impair(double loss, uint32_t delay_ms, uint32_t jitter_ms = 0, double rate_bps = 0) {
    LinkImpairment l;
    l.loss = loss;
    l.delay_ms = delay_ms;
    l.jitter_ms = jitter_ms;
    l.rate_bps = rate_bps;
    if (rate_bps > 0) l.queue_bytes = static_cast<size_t>(rate_bps / 8 * 0.2);   // 200 ms of drop-tail
    return l;
}

LinkImpairment bursty(uint32_t delay_ms) {
    LinkImpairment l;
    l.delay_ms = delay_ms;
    l.burst_enter = 0.01;     // ~1 burst per 100 packets
    l.burst_leave = 0.15;     // ~7 packets long
    l.burst_loss = 0.8;
    l.loss = 0.005;
    return l;
}

std::vector<Scenario> scenarios() {
    // Thresholds: what this build delivers on the reference machine, with
    // margin. Latency includes the playout cushion, which trades up to
    // 300 ms of delay for smooth motion on a jittery path.
    //                 min fps  freeze ms  p95 ms  bursts  forced keyframes
    const Expect healthy{55.0, 250, 300, 2, 1};
    std::vector<Scenario> s;
    s.push_back({"clean", 4'000'000, impair(0, 10), impair(0, 10), impair(0, 10),
                 healthy, healthy});
    // A clean 50 ms RTT path for longer than BBR's 10 s MinRtt expiry: every
    // connection enters ProbeRTT at least once. Stock BBRv1 drains to four
    // packets for 200 ms + a round trip there (a ~250 ms freeze every 10 s per
    // connection, or the playout cushion stretching to hide it); MsQuic patch
    // 0004 keeps the stream flowing. Unpatched: freeze ~120 ms, p95 ~250 ms.
    {
        const Expect steady{55.0, 100, 120, 2, 1};
        s.push_back({"steady-rtt-50ms", 4'000'000, impair(0, 12), impair(0, 12), impair(0, 12),
                     steady, steady, 3000, 22000});
    }
    // Loss on the viewer's own link only: V copes, D (clean path) is untouched
    // apart from the keyframes V's recoveries cost.
    s.push_back({"viewer-loss-10pct", 4'000'000, impair(0, 10), impair(0.10, 30, 3), impair(0.10, 30, 3),
                 {52.0, 400, 500, 4, 8}, {55.0, 250, 300, 2, 8}});
    // Loss on every link, the sharer's uplink included: everyone sees it.
    {
        const Expect e{55.0, 500, 500, 3, 2};
        s.push_back({"loss-2pct", 4'000'000, impair(0.02, 20, 2), impair(0.02, 20, 2), impair(0.02, 20, 2), e, e});
    }
    {
        const Expect e{52.0, 400, 700, 4, 3};
        s.push_back({"loss-10pct", 4'000'000, impair(0.10, 30, 3), impair(0.10, 30, 3), impair(0.10, 30, 3), e, e});
    }
    {
        const Expect e{30.0, 2500, 1200, 14, 10};
        s.push_back({"loss-25pct", 3'000'000, impair(0.25, 40, 4), impair(0.25, 40, 4), impair(0.25, 40, 4), e, e});
    }
    {
        const Expect e{55.0, 500, 500, 3, 2};
        s.push_back({"burst-loss", 4'000'000, bursty(20), bursty(20), bursty(20), e, e});
    }
    {
        // Jitter beyond the 300 ms playout cap still shows as some clumping.
        const Expect e{42.0, 1200, 1500, 16, 5};
        s.push_back({"rtt-300ms-3pct", 4'000'000, impair(0.03, 150, 5), impair(0.03, 150, 5), impair(0.03, 150, 5), e, e});
    }
    // Viewer V's downlink carries half the stream. Without simulcast it can
    // only get the frames its link carries (an occasional keyframe); what
    // matters is that it never delays D and does not force a keyframe for
    // everyone every second (at most one per ~4 s).
    s.push_back({"viewer-collapse", 4'000'000, impair(0, 10), impair(0, 15, 0, 2'000'000), impair(0, 10),
                 {1.5, 5000, 1500, 20, 4}, {55.0, 250, 300, 2, 4}});
    // The sharer's uplink carries less than its target: it adapts down and
    // every viewer gets a steady, lower-rate stream.
    {
        const Expect adapted{50.0, 250, 400, 2, 1};
        s.push_back({"sharer-collapse", 6'000'000, impair(0, 15, 0, 2'500'000), impair(0, 10), impair(0, 10),
                     adapted, adapted, 6000, 10000});
    }
    return s;
}

bool judge(const char* scenario, const char* who, const Viewer::Report& r, const Expect& e,
           uint32_t forced) {
    LOG("[%s] %-6s fps %5.1f  freeze %5lld ms  jank %3d  bursts %3d  latency p50 %4lld p95 %4lld ms  forced keyframes %u  (requests %llu, aborted %u)\n",
        scenario, who, r.fps, static_cast<long long>(r.freeze_ms), r.jank, r.bursts,
        static_cast<long long>(r.p50_ms), static_cast<long long>(r.p95_ms), forced,
        static_cast<unsigned long long>(r.requests), r.aborted);
    bool ok = true;
    if (r.fps < e.min_fps) { LOG("  FAIL: %s fps %.1f < %.1f\n", who, r.fps, e.min_fps); ok = false; }
    if (r.freeze_ms > e.max_freeze_ms) { LOG("  FAIL: %s freeze %lld ms > %lld\n", who, static_cast<long long>(r.freeze_ms), static_cast<long long>(e.max_freeze_ms)); ok = false; }
    if (r.p95_ms > e.max_p95_ms) { LOG("  FAIL: %s latency p95 %lld ms > %lld\n", who, static_cast<long long>(r.p95_ms), static_cast<long long>(e.max_p95_ms)); ok = false; }
    if (r.bursts > e.max_bursts) { LOG("  FAIL: %s %d bursts > %d\n", who, r.bursts, e.max_bursts); ok = false; }
    if (forced > e.max_forced_keyframes) { LOG("  FAIL: %u forced keyframes > %u\n", forced, e.max_forced_keyframes); ok = false; }
    return ok;
}

bool run_scenario(const Scenario& sc) {
    LOG("\n=== %s ===\n", sc.name);
    ImpairedRelay sharer_relay(SHARER_RELAY_PORT, SERVER_PORT, 11);
    ImpairedRelay viewer_relay(VIEWER_RELAY_PORT, SERVER_PORT, 22);
    CHECK(sharer_relay.start() && viewer_relay.start(), "relays start");
    // The path is what it is from the first packet on, as in real life: the
    // transport learns its RTT during the handshake.
    sharer_relay.set_uplink(sc.sharer_up);
    sharer_relay.set_downlink(impair(sc.sharer_up.loss, sc.sharer_up.delay_ms, sc.sharer_up.jitter_ms));
    viewer_relay.set_downlink(sc.viewer_down);
    viewer_relay.set_uplink(sc.viewer_up);

    NetClient sharer_net, viewer_net, direct_net;
    const uint32_t sharer_id = connect_and_join(sharer_net, SHARER_RELAY_PORT,
        "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about", "sharer");
    CHECK(sharer_id != 0, "sharer joins");
    const uint32_t viewer_id = connect_and_join(viewer_net, VIEWER_RELAY_PORT,
        "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo wrong", "viewer");
    CHECK(viewer_id != 0, "viewer joins");
    const uint32_t direct_id = connect_and_join(direct_net, SERVER_PORT,
        "legal winner thank year wave sausage worth useful legal winner thank yellow", "direct");
    CHECK(direct_id != 0, "direct viewer joins");

    BinaryWriter start;
    start.write_u8(static_cast<uint8_t>(VideoCodecId::AV1));
    start.write_u16(1920);
    start.write_u16(1080);
    CHECK(sharer_net.send_message(ControlMessageType::SCREEN_SHARE_START, start.data().data(), start.data().size()),
          "share start");
    std::vector<uint8_t> payload;
    CHECK(wait_for_message(viewer_net, ControlMessageType::SCREEN_SHARE_STARTED, payload), "viewer told");
    CHECK(wait_for_message(direct_net, ControlMessageType::SCREEN_SHARE_STARTED, payload), "direct told");

    Sharer sharer(sharer_net);
    Viewer viewer(viewer_net, "V");
    Viewer direct(direct_net, "D");
    sharer.start(sc.target_bps);
    viewer.watch(sharer_id);
    direct.watch(sharer_id);

    std::this_thread::sleep_for(std::chrono::milliseconds(sc.warmup_ms));
    sharer.reset_counters();
    const int64_t from = now_us();
    std::this_thread::sleep_for(std::chrono::milliseconds(sc.measure_ms));
    const int64_t to = now_us();
    const uint32_t forced = sharer.forced_keyframes();
    const auto rv = viewer.report(from, to);
    const auto rd = direct.report(from, to);
    LOG("[%s] sender: %u frames sent, %u skipped by admission, bitrate now %u kbps\n",
        sc.name, sharer.frames_sent(), sharer.admission_skips(), sharer.bitrate() / 1000);
    const auto up = sharer_relay.uplink_stats();
    const auto down = viewer_relay.downlink_stats();
    LOG("[%s] relay drops: sharer up %llu random / %llu burst / %llu queue; viewer down %llu / %llu / %llu\n",
        sc.name, (unsigned long long)up.random_drops, (unsigned long long)up.burst_drops,
        (unsigned long long)up.queue_drops, (unsigned long long)down.random_drops,
        (unsigned long long)down.burst_drops, (unsigned long long)down.queue_drops);

    bool ok = judge(sc.name, "V", rv, sc.v, forced);
    ok &= judge(sc.name, "D", rd, sc.d, forced);

    sharer.stop();
    sharer_relay.clear();
    viewer_relay.clear();
    sharer_net.disconnect();
    viewer_net.disconnect();
    direct_net.disconnect();
    sharer_relay.stop();
    viewer_relay.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    LOG("=== Parties video network test ===\n");
    spdlog::set_level(VERBOSE ? spdlog::level::debug : spdlog::level::warn);
    if (!crypto_init() || !net_init() || quic_init() == nullptr) {
        LOG("FAIL: subsystem init\n");
        return 1;
    }
    const auto tmp = fs::temp_directory_path() / "parties_video_network_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    Config cfg;
    cfg.listen_ip   = "127.0.0.1";
    cfg.port        = SERVER_PORT;
    cfg.cert_file   = (tmp / "server.der").string();
    cfg.key_file    = (tmp / "server.key.der").string();
    cfg.db_path     = (tmp / "test.db").string();
    cfg.max_clients = 16;

    Server server;
    bool ok = server.start(cfg);
    if (!ok) LOG("FAIL: server start\n");
    std::thread server_thread;
    if (ok) {
        server_thread = std::thread([&] { server.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::vector<std::string> wanted(argv + 1, argv + argc);
    int failed = 0, ran = 0;
    if (ok) {
        for (const auto& sc : scenarios()) {
            if (!wanted.empty() && std::find(wanted.begin(), wanted.end(), sc.name) == wanted.end()) continue;
            ++ran;
            if (!run_scenario(sc)) ++failed;
        }
    }

    server.stop();
    if (server_thread.joinable()) server_thread.join();
    quic_cleanup();
    net_cleanup();
    crypto_cleanup();
    std::error_code ec;
    fs::remove_all(tmp, ec);

    ok = ok && ran > 0 && failed == 0;
    LOG(ok ? "\n=== PASSED (%d scenarios) ===\n" : "\n=== FAILED (%d of %d scenarios) ===\n",
        ok ? ran : failed, ran);
    return ok ? 0 : 1;
}
