#pragma once

// Internal header — included only by net_client_msquic.cpp (the single
// transport implementation compiled on every platform).
//
// Pure wire-protocol parsing: accumulates bytes into a buffer and extracts
// complete framed messages.  No transport library dependencies.

#include <client/net_client.h>
#include <parties/protocol.h>
#include <parties/thread_queue.h>
#include <parties/log.h>

#include <cstring>
#include <functional>
#include <vector>

namespace parties::client::parsing {

// Control stream — [u32 total_len][u16 type][payload]
// Pushes complete ServerMessage values into `incoming`.
inline void process_stream_data(const uint8_t* data, size_t len,
                                std::vector<uint8_t>& buf,
                                ThreadQueue<ServerMessage>& incoming)
{
    buf.insert(buf.end(), data, data + len);

    while (buf.size() >= 6) {
        uint32_t msg_len;
        std::memcpy(&msg_len, buf.data(), 4);

        if (msg_len < 2 || msg_len > 1024u * 1024u) {
            LOG_ERROR("Invalid control message length {} — resetting", msg_len);
            buf.clear();
            break;
        }

        size_t total_needed = 4 + msg_len;
        if (buf.size() < total_needed) break;

        uint16_t raw_type;
        std::memcpy(&raw_type, buf.data() + 4, 2);

        uint32_t payload_len = msg_len - 2;
        ServerMessage msg;
        msg.type = static_cast<protocol::ControlMessageType>(raw_type);
        if (payload_len > 0)
            msg.payload.assign(buf.data() + 6, buf.data() + 6 + payload_len);

        incoming.push(std::move(msg));
        buf.erase(buf.begin(), buf.begin() + total_needed);
    }
}

// Video stream — [u32 frame_len][data]
// Calls on_data_received for each complete frame.
inline void process_video_stream_data(
    const uint8_t* data, size_t len,
    std::vector<uint8_t>& buf,
    const std::function<void(const uint8_t*, size_t)>& on_data_received)
{
    buf.insert(buf.end(), data, data + len);

    while (buf.size() >= 4) {
        uint32_t frame_len;
        std::memcpy(&frame_len, buf.data(), 4);

        if (frame_len == 0 || frame_len > 4u * 1024u * 1024u) {
            LOG_ERROR("Invalid video frame length {} — resetting", frame_len);
            buf.clear();
            break;
        }

        size_t total_needed = 4 + frame_len;
        if (buf.size() < total_needed) break;

        if (on_data_received)
            on_data_received(buf.data() + 4, frame_len);

        buf.erase(buf.begin(), buf.begin() + total_needed);
    }
}

} // namespace parties::client::parsing
