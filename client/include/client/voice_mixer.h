#pragma once

#include <parties/types.h>
#include <parties/codec.h>
#include <parties/audio_common.h>

#include <cstdint>
#include <functional>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <deque>

namespace parties::client {

class VoiceMixer {
public:
    // apply_makeup: when true (the primary voice mix) the +VOICE_OUTPUT_GAIN_DB
    // master makeup gain is applied to the whole mix, because decoded mic voice
    // sits well below full scale. The secondary/auxiliary mix (karaoke, join
    // sounds, plugin audio) passes false: that audio is already at its intended
    // level and must NOT get voice makeup or normalization.
    explicit VoiceMixer(bool apply_makeup = true);
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

    // Called (without mutex held) when a stream is created for a new user.
    // Use this to apply saved per-user audio preferences.
    std::function<void(UserId)> on_stream_created;

    // Set per-user volume (0.0 - 2.0)
    void set_user_volume(UserId user_id, float volume);

    // Get per-user volume (returns 1.0 if not set)
    float get_user_volume(UserId user_id) const;

    // Global volume for this whole mix (slider position 0.0 - 2.0, 1.0 = unity).
    // Multiplies on top of per-user volume. Lets the user balance the primary
    // voice mix against the secondary stream independently.
    void  set_master_volume(float position);
    float get_master_volume() const;

    // Per-user voice compression (normalization).
    // When enabled, automatically adjusts gain to bring this user's voice to the target level.
    // Overrides global normalization for this user's stream.
    void set_user_compression(UserId user_id, bool enabled, float target = 0.8f);
    bool get_user_compression(UserId user_id) const;
    float get_user_compression_target(UserId user_id) const;

    // Get per-user audio RMS level (0.0 - 1.0, updated each mix cycle)
    // Returns map of user_id -> level for all active streams.
    std::unordered_map<UserId, float> get_user_levels() const;

    // Cumulative decode-path counters across all streams (observability + tests).
    // normal = in-order decode; fec = a lost frame recovered from the next
    // packet's in-band FEC; plc = a frame concealed (no data/no successor);
    // resync = playout clock snapped forward over an unrecoverable gap.
    struct DecodeStats {
        uint64_t normal = 0;
        uint64_t fec = 0;
        uint64_t plc = 0;
        uint64_t resync = 0;
    };
    DecodeStats decode_stats() const;

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

        // Per-user compression (normalization)
        bool compress = false;
        float compress_target = 0.8f;  // Target RMS level (0.0 - 1.0)
        float compress_gain = 1.0f;    // Smoothed gain (converges toward target/rms)
    };

    UserStream& get_or_create_stream(UserId user_id);

    // Decode one frame from a user stream into pcm_out
    // Returns true if audio was produced
    bool decode_frame(UserStream& stream, float* pcm_out, int frame_size);

    mutable std::mutex mutex_;
    std::unordered_map<UserId, UserStream> streams_;
    DecodeStats stats_;   // guarded by mutex_ (updated inside mix_output)

    const bool apply_makeup_;       // primary mix: apply voice makeup gain; aux: don't
    float master_volume_ = 1.0f;    // slider position (0..2); guarded by mutex_

    // Temporary mix buffer (avoids allocation in audio callback)
    std::vector<float> mix_buf_;
    std::vector<float> user_buf_;

    static constexpr int MAX_JITTER_PACKETS = 10;   // Max queued packets per user (~200ms cap)
    static constexpr int JITTER_PRE_BUFFER  = 4;    // ~80ms pre-buffer — deep enough that a lost
                                                    // frame's successor is queued in time for FEC
    static constexpr int PLC_MAX_FRAMES     = 10;   // After this many concealed frames, go silent
    static constexpr int RESYNC_GAP         = 32;   // ~640ms: a forward gap this large isn't
                                                    // recoverable loss (seq reset / pathological
                                                    // burst) — snap the playout clock forward
};

} // namespace parties::client
