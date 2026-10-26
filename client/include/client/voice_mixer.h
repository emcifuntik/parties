#pragma once

#include <client/audio_activity.h>
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
    explicit VoiceMixer(bool apply_makeup = true);
    ~VoiceMixer();

    void push_packet(UserId user_id, uint16_t seq, const uint8_t* opus_data, size_t opus_len);

    void mix_output(float* output, int frame_count);

    void remove_user(UserId user_id);

    void clear();

    std::function<void(UserId)> on_stream_created;

    void set_user_volume(UserId user_id, float volume);

    float get_user_volume(UserId user_id) const;

    void  set_master_volume(float position);
    float get_master_volume() const;

    void set_user_compression(UserId user_id, bool enabled, float target = 0.8f);
    bool get_user_compression(UserId user_id) const;
    float get_user_compression_target(UserId user_id) const;

    std::unordered_map<UserId, float> get_user_levels() const;

    std::vector<UserId> get_active_users() const;

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
        std::deque<JitterPacket> packet_queue;
        float volume = 1.0f;
        int consecutive_empty = 0;
        bool initialized = false;
        bool primed = false;
        uint16_t next_seq = 0;
        bool has_seq = false;

        std::vector<float> pcm_buf;
        size_t pcm_pos = 0;

        float level = 0.0f;
        AudioActivity activity;

        bool compress = false;
        float compress_target = 0.8f;
        float compress_gain = 1.0f;
    };

    UserStream& get_or_create_stream(UserId user_id);

    bool decode_frame(UserStream& stream, float* pcm_out, int frame_size);

    mutable std::mutex mutex_;
    std::unordered_map<UserId, UserStream> streams_;
    DecodeStats stats_;

    const bool apply_makeup_;
    float master_volume_ = 1.0f;

    std::vector<float> mix_buf_;
    std::vector<float> user_buf_;

    static constexpr int MAX_JITTER_PACKETS = 10;
    static constexpr int JITTER_PRE_BUFFER  = 4;
    static constexpr int PLC_MAX_FRAMES     = 10;
    static constexpr int RESYNC_GAP         = 32;
};

}
