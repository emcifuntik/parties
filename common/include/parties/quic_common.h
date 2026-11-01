#pragma once

#include <msquic.h>

namespace parties {

// Initialize the MsQuic library (call once at startup).
// Returns the API table, or nullptr on failure.
const QUIC_API_TABLE* quic_init();

// Clean up MsQuic (call once at shutdown).
void quic_cleanup();

// Get the API table (must call quic_init() first).
const QUIC_API_TABLE* quic_api();

// Turns the UDP offloads of the platform datapath off (Windows: send
// segmentation USO and receive coalescing URO). Some NIC, VPN and filter
// drivers corrupt or stall offloaded traffic, which looks like a link that
// chokes under load. Only effective before the first connection: MsQuic
// creates its datapath at the first RegistrationOpen and keeps it until
// quic_cleanup(). Returns false when it is too late (applies after a restart).
bool quic_disable_datapath_offloads(bool disable);

// Settings shared by the client and the server for real-time media:
//   * BBR congestion control. CUBIC reads random loss as congestion: at 5 %
//     loss and 100 ms RTT it carries about 0.5 Mbps, at 25 % loss a fraction
//     of that, so a screen share stalls on any lossy Wi-Fi or mobile link.
//   * Room for many concurrent per-frame video streams (one unidirectional
//     stream per frame, several shares and cameras at 60 fps, held open by
//     retransmissions at high RTT).
void apply_media_transport_settings(QUIC_SETTINGS& settings);

// Network compatibility mode, per connection: a fixed 1280-byte path MTU.
// MsQuic only ever raises the MTU it discovered, so a path that shrinks
// later (VPN reconnect, PPPoE, hotspot) loses every full-size packet.
void apply_compatibility_transport_settings(QUIC_SETTINGS& settings);

// Our ALPN identifier
inline QUIC_BUFFER make_alpn() {
    static uint8_t alpn[] = "parties";
    return { 7, alpn };
}

} // namespace parties
