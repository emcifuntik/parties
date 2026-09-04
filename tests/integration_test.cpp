// Integration test: Server + 3 headless clients + voice & screen share
//
// Voice flow:
//   1. Start server with fresh DB + 1 channel
//   2. Client A connects, authenticates via Ed25519 identity, joins channel
//   3. Client B connects, authenticates via Ed25519 identity, joins channel
//   4. Client A Opus-encodes PCM silence and sends as voice datagram
//   5. Client B receives the forwarded voice data
//   6. Verify: packet type, sender user_id, and opus payload match
//
// Screen share flow (A and B speak protocol 1.2):
//   7.  Client A sends SCREEN_SHARE_START → both receive SCREEN_SHARE_STARTED
//       (trailing replay flag = 0 for a live start)
//   8.  Client B subscribes via SCREEN_SHARE_VIEW → Client A receives
//       SCREEN_SHARE_VIEWER [user_b][1]
//   9.  Client A sends a synthetic frame on the legacy video stream 1 →
//       Client B (protocol 1.2) receives it on a per-frame unidirectional
//       stream (on_video_stream_frame) + verifies sender and bytes
//   10. Client A sends 5 frames via send_video_frame_stream (frame_seq 10..14,
//       keyframe first) → Client B receives all 5 in order
//   11. Client B unsubscribes → A receives SCREEN_SHARE_VIEWER [user_b][0];
//       B re-subscribes → A receives [user_b][1] (+ the server's auto-PLI)
//   12. Client B sends PLI → Client A receives forwarded PLI (priority datagram)
//
// Legacy peer flow (Client C speaks protocol 1.1 — same major, accepted):
//   13. Client C authenticates with version (1 << 8) | 1 and joins the channel
//       AFTER A started sharing → its SCREEN_SHARE_STARTED for A carries the
//       trailing replay flag = 1 (A's live start in step 7 carried 0)
//   14. Client C subscribes to A with the legacy 4-byte SCREEN_SHARE_VIEW →
//       A receives SCREEN_SHARE_VIEWER [user_c][1]; A sends one keyframe via
//       send_video_frame_stream → C receives it re-originated on the legacy
//       stream 1 as [0x02][sender u32][hdr14][encoded] (sender == A, bytes
//       intact); C's on_video_stream_frame is never invoked
//   15. Client C starts its own share (everyone gets SCREEN_SHARE_STARTED,
//       replay = 0); B subscribes to C → C receives NO SCREEN_SHARE_VIEWER
//       within a bounded 500 ms (suppressed for pre-1.2 sharers); B
//       unsubscribes, C stops → everyone gets SCREEN_SHARE_STOPPED [user_c]
//   16. Client C sends CHANNEL_LEAVE → A receives SCREEN_SHARE_VIEWER
//       [user_c][0] (implicit unsubscribe on leave)
//   17. Client A sends SCREEN_SHARE_STOP → A and B receive SCREEN_SHARE_STOPPED
//
// Every AUTH_RESPONSE must carry the trailing u16 protocol version, which is the
// SERVER's version (== PROTOCOL_VERSION) regardless of what the client reported.

#include <parties/crypto.h>
#include <parties/types.h>
#include <parties/net_common.h>
#include <parties/quic_common.h>
#include <parties/protocol.h>
#include <parties/serialization.h>
#include <parties/codec.h>
#include <parties/audio_common.h>
#include <parties/video_common.h>

#include <server/config.h>
#include <server/server.h>
#include <server/database.h>

#include <client/net_client.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

using namespace parties;
using namespace parties::protocol;
using namespace parties::client;
using namespace parties::server;

static constexpr uint16_t TEST_PORT = 17800;
static constexpr int TIMEOUT_MS = 10000;

// Client C authenticates as a protocol 1.1 peer: same major (accepted by the
// server's major-only check), but no per-frame streams and no
// SCREEN_SHARE_VIEWER notifications.
static constexpr uint16_t LEGACY_PROTOCOL_VERSION = static_cast<uint16_t>((1u << 8) | 1u);
static_assert(LEGACY_PROTOCOL_VERSION == protocol::PROTOCOL_VERSION_ASSUMED_LEGACY,
              "legacy test version must be the one the server assumes for pre-1.2 peers");
static_assert(!protocol::protocol_supports_frame_streams(LEGACY_PROTOCOL_VERSION),
              "a 1.1 peer must not be served per-frame streams");
static_assert(protocol::protocol_supports_frame_streams(protocol::PROTOCOL_VERSION),
              "clients A and B (current version) must be served per-frame streams");

#define LOG(...) do { std::fprintf(stderr, __VA_ARGS__); std::fflush(stderr); } while(0)

// ── Helpers ──────────────────────────────────────────────────────────────

static bool wait_for_message(NetClient& client, ControlMessageType type,
                             std::vector<uint8_t>& payload, int timeout_ms = TIMEOUT_MS) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        auto msg = client.incoming().try_pop();
        if (msg) {
            if (msg->type == type) {
                payload = std::move(msg->payload);
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// Wait for a message, collecting other messages into side_msgs
static bool wait_for_message_collecting(
    NetClient& client, ControlMessageType type,
    std::vector<uint8_t>& payload,
    std::vector<std::pair<ControlMessageType, std::vector<uint8_t>>>& side_msgs,
    int timeout_ms = TIMEOUT_MS)
{
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        auto msg = client.incoming().try_pop();
        if (msg) {
            if (msg->type == type) {
                payload = std::move(msg->payload);
                return true;
            }
            side_msgs.emplace_back(msg->type, std::move(msg->payload));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// Find a message of given type in a collected side_msgs vector
[[maybe_unused]] static bool find_side_message(
    const std::vector<std::pair<ControlMessageType, std::vector<uint8_t>>>& msgs,
    ControlMessageType type, std::vector<uint8_t>& payload)
{
    for (auto& [t, p] : msgs) {
        if (t == type) { payload = p; return true; }
    }
    return false;
}

static void drain_messages(NetClient& client) {
    while (auto msg = client.incoming().try_pop()) {}
}

#define TEST_ASSERT(cond, msg) do {                               \
    if (!(cond)) {                                                \
        LOG("FAIL: %s (line %d)\n", msg, __LINE__);              \
        return 1;                                                 \
    }                                                             \
} while(0)

static bool wait_for_connect(NetClient& client, int timeout_ms = TIMEOUT_MS) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (client.is_connected()) return true;
        if (client.connect_failed()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// ── Crash handler ────────────────────────────────────────────────────────

#ifdef _WIN32
static LONG WINAPI crash_handler(EXCEPTION_POINTERS* ep) {
    auto* rec = ep->ExceptionRecord;
    auto* ctx = ep->ContextRecord;

    // Get module info for faulting address
    HMODULE mod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (LPCSTR)rec->ExceptionAddress, &mod);
    char mod_name[MAX_PATH] = "?";
    if (mod) GetModuleFileNameA(mod, mod_name, MAX_PATH);
    auto offset = (uintptr_t)rec->ExceptionAddress - (uintptr_t)mod;

    std::fprintf(stderr, "\n!!! CRASH: exception=0x%08lX\n", rec->ExceptionCode);
    std::fprintf(stderr, "    Faulting addr: %p (%s + 0x%llx)\n",
                 rec->ExceptionAddress, mod_name, (unsigned long long)offset);
    std::fprintf(stderr, "    RIP=%p RSP=%p\n", (void*)ctx->Rip, (void*)ctx->Rsp);
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        std::fprintf(stderr, "    Access violation %s address %p\n",
                     rec->ExceptionInformation[0] == 0 ? "reading" : "writing",
                     (void*)rec->ExceptionInformation[1]);
    }

    // Walk the stack manually using RBP chain
    std::fprintf(stderr, "    Stack trace (return addresses):\n");
    auto* rsp = reinterpret_cast<uintptr_t*>(ctx->Rsp);
    for (int i = 0; i < 20 && rsp; i++) {
        uintptr_t addr = *rsp++;
        HMODULE m = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)addr, &m) && m) {
            char name[MAX_PATH];
            GetModuleFileNameA(m, name, MAX_PATH);
            std::fprintf(stderr, "      [%d] %p (%s + 0x%llx)\n",
                         i, (void*)addr, name, (unsigned long long)(addr - (uintptr_t)m));
        }
    }

    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

