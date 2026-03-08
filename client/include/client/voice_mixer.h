#pragma once

#include <parties/types.h>
#include <parties/codec.h>
#include <parties/audio_common.h>

#include <cstdint>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <deque>

namespace parties::client {

class VoiceMixer {
public:
    VoiceMixer();
    ~VoiceMixer();

    // Feed an incoming opus packet for a given user (with sequence number for reordering)
    void push_packet(UserId user_id, uint16_t seq, const uint8_t* opus_data, size_t opus_len);

    // Called from the audio playback callback.
    // Decodes all active streams, mixes them into output buffer.
    void mix_output(float* output, int frame_count);

    // Remove a user stream (e.g., user left channel)
    void remove_user(UserId user_id);

    // Remove all user streams
    void clear();

    // Set per-user volume (0.0 - 1.0)
    void set_user_volume(UserId user_id, float volume);

    // Get per-user audio RMS level (0.0 - 1.0, updated each mix cycle)
    // Returns map of user_id -> level for all active streams.
    std::unordered_map<UserId, float> get_user_levels() const;

private:
    struct JitterPacket {
        uint16_t seq;
        std::vector<uint8_t> data;
    };

    struct UserStream {
        OpusCodec decoder;
        std::deque<JitterPacket> packet_queue;  // Sorted by sequence number
        float volume = 1.0f;
        int consecutive_empty = 0;     // Count of consecutive empty frames (for PLC)
        bool initialized = false;
        bool primed = false;           // True once we've buffered enough packets to start
        uint16_t next_seq = 0;         // Expected next sequence number for playback
        bool has_seq = false;          // True once first packet establishes sequence

        // Decoded PCM buffer for partial reads
        std::vector<float> pcm_buf;
        size_t pcm_pos = 0;

        // Audio level (RMS of last decoded frame, updated in mix_output)
        float level = 0.0f;
    };

    UserStream& get_or_create_stream(UserId user_id);

    // Decode one frame from a user stream into pcm_out
    // Returns true if audio was produced
    bool decode_frame(UserStream& stream, float* pcm_out, int frame_size);

    mutable std::mutex mutex_;
    std::unordered_map<UserId, UserStream> streams_;

    // Temporary mix buffer (avoids allocation in audio callback)
    std::vector<float> mix_buf_;
    std::vector<float> user_buf_;

    static constexpr int MAX_JITTER_PACKETS = 10;   // Max queued packets per user
    static constexpr int JITTER_PRE_BUFFER  = 3;    // Buffer this many before starting playback
    static constexpr int PLC_MAX_FRAMES     = 10;   // After this many PLC frames, go silent
};

} // namespace parties::client