// ── Main ─────────────────────────────────────────────────────────────────

int main() {
#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_handler);
#endif
    LOG("=== Parties Integration Test ===\n\n");

    // ── Init subsystems ──
    TEST_ASSERT(crypto_init(), "crypto_init");
    TEST_ASSERT(net_init(), "net_init");
    TEST_ASSERT(quic_init() != nullptr, "quic_init");
    LOG("[1/21] Subsystems initialized\n");

    // ── Create temp directory for test artifacts ──
    auto tmp = fs::temp_directory_path() / "parties_integration_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    std::string db_path   = (tmp / "test.db").string();
    std::string cert_path = (tmp / "server.der").string();
    std::string key_path  = (tmp / "server.key.der").string();

    // ── Configure and start server ──
    Config cfg;
    cfg.listen_ip      = "127.0.0.1";
    cfg.port           = TEST_PORT;
    cfg.cert_file      = cert_path;
    cfg.key_file       = key_path;
    cfg.db_path        = db_path;
    cfg.max_clients    = 8;

    Server server;
    TEST_ASSERT(server.start(cfg), "server start");

    std::thread server_thread([&]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    LOG("[2/21] Server started on port %u\n", TEST_PORT);

    NetClient client_a;
    NetClient client_b;
    NetClient client_c;   // legacy (protocol 1.1) peer, connected in step 17

    std::mutex voice_mutex;
    std::vector<uint8_t> received_voice;
    std::atomic<bool> voice_received{false};

    auto cleanup = [&]() {
        client_a.disconnect();
        client_b.disconnect();
        client_c.disconnect();
        server.stop();
        if (server_thread.joinable()) server_thread.join();
        quic_cleanup();
        net_cleanup();
        crypto_cleanup();
        fs::remove_all(tmp);
    };

    // ── Generate Ed25519 keypairs for the three test clients ──
    SecretKey sk_a{}, sk_b{}, sk_c{};
    PublicKey pk_a{}, pk_b{}, pk_c{};
    TEST_ASSERT(derive_keypair("abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about", sk_a, pk_a),
                "derive keypair A");
    TEST_ASSERT(derive_keypair("zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo wrong", sk_b, pk_b),
                "derive keypair B");
    TEST_ASSERT(derive_keypair("letter advice cage absurd amount doctor acoustic avoid letter advice cage above", sk_c, pk_c),
                "derive keypair C");

    // ── Client A: connect + auth identity ──
    LOG("[3/21] Client A connecting...\n");
    TEST_ASSERT(client_a.connect("127.0.0.1", TEST_PORT), "client A connect");
    TEST_ASSERT(wait_for_connect(client_a), "client A wait for connect");
    LOG("[3/21] Client A connected\n");

    uint32_t user_a_id = 0;
    {
        auto now = static_cast<uint64_t>(std::time(nullptr));

        // Build message to sign: pubkey + display_name + timestamp
        BinaryWriter sign_buf;
        sign_buf.write_bytes(pk_a.data(), 32);
        sign_buf.write_string("test_user_a");
        sign_buf.write_u64(now);
        Signature sig_a{};
        TEST_ASSERT(ed25519_sign(sign_buf.data().data(), sign_buf.data().size(),
                    sk_a, pk_a, sig_a), "client A sign");

        // AUTH_IDENTITY: [version(2)][pubkey(32)][name][timestamp(8)][sig(64)][password]
        BinaryWriter w;
        w.write_u16(protocol::PROTOCOL_VERSION);
        w.write_bytes(pk_a.data(), 32);
        w.write_string("test_user_a");
        w.write_u64(now);
        w.write_bytes(sig_a.data(), 64);
        w.write_string("");  // no password

        TEST_ASSERT(client_a.send_message(ControlMessageType::AUTH_IDENTITY,
                    w.data().data(), w.data().size()), "client A send auth");

        std::vector<uint8_t> payload;
        TEST_ASSERT(wait_for_message(client_a, ControlMessageType::AUTH_RESPONSE, payload),
                    "client A auth response");
        // AUTH_RESPONSE: [user_id(4)][session_token(32)][role(1)][server_name][protocol_version(2)]
        BinaryReader r(payload.data(), payload.size());
        user_a_id = r.read_u32();
        TEST_ASSERT(user_a_id > 0, "client A user_id > 0");
        uint8_t token_a[32];
        r.read_bytes(token_a, 32);
        (void)r.read_u8();          // role
        (void)r.read_string();      // server name
        TEST_ASSERT(!r.error(), "client A auth response well-formed");
        TEST_ASSERT(r.remaining() >= 2, "client A auth response has trailing protocol version");
        TEST_ASSERT(r.read_u16() == protocol::PROTOCOL_VERSION,
                    "client A auth response protocol version == PROTOCOL_VERSION");

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        drain_messages(client_a);
    }
    LOG("[3/21] Client A authenticated (user_id=%u)\n", user_a_id);

    // ── Client B: connect + register + auth ──
    LOG("[4/21] Client B connecting...\n");
    TEST_ASSERT(client_b.connect("127.0.0.1", TEST_PORT), "client B connect");
    TEST_ASSERT(wait_for_connect(client_b), "client B wait for connect");
    LOG("[4/21] Client B connected\n");

    client_b.on_data_received = [&](const uint8_t* data, size_t len) {
        LOG("[callback] Client B on_data_received: %zu bytes, first=0x%02x\n",
            len, len > 0 ? data[0] : 0);
        std::lock_guard<std::mutex> lock(voice_mutex);
        received_voice.assign(data, data + len);
        voice_received = true;
        LOG("[callback] Voice data stored\n");
    };

    uint32_t user_b_id = 0;
    {
        auto now = static_cast<uint64_t>(std::time(nullptr));

        BinaryWriter sign_buf;
        sign_buf.write_bytes(pk_b.data(), 32);
        sign_buf.write_string("test_user_b");
        sign_buf.write_u64(now);
        Signature sig_b{};
        TEST_ASSERT(ed25519_sign(sign_buf.data().data(), sign_buf.data().size(),
                    sk_b, pk_b, sig_b), "client B sign");

        // AUTH_IDENTITY: [version(2)][pubkey(32)][name][timestamp(8)][sig(64)][password]
        BinaryWriter w;
        w.write_u16(protocol::PROTOCOL_VERSION);
        w.write_bytes(pk_b.data(), 32);
        w.write_string("test_user_b");
        w.write_u64(now);
        w.write_bytes(sig_b.data(), 64);
        w.write_string("");  // no password

        TEST_ASSERT(client_b.send_message(ControlMessageType::AUTH_IDENTITY,
                    w.data().data(), w.data().size()), "client B send auth");

        std::vector<uint8_t> payload;
        TEST_ASSERT(wait_for_message(client_b, ControlMessageType::AUTH_RESPONSE, payload),
                    "client B auth response");
        BinaryReader r(payload.data(), payload.size());
        user_b_id = r.read_u32();
        TEST_ASSERT(user_b_id > 0, "client B user_id > 0");
        uint8_t token_b[32];
        r.read_bytes(token_b, 32);
        (void)r.read_u8();          // role
        (void)r.read_string();      // server name
        TEST_ASSERT(!r.error(), "client B auth response well-formed");
        TEST_ASSERT(r.remaining() >= 2, "client B auth response has trailing protocol version");
        TEST_ASSERT(r.read_u16() == protocol::PROTOCOL_VERSION,
                    "client B auth response protocol version == PROTOCOL_VERSION");

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        drain_messages(client_b);
    }
    LOG("[4/21] Client B authenticated (user_id=%u)\n", user_b_id);

    // ── Both join channel 1 ──
    LOG("[5/21] Joining channel...\n");
    {
        BinaryWriter w;
        w.write_u32(1);
        TEST_ASSERT(client_a.send_message(ControlMessageType::CHANNEL_JOIN,
                    w.data().data(), w.data().size()), "client A send channel join");

        std::vector<uint8_t> payload;
        std::vector<std::pair<ControlMessageType, std::vector<uint8_t>>> side;
        TEST_ASSERT(wait_for_message_collecting(client_a, ControlMessageType::CHANNEL_USER_LIST,
                    payload, side), "client A channel user list");

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        drain_messages(client_a);
    }
    LOG("[5/21] Client A joined channel 1\n");

    {
        BinaryWriter w;
        w.write_u32(1);
        TEST_ASSERT(client_b.send_message(ControlMessageType::CHANNEL_JOIN,
                    w.data().data(), w.data().size()), "client B send channel join");

        std::vector<uint8_t> payload;
        std::vector<std::pair<ControlMessageType, std::vector<uint8_t>>> side;
        TEST_ASSERT(wait_for_message_collecting(client_b, ControlMessageType::CHANNEL_USER_LIST,
                    payload, side), "client B channel user list");

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        drain_messages(client_b);
    }
    LOG("[6/21] Client B joined channel 1\n");

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    drain_messages(client_a);

    // ── Client A: encode PCM silence and send as voice ──
    LOG("[7/21] Sending voice data...\n");
    std::vector<uint8_t> original_opus;
    {
        OpusCodec codec;
        TEST_ASSERT(codec.init_encoder(audio::SAMPLE_RATE, audio::CHANNELS,
                    audio::OPUS_BITRATE), "opus encoder init");

        float pcm[audio::OPUS_FRAME_SIZE] = {};
        uint8_t opus_buf[audio::MAX_OPUS_PACKET];
        int opus_len = codec.encode(pcm, audio::OPUS_FRAME_SIZE,
                                    opus_buf, audio::MAX_OPUS_PACKET);
        TEST_ASSERT(opus_len > 0, "opus encode");

        original_opus.assign(opus_buf, opus_buf + opus_len);

        std::vector<uint8_t> voice_pkt;
        voice_pkt.push_back(VOICE_PACKET_TYPE);
        uint16_t seq = 0;
        voice_pkt.insert(voice_pkt.end(), reinterpret_cast<uint8_t*>(&seq),
                         reinterpret_cast<uint8_t*>(&seq) + 2);
        voice_pkt.insert(voice_pkt.end(), opus_buf, opus_buf + opus_len);

        TEST_ASSERT(client_a.send_data(voice_pkt.data(), voice_pkt.size()),
                    "client A send voice");
    }
    LOG("[7/21] Client A sent %zu bytes of Opus data\n", original_opus.size());

    // ── Wait for Client B to receive forwarded voice ──
    LOG("[8/21] Waiting for Client B to receive voice...\n");
    {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(TIMEOUT_MS);
        while (!voice_received && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        TEST_ASSERT(voice_received.load(), "client B received voice data");

        std::lock_guard<std::mutex> lock(voice_mutex);

        TEST_ASSERT(received_voice.size() >= 7, "voice packet minimum size");
        TEST_ASSERT(received_voice[0] == VOICE_PACKET_TYPE, "voice packet type byte");

        uint32_t sender_id;
        std::memcpy(&sender_id, received_voice.data() + 1, 4);
        TEST_ASSERT(sender_id == user_a_id,
                    "voice packet sender_id matches client A");

        uint16_t recv_seq;
        std::memcpy(&recv_seq, received_voice.data() + 5, 2);
        TEST_ASSERT(recv_seq == 0, "voice packet sequence number");

        size_t opus_len = received_voice.size() - 7;
        TEST_ASSERT(opus_len == original_opus.size(), "opus payload length matches");
        TEST_ASSERT(std::memcmp(received_voice.data() + 7, original_opus.data(),
                    opus_len) == 0, "opus payload content matches");

        LOG("[8/21] Voice verified: sender=%u, opus=%zu bytes\n", sender_id, opus_len);
    }

    // ── Verify Opus decode ──
    {
        OpusCodec decoder;
        TEST_ASSERT(decoder.init_decoder(audio::SAMPLE_RATE, audio::CHANNELS),
                    "opus decoder init");

        float pcm_out[audio::OPUS_FRAME_SIZE];
        int decoded = decoder.decode(original_opus.data(),
                                     static_cast<int>(original_opus.size()),
                                     pcm_out, audio::OPUS_FRAME_SIZE);
        TEST_ASSERT(decoded == audio::OPUS_FRAME_SIZE,
                    "opus decode produces correct frame size");
    }
    LOG("[9/21] Opus decode OK\n");

    // ══════════════════════════════════════════════════════════════════════
    // SCREEN SHARING TESTS
    // ══════════════════════════════════════════════════════════════════════

    // ── Client A: start screen share ──
    LOG("[10/21] Screen share start...\n");
    {
        BinaryWriter w;
        w.write_u8(static_cast<uint8_t>(VideoCodecId::AV1));
        w.write_u16(1920);
        w.write_u16(1080);
        TEST_ASSERT(client_a.send_message(ControlMessageType::SCREEN_SHARE_START,
                    w.data().data(), w.data().size()), "client A send share start");

        // Both clients should receive SCREEN_SHARE_STARTED
        std::vector<uint8_t> payload_a, payload_b;
        TEST_ASSERT(wait_for_message(client_a, ControlMessageType::SCREEN_SHARE_STARTED, payload_a),
                    "client A receive SCREEN_SHARE_STARTED");
        TEST_ASSERT(wait_for_message(client_b, ControlMessageType::SCREEN_SHARE_STARTED, payload_b),
                    "client B receive SCREEN_SHARE_STARTED");

        // Verify payload: [user_id(4)][codec(1)][width(2)][height(2)][replay(1)]
        BinaryReader ra(payload_a.data(), payload_a.size());
        TEST_ASSERT(ra.read_u32() == user_a_id, "share started user_id (A)");
        TEST_ASSERT(ra.read_u8() == static_cast<uint8_t>(VideoCodecId::AV1), "share started codec (A)");
        TEST_ASSERT(ra.read_u16() == 1920, "share started width (A)");
        TEST_ASSERT(ra.read_u16() == 1080, "share started height (A)");
        TEST_ASSERT(ra.remaining() >= 1, "share started has trailing replay flag (A)");
        TEST_ASSERT(ra.read_u8() == 0, "share started replay flag is 0 for a live start (A)");

        BinaryReader rb(payload_b.data(), payload_b.size());
        TEST_ASSERT(rb.read_u32() == user_a_id, "share started user_id (B)");

        drain_messages(client_a);
        drain_messages(client_b);
    }
    LOG("[10/21] Screen share started, both clients notified\n");

    // ── Client B: subscribe to Client A's share; A is told it has a viewer ──
    LOG("[11/21] Client B subscribing to screen share...\n");
    {
        BinaryWriter w;
        w.write_u32(user_a_id);
        TEST_ASSERT(client_b.send_message(ControlMessageType::SCREEN_SHARE_VIEW,
                    w.data().data(), w.data().size()), "client B send share view");

        // SCREEN_SHARE_VIEWER to the sharer: [viewer_id(4)][watching(1)]
        std::vector<uint8_t> payload;
        std::vector<std::pair<ControlMessageType, std::vector<uint8_t>>> side;
        TEST_ASSERT(wait_for_message_collecting(client_a, ControlMessageType::SCREEN_SHARE_VIEWER,
                    payload, side), "client A receive SCREEN_SHARE_VIEWER (subscribe)");
        BinaryReader r(payload.data(), payload.size());
        TEST_ASSERT(r.read_u32() == user_b_id, "viewer notification: viewer is client B");
        TEST_ASSERT(r.read_u8() == 1, "viewer notification: watching == 1");
        TEST_ASSERT(!r.error(), "viewer notification: well-formed");
    }
    LOG("[11/21] Client B subscribed, Client A notified\n");

    // ── Client A: send a synthetic frame on the legacy video stream 1 ──
    // Client B speaks protocol 1.2, so the server re-originates it toward B on
    // a per-frame unidirectional stream: on_video_stream_frame(sender, [hdr][payload]).
    LOG("[12/21] Sending video frame on legacy stream 1...\n");
    std::mutex video_mutex;
    std::vector<uint8_t> received_video;          // [hdr14][payload] as delivered
    uint32_t received_video_sender = 0;
    std::atomic<bool> video_received{false};
    // Frames from the per-frame-stream step (14) are collected here too.
    struct RecvFrame { uint32_t sender; std::vector<uint8_t> frame; };
    std::vector<RecvFrame> stream_frames;

    client_b.on_video_stream_frame = [&](uint32_t sender_id, std::vector<uint8_t>&& frame) {
        std::lock_guard<std::mutex> lock(video_mutex);
        if (!video_received) {
            received_video = frame;
            received_video_sender = sender_id;
            video_received = true;
            return;
        }
        stream_frames.push_back(RecvFrame{ sender_id, std::move(frame) });
    };
    // Nothing must arrive on the legacy path for a 1.2 viewer; flag it if it does.
    std::atomic<bool> legacy_video_seen{false};
    client_b.on_data_received = [&](const uint8_t* data, size_t len) {
        if (len > 0 && data[0] == VIDEO_FRAME_PACKET_TYPE) legacy_video_seen = true;
    };

    std::vector<uint8_t> original_video_payload;
    {
        // Build synthetic video header + data:
        // [frame_number(4)][timestamp(4)][flags(1)][width(2)][height(2)][codec_id(1)][data(N)]
        BinaryWriter vw;
        vw.write_u32(1);       // frame_number
        vw.write_u32(0);       // timestamp
        vw.write_u8(VIDEO_FLAG_KEYFRAME);
        vw.write_u16(1920);
        vw.write_u16(1080);
        vw.write_u8(static_cast<uint8_t>(VideoCodecId::AV1));
        // Fake AV1 payload (32 bytes of pattern data)
        for (uint8_t i = 0; i < 32; i++) vw.write_u8(0xAA ^ i);
        original_video_payload = vw.data();

        // Wire packet: [type(1)][payload(N)]
        std::vector<uint8_t> pkt;
        pkt.push_back(VIDEO_FRAME_PACKET_TYPE);
        pkt.insert(pkt.end(), original_video_payload.begin(), original_video_payload.end());

        TEST_ASSERT(client_a.send_video(pkt.data(), pkt.size()),
                    "client A send video frame");
    }
    LOG("[12/21] Client A sent video frame (%zu bytes)\n",
        original_video_payload.size());

    // ── Client B: receive and verify the frame on a per-frame stream ──
    LOG("[13/21] Waiting for Client B to receive video frame...\n");
    {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(TIMEOUT_MS);
        while (!video_received && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        TEST_ASSERT(video_received.load(), "client B received video frame on a per-frame stream");
        TEST_ASSERT(!legacy_video_seen.load(), "no video on the legacy stream for a 1.2 viewer");

        std::lock_guard<std::mutex> lock(video_mutex);
        TEST_ASSERT(received_video_sender == user_a_id, "video sender_id matches client A");
        TEST_ASSERT(received_video.size() == original_video_payload.size(),
                    "video payload length matches");
        TEST_ASSERT(std::memcmp(received_video.data(), original_video_payload.data(),
                    received_video.size()) == 0, "video payload content matches");

        LOG("[13/21] Video frame verified: sender=%u, %zu bytes OK\n",
            received_video_sender, received_video.size());
    }

    // ── Client A: five frames on per-frame unidirectional streams ──
    LOG("[14/21] Sending 5 frames on per-frame streams...\n");
    std::vector<std::vector<uint8_t>> stream_originals;   // [hdr14][payload] per frame
    {
        for (uint32_t i = 0; i < 5; i++) {
            VideoFrameHeader hdr;
            hdr.frame_seq = 10 + i;
            hdr.timestamp = 10 + i;
            hdr.flags     = (i == 0) ? VIDEO_FLAG_KEYFRAME : 0;
            hdr.width     = 1920;
            hdr.height    = 1080;
            hdr.codec     = static_cast<uint8_t>(VideoCodecId::AV1);
            uint8_t hdr_bytes[VIDEO_FRAME_HEADER_SIZE];
            hdr.write(hdr_bytes);

            std::vector<uint8_t> payload(64 + 16 * i);
            for (size_t k = 0; k < payload.size(); k++)
                payload[k] = static_cast<uint8_t>((0x5A + i * 7 + k) & 0xFF);

            std::vector<uint8_t> whole(hdr_bytes, hdr_bytes + VIDEO_FRAME_HEADER_SIZE);
            whole.insert(whole.end(), payload.begin(), payload.end());
            stream_originals.push_back(std::move(whole));

            TEST_ASSERT(client_a.send_video_frame_stream(hdr_bytes, VIDEO_FRAME_HEADER_SIZE,
                        payload.data(), payload.size()), "client A send per-frame stream");
        }

        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(TIMEOUT_MS);
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(video_mutex);
                if (stream_frames.size() >= 5) break;
            }
            if (std::chrono::steady_clock::now() >= deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        std::lock_guard<std::mutex> lock(video_mutex);
        TEST_ASSERT(stream_frames.size() == 5, "client B received all 5 per-frame streams");
        for (size_t i = 0; i < 5; i++) {
            TEST_ASSERT(stream_frames[i].sender == user_a_id, "per-frame stream sender is client A");
            VideoFrameHeader hdr;
            TEST_ASSERT(VideoFrameHeader::parse(stream_frames[i].frame.data(),
                        stream_frames[i].frame.size(), hdr), "per-frame stream header parses");
            TEST_ASSERT(hdr.frame_seq == 10 + i, "per-frame streams arrive in order (frame_seq)");
            TEST_ASSERT(hdr.keyframe() == (i == 0), "per-frame stream keyframe flag");
            TEST_ASSERT(stream_frames[i].frame == stream_originals[i],
                        "per-frame stream content matches");
        }
        LOG("[14/21] All 5 per-frame streams verified (frame_seq 10..14)\n");
    }

    // ── Viewer notifications: B unsubscribes, then re-subscribes ──
    // The re-subscribe also makes the server originate a PLI toward A
    // (requester = B) — the PLI step below waits for it so the coalescer
    // window has passed before B's own PLI is sent.
    std::mutex pli_mutex;
    std::vector<uint8_t> received_pli;
    std::atomic<bool> pli_received{false};
    client_a.on_data_received = [&](const uint8_t* data, size_t len) {
        if (len >= 6 && data[0] == VIDEO_CONTROL_TYPE && data[1] == VIDEO_CTL_PLI) {
            std::lock_guard<std::mutex> lock(pli_mutex);
            received_pli.assign(data, data + len);
            pli_received = true;
        }
    };

    LOG("[15/21] Viewer notifications (unsubscribe / re-subscribe)...\n");
    {
        drain_messages(client_a);

        // Additive unsubscribe: [target(4)][action(1)=0]
        BinaryWriter w;
        w.write_u32(user_a_id);
        w.write_u8(0);
        TEST_ASSERT(client_b.send_message(ControlMessageType::SCREEN_SHARE_VIEW,
                    w.data().data(), w.data().size()), "client B send unsubscribe");

        std::vector<uint8_t> payload;
        std::vector<std::pair<ControlMessageType, std::vector<uint8_t>>> side;
        TEST_ASSERT(wait_for_message_collecting(client_a, ControlMessageType::SCREEN_SHARE_VIEWER,
                    payload, side), "client A receive SCREEN_SHARE_VIEWER (unsubscribe)");
        BinaryReader r(payload.data(), payload.size());
        TEST_ASSERT(r.read_u32() == user_b_id, "unsubscribe notification: viewer is client B");
        TEST_ASSERT(r.read_u8() == 0, "unsubscribe notification: watching == 0");

        // Additive re-subscribe: [target(4)][action(1)=1]
        BinaryWriter w2;
        w2.write_u32(user_a_id);
        w2.write_u8(1);
        TEST_ASSERT(client_b.send_message(ControlMessageType::SCREEN_SHARE_VIEW,
                    w2.data().data(), w2.data().size()), "client B send re-subscribe");

        std::vector<uint8_t> payload2;
        side.clear();
        TEST_ASSERT(wait_for_message_collecting(client_a, ControlMessageType::SCREEN_SHARE_VIEWER,
                    payload2, side), "client A receive SCREEN_SHARE_VIEWER (re-subscribe)");
        BinaryReader r2(payload2.data(), payload2.size());
        TEST_ASSERT(r2.read_u32() == user_b_id, "re-subscribe notification: viewer is client B");
        TEST_ASSERT(r2.read_u8() == 1, "re-subscribe notification: watching == 1");

        // The server-originated auto-PLI for the new viewer (unreliable
        // datagram: tolerated if it never shows up on this loopback run).
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        while (!pli_received && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (pli_received) {
            std::lock_guard<std::mutex> lock(pli_mutex);
            uint32_t requester_id = 0;
            std::memcpy(&requester_id, received_pli.data() + 2, 4);
            TEST_ASSERT(requester_id == user_b_id, "auto-PLI requester_id matches client B");
            LOG("[15/21] Auto-PLI on subscribe received by client A\n");
        } else {
            LOG("[15/21] (auto-PLI datagram not observed; continuing)\n");
        }
    }
    LOG("[15/21] Viewer notifications verified\n");

    // ── PLI test: Client B requests keyframe from Client A ──
    LOG("[16/21] Testing PLI forwarding...\n");
    {
        // Let the server's per-sharer PLI coalescer (200 ms) expire so B's
        // request is forwarded rather than merged into the auto-PLI above.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        {
            std::lock_guard<std::mutex> lock(pli_mutex);
            received_pli.clear();
            pli_received = false;
        }

        // Client B sends PLI: [VIDEO_CONTROL_TYPE][VIDEO_CTL_PLI][target_user_id(4)]
        std::vector<uint8_t> pli_pkt(6);
        pli_pkt[0] = VIDEO_CONTROL_TYPE;
        pli_pkt[1] = VIDEO_CTL_PLI;
        std::memcpy(pli_pkt.data() + 2, &user_a_id, 4);
        TEST_ASSERT(client_b.send_data(pli_pkt.data(), pli_pkt.size()),
                    "client B send PLI");

        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(TIMEOUT_MS);
        while (!pli_received && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        TEST_ASSERT(pli_received.load(), "client A received forwarded PLI");

        std::lock_guard<std::mutex> lock(pli_mutex);
        // Server forwards: [VIDEO_CONTROL_TYPE][VIDEO_CTL_PLI][requester_user_id(4)]
        TEST_ASSERT(received_pli.size() >= 6, "PLI packet size");
        TEST_ASSERT(received_pli[0] == VIDEO_CONTROL_TYPE, "PLI type byte");
        TEST_ASSERT(received_pli[1] == VIDEO_CTL_PLI, "PLI subtype byte");
        uint32_t requester_id;
        std::memcpy(&requester_id, received_pli.data() + 2, 4);
        TEST_ASSERT(requester_id == user_b_id, "PLI requester_id matches client B");

        LOG("[16/21] PLI verified: requester=%u\n", requester_id);
    }

    // ══════════════════════════════════════════════════════════════════════
    // LEGACY (PROTOCOL 1.1) PEER: LATE-JOIN REPLAY, STREAM-1 RE-ORIGINATION,
    // SUPPRESSED VIEWER NOTIFICATIONS, IMPLICIT UNSUBSCRIBE ON LEAVE
    // ══════════════════════════════════════════════════════════════════════

    // ── Client C: a protocol 1.1 peer joins while A is already sharing ──
    LOG("[17/21] Client C (protocol 1.1) connecting...\n");
    // C must never be handed a per-frame stream: flag it if the server tries.
    std::atomic<bool> c_stream_frame_seen{false};
    client_c.on_video_stream_frame = [&](uint32_t, std::vector<uint8_t>&&) {
        c_stream_frame_seen = true;
    };
    // Video toward C arrives on the legacy stream 1 as
    // [0x02][sender u32][hdr14][encoded]; PLI datagrams (0x03) are ignored here.
    std::mutex c_video_mutex;
    std::vector<uint8_t> c_received_video;
    std::atomic<bool> c_video_received{false};
    client_c.on_data_received = [&](const uint8_t* data, size_t len) {
        if (len == 0 || data[0] != VIDEO_FRAME_PACKET_TYPE) return;
        std::lock_guard<std::mutex> lock(c_video_mutex);
        if (c_video_received) return;
        c_received_video.assign(data, data + len);
        c_video_received = true;
    };

    TEST_ASSERT(client_c.connect("127.0.0.1", TEST_PORT), "client C connect");
    TEST_ASSERT(wait_for_connect(client_c), "client C wait for connect");
    LOG("[17/21] Client C connected\n");

    uint32_t user_c_id = 0;
    {
        auto now = static_cast<uint64_t>(std::time(nullptr));

        BinaryWriter sign_buf;
        sign_buf.write_bytes(pk_c.data(), 32);
        sign_buf.write_string("test_user_c");
        sign_buf.write_u64(now);
        Signature sig_c{};
        TEST_ASSERT(ed25519_sign(sign_buf.data().data(), sign_buf.data().size(),
                    sk_c, pk_c, sig_c), "client C sign");

        // AUTH_IDENTITY with protocol 1.1: the server checks the MAJOR only,
        // records the minor, and treats C as a legacy (stream 1) peer.
        BinaryWriter w;
        w.write_u16(LEGACY_PROTOCOL_VERSION);
        w.write_bytes(pk_c.data(), 32);
        w.write_string("test_user_c");
        w.write_u64(now);
        w.write_bytes(sig_c.data(), 64);
        w.write_string("");  // no password

        TEST_ASSERT(client_c.send_message(ControlMessageType::AUTH_IDENTITY,
                    w.data().data(), w.data().size()), "client C send auth");

        std::vector<uint8_t> payload;
        TEST_ASSERT(wait_for_message(client_c, ControlMessageType::AUTH_RESPONSE, payload),
                    "client C auth response (1.1 accepted: major check only)");
        BinaryReader r(payload.data(), payload.size());
        user_c_id = r.read_u32();
        TEST_ASSERT(user_c_id > 0, "client C user_id > 0");
        uint8_t token_c[32];
        r.read_bytes(token_c, 32);
        (void)r.read_u8();          // role
        (void)r.read_string();      // server name
        TEST_ASSERT(!r.error(), "client C auth response well-formed");
        // The trailing field is the SERVER's version, whatever the client said.
        TEST_ASSERT(r.remaining() >= 2, "client C auth response has trailing protocol version");
        TEST_ASSERT(r.read_u16() == protocol::PROTOCOL_VERSION,
                    "client C auth response protocol version == server PROTOCOL_VERSION");

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        drain_messages(client_c);
    }
    LOG("[17/21] Client C authenticated as a 1.1 peer (user_id=%u)\n", user_c_id);

    // C joins the channel while A's share is active: the late-join replay of
    // SCREEN_SHARE_STARTED must carry replay = 1 (A's live start carried 0).
    {
        BinaryWriter w;
        w.write_u32(1);
        TEST_ASSERT(client_c.send_message(ControlMessageType::CHANNEL_JOIN,
                    w.data().data(), w.data().size()), "client C send channel join");

        std::vector<uint8_t> payload;
        std::vector<std::pair<ControlMessageType, std::vector<uint8_t>>> side;
        TEST_ASSERT(wait_for_message_collecting(client_c, ControlMessageType::SCREEN_SHARE_STARTED,
                    payload, side), "client C receive late-join SCREEN_SHARE_STARTED");
        BinaryReader r(payload.data(), payload.size());
        TEST_ASSERT(r.read_u32() == user_a_id, "late-join share started: sharer is client A");
        TEST_ASSERT(r.read_u8() == static_cast<uint8_t>(VideoCodecId::AV1), "late-join share started codec");
        TEST_ASSERT(r.read_u16() == 1920, "late-join share started width");
        TEST_ASSERT(r.read_u16() == 1080, "late-join share started height");
        TEST_ASSERT(r.remaining() >= 1, "late-join share started has trailing replay flag");
        TEST_ASSERT(r.read_u8() == 1, "late-join share started replay flag == 1");

        // The channel user list is sent by the same join handler (before the
        // replay); accept it in either position.
        std::vector<uint8_t> list;
        if (!find_side_message(side, ControlMessageType::CHANNEL_USER_LIST, list))
            TEST_ASSERT(wait_for_message(client_c, ControlMessageType::CHANNEL_USER_LIST, list),
                        "client C channel user list");

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        drain_messages(client_c);
    }
    LOG("[17/21] Client C joined channel 1, late-join replay flag == 1 verified\n");

    // ── Client C subscribes with the legacy 4-byte form; A is told ──
    LOG("[18/21] Client C (legacy) subscribing to Client A...\n");
    {
        // Legacy single-select: [target(4)], no action byte.
        BinaryWriter w;
        w.write_u32(user_a_id);
        TEST_ASSERT(client_c.send_message(ControlMessageType::SCREEN_SHARE_VIEW,
                    w.data().data(), w.data().size()), "client C send legacy share view");

        // A (1.2) is notified about C. The server records the subscription
        // before it sends this, so once it has arrived the frame sent below
        // is guaranteed to be forwarded to C.
        std::vector<uint8_t> payload;
        std::vector<std::pair<ControlMessageType, std::vector<uint8_t>>> side;   // USER_JOINED_CHANNEL (C)
        TEST_ASSERT(wait_for_message_collecting(client_a, ControlMessageType::SCREEN_SHARE_VIEWER,
                    payload, side), "client A receive SCREEN_SHARE_VIEWER (client C subscribed)");
        BinaryReader r(payload.data(), payload.size());
        TEST_ASSERT(r.read_u32() == user_c_id, "viewer notification (C): viewer is client C");
        TEST_ASSERT(r.read_u8() == 1, "viewer notification (C): watching == 1");
        TEST_ASSERT(!r.error(), "viewer notification (C): well-formed");
    }
    LOG("[18/21] Client C subscribed, Client A notified\n");

    // ── Client A sends one keyframe on a per-frame stream; the server
    //    re-originates it toward C on the legacy stream 1 ──
    std::vector<uint8_t> legacy_frame_payload;
    {
        // A fresh subscriber is gated until a keyframe, so send one. B (1.2,
        // still subscribed) gets the same frame on a per-frame stream.
        VideoFrameHeader hdr;
        hdr.frame_seq = 20;
        hdr.timestamp = 20;
        hdr.flags     = VIDEO_FLAG_KEYFRAME;
        hdr.width     = 1920;
        hdr.height    = 1080;
        hdr.codec     = static_cast<uint8_t>(VideoCodecId::AV1);
        uint8_t hdr_bytes[VIDEO_FRAME_HEADER_SIZE];
        hdr.write(hdr_bytes);

        legacy_frame_payload.resize(96);
        for (size_t k = 0; k < legacy_frame_payload.size(); k++)
            legacy_frame_payload[k] = static_cast<uint8_t>((0xC3 + 5 * k) & 0xFF);

        TEST_ASSERT(client_a.send_video_frame_stream(hdr_bytes, VIDEO_FRAME_HEADER_SIZE,
                    legacy_frame_payload.data(), legacy_frame_payload.size()),
                    "client A send per-frame stream for the legacy viewer");

        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(TIMEOUT_MS);
        while (!c_video_received && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));

        TEST_ASSERT(c_video_received.load(), "client C received the frame on the legacy stream 1");
        TEST_ASSERT(!c_stream_frame_seen.load(), "no per-frame stream toward a 1.1 viewer");

        std::lock_guard<std::mutex> lock(c_video_mutex);
        // [0x02][sender u32][hdr14][encoded]
        const size_t prefix = 1 + 4 + VIDEO_FRAME_HEADER_SIZE;
        TEST_ASSERT(c_received_video.size() == prefix + legacy_frame_payload.size(),
                    "legacy frame length");
        TEST_ASSERT(c_received_video[0] == VIDEO_FRAME_PACKET_TYPE, "legacy frame type byte");
        uint32_t sender_id = 0;
        std::memcpy(&sender_id, c_received_video.data() + 1, 4);
        TEST_ASSERT(sender_id == user_a_id, "legacy frame sender_id matches client A");
        VideoFrameHeader got;
        TEST_ASSERT(VideoFrameHeader::parse(c_received_video.data() + 5,
                    c_received_video.size() - 5, got), "legacy frame header parses");
        TEST_ASSERT(got.frame_seq == 20, "legacy frame frame_seq");
        TEST_ASSERT(got.keyframe(), "legacy frame keyframe flag");
        TEST_ASSERT(got.width == 1920 && got.height == 1080, "legacy frame dimensions");
        TEST_ASSERT(std::memcmp(c_received_video.data() + prefix, legacy_frame_payload.data(),
                    legacy_frame_payload.size()) == 0, "legacy frame encoded bytes intact");
        LOG("[18/21] Legacy re-origination verified: sender=%u, %zu bytes on stream 1\n",
            sender_id, c_received_video.size());
    }

    // ── Client C shares; a pre-1.2 sharer gets no viewer notifications ──
    LOG("[19/21] Client C starts sharing; B subscribes to C...\n");
    {
        // Clear USER_JOINED_CHANNEL (C) and anything else still queued.
        drain_messages(client_a);
        drain_messages(client_b);
        drain_messages(client_c);

        BinaryWriter w;
        w.write_u8(static_cast<uint8_t>(VideoCodecId::H264));
        w.write_u16(1280);
        w.write_u16(720);
        TEST_ASSERT(client_c.send_message(ControlMessageType::SCREEN_SHARE_START,
                    w.data().data(), w.data().size()), "client C send share start");

        // Everyone in the channel (C included) gets a live SCREEN_SHARE_STARTED.
        NetClient* everyone[] = { &client_a, &client_b, &client_c };
        std::vector<uint8_t> payload;
        std::vector<std::pair<ControlMessageType, std::vector<uint8_t>>> side;
        for (NetClient* c : everyone) {
            side.clear();
            TEST_ASSERT(wait_for_message_collecting(*c, ControlMessageType::SCREEN_SHARE_STARTED,
                        payload, side), "receive SCREEN_SHARE_STARTED for client C");
            BinaryReader r(payload.data(), payload.size());
            TEST_ASSERT(r.read_u32() == user_c_id, "share started (C): sharer is client C");
            TEST_ASSERT(r.read_u8() == static_cast<uint8_t>(VideoCodecId::H264), "share started (C): codec");
            TEST_ASSERT(r.read_u16() == 1280, "share started (C): width");
            TEST_ASSERT(r.read_u16() == 720, "share started (C): height");
            TEST_ASSERT(r.remaining() >= 1, "share started (C): has trailing replay flag");
            TEST_ASSERT(r.read_u8() == 0, "share started (C): replay flag == 0 for a live start");
        }

        // B subscribes to C (additive form). The server sends C its auto-PLI
        // (priority datagram) but must NOT send SCREEN_SHARE_VIEWER to a
        // pre-1.2 sharer: bounded negative check, 500 ms.
        BinaryWriter w2;
        w2.write_u32(user_c_id);
        w2.write_u8(1);
        TEST_ASSERT(client_b.send_message(ControlMessageType::SCREEN_SHARE_VIEW,
                    w2.data().data(), w2.data().size()), "client B send subscribe to C");

        std::vector<uint8_t> unexpected;
        side.clear();
        TEST_ASSERT(!wait_for_message_collecting(client_c, ControlMessageType::SCREEN_SHARE_VIEWER,
                    unexpected, side, 500), "no SCREEN_SHARE_VIEWER for a pre-1.2 sharer (subscribe)");

        // B unsubscribes from C (additive form), then C stops sharing.
        BinaryWriter w3;
        w3.write_u32(user_c_id);
        w3.write_u8(0);
        TEST_ASSERT(client_b.send_message(ControlMessageType::SCREEN_SHARE_VIEW,
                    w3.data().data(), w3.data().size()), "client B send unsubscribe from C");
        TEST_ASSERT(client_c.send_message(ControlMessageType::SCREEN_SHARE_STOP,
                    nullptr, 0), "client C send share stop");

        for (NetClient* c : everyone) {
            side.clear();
            TEST_ASSERT(wait_for_message_collecting(*c, ControlMessageType::SCREEN_SHARE_STOPPED,
                        payload, side), "receive SCREEN_SHARE_STOPPED for client C");
            BinaryReader r(payload.data(), payload.size());
            TEST_ASSERT(r.read_u32() == user_c_id, "share stopped (C): sharer is client C");
            if (c == &client_c) {
                std::vector<uint8_t> viewer_msg;
                TEST_ASSERT(!find_side_message(side, ControlMessageType::SCREEN_SHARE_VIEWER, viewer_msg),
                            "no SCREEN_SHARE_VIEWER for a pre-1.2 sharer (unsubscribe)");
            }
        }
        TEST_ASSERT(!c_stream_frame_seen.load(), "still no per-frame stream toward client C");
    }
    LOG("[19/21] Client C shared and stopped; no viewer notifications to a 1.1 sharer\n");

    // ── Client C leaves the channel: the implicit unsubscribe notifies A ──
    LOG("[20/21] Client C leaves the channel...\n");
    {
        TEST_ASSERT(client_c.send_message(ControlMessageType::CHANNEL_LEAVE, nullptr, 0),
                    "client C send channel leave");

        std::vector<uint8_t> payload;
        std::vector<std::pair<ControlMessageType, std::vector<uint8_t>>> side;
        TEST_ASSERT(wait_for_message_collecting(client_a, ControlMessageType::SCREEN_SHARE_VIEWER,
                    payload, side), "client A receive SCREEN_SHARE_VIEWER (client C left)");
        BinaryReader r(payload.data(), payload.size());
        TEST_ASSERT(r.read_u32() == user_c_id, "leave notification: viewer is client C");
        TEST_ASSERT(r.read_u8() == 0, "leave notification: watching == 0");
        TEST_ASSERT(!r.error(), "leave notification: well-formed");

        // The others also learn that C left the channel.
        std::vector<uint8_t> left;
        side.clear();
        TEST_ASSERT(wait_for_message_collecting(client_b, ControlMessageType::USER_LEFT_CHANNEL,
                    left, side), "client B receive USER_LEFT_CHANNEL for client C");
        BinaryReader rl(left.data(), left.size());
        TEST_ASSERT(rl.read_u32() == user_c_id, "user left: client C");
        TEST_ASSERT(rl.read_u32() == 1, "user left: channel 1");
    }
    LOG("[20/21] Implicit unsubscribe on leave verified\n");

    // ── Client A: stop screen share (C has left; only A and B are told) ──
    LOG("[21/21] Screen share stop...\n");
    {
        TEST_ASSERT(client_a.send_message(ControlMessageType::SCREEN_SHARE_STOP,
                    nullptr, 0), "client A send share stop");

        std::vector<uint8_t> payload_a, payload_b;
        TEST_ASSERT(wait_for_message(client_a, ControlMessageType::SCREEN_SHARE_STOPPED, payload_a),
                    "client A receive SCREEN_SHARE_STOPPED");
        TEST_ASSERT(wait_for_message(client_b, ControlMessageType::SCREEN_SHARE_STOPPED, payload_b),
                    "client B receive SCREEN_SHARE_STOPPED");

        BinaryReader ra(payload_a.data(), payload_a.size());
        TEST_ASSERT(ra.read_u32() == user_a_id, "share stopped user_id (A)");
        BinaryReader rb(payload_b.data(), payload_b.size());
        TEST_ASSERT(rb.read_u32() == user_a_id, "share stopped user_id (B)");
    }
    LOG("[21/21] Screen share stopped, both clients notified\n");

    // Detach callbacks that capture locals of this scope before the clients
    // are torn down in cleanup().
    client_a.on_data_received = nullptr;
    client_b.on_data_received = nullptr;
    client_b.on_video_stream_frame = nullptr;
    client_c.on_data_received = nullptr;
    client_c.on_video_stream_frame = nullptr;

    LOG("\n=== ALL TESTS PASSED ===\n");
    cleanup();
    return 0;
}
