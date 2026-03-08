#include <client/voice_mixer.h>

#include <cstring>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <parties/profiler.h>

namespace parties::client {

VoiceMixer::VoiceMixer()
    : mix_buf_(audio::OPUS_FRAME_SIZE, 0.0f)
    , user_buf_(audio::OPUS_FRAME_SIZE, 0.0f) {}

VoiceMixer::~VoiceMixer() = default;

VoiceMixer::UserStream& VoiceMixer::get_or_create_stream(UserId user_id) {
    auto it = streams_.find(user_id);
    if (it != streams_.end()) return it->second;

    auto& stream = streams_[user_id];
    stream.decoder.init_decoder(audio::SAMPLE_RATE, audio::CHANNELS);
    stream.initialized = true;
    stream.pcm_buf.resize(audio::OPUS_FRAME_SIZE, 0.0f);
    stream.pcm_pos = audio::OPUS_FRAME_SIZE; // Empty — will trigger decode on first read
    return stream;
}

// Signed distance from a to b in uint16_t sequence space.
// Positive means b is ahead of a.
static int16_t seq_diff(uint16_t a, uint16_t b) {
    return static_cast<int16_t>(b - a);
}

void VoiceMixer::push_packet(UserId user_id, uint16_t seq, const uint8_t* opus_data, size_t opus_len) {
	ZoneScopedN("VoiceMixer::push_packet");
    std::lock_guard<std::mutex> lock(mutex_);
    auto& stream = get_or_create_stream(user_id);

    // First packet establishes the sequence baseline
    if (!stream.has_seq) {
        stream.next_seq = seq;
        stream.has_seq = true;
    }

    // Discard packets that are too old (already played)
    if (seq_diff(stream.next_seq, seq) < 0)
        return;

    // Find insertion index (sorted by seq). Search from the back since
    // packets typically arrive roughly in order.
    size_t insert_idx = stream.packet_queue.size();
    for (size_t i = stream.packet_queue.size(); i > 0; --i) {
        int16_t d = seq_diff(stream.packet_queue[i - 1].seq, seq);
        if (d == 0) return;  // duplicate
        if (d > 0) {
            insert_idx = i;
            break;
        }
        insert_idx = i - 1;
    }

    // Drop oldest if buffer is full
    if (stream.packet_queue.size() >= MAX_JITTER_PACKETS) {
        stream.packet_queue.pop_front();
        if (insert_idx > 0) --insert_idx;
    }

    stream.packet_queue.insert(stream.packet_queue.begin() + insert_idx,
                               {seq, {opus_data, opus_data + opus_len}});
    stream.consecutive_empty = 0;
}

bool VoiceMixer::decode_frame(UserStream& stream, float* pcm_out, int frame_size) {
	ZoneScopedN("VoiceMixer::decode_frame");
    if (!stream.initialized) return false;

    // Wait until we've buffered enough packets before starting playback
    if (!stream.primed) {
        if (stream.packet_queue.size() < JITTER_PRE_BUFFER)
            return false;
        stream.primed = true;
    }

    if (!stream.packet_queue.empty()) {
        auto& pkt = stream.packet_queue.front();
        int decoded = stream.decoder.decode(pkt.data.data(), static_cast<int>(pkt.data.size()),
                                             pcm_out, frame_size);
        stream.next_seq = pkt.seq + 1;
        stream.packet_queue.pop_front();
        stream.consecutive_empty = 0;

        if (decoded > 0) return true;
    }

    // No packet available — try PLC (pass nullptr to Opus)
    stream.consecutive_empty++;
    if (stream.consecutive_empty <= PLC_MAX_FRAMES) {
        int decoded = stream.decoder.decode(nullptr, 0, pcm_out, frame_size);
        return decoded > 0;
    }

    // Too many consecutive PLC frames — reset priming so we re-buffer
    stream.primed = false;
    stream.has_seq = false;
    return false;
}

void VoiceMixer::mix_output(float* output, int frame_count) {
	ZoneScopedN("VoiceMixer::mix_output");
    std::lock_guard<std::mutex> lock(mutex_);

    std::memset(output, 0, frame_count * sizeof(float));

    if (streams_.empty()) return;

    // Process frame_count samples which may span multiple OPUS_FRAME_SIZE blocks
    int written = 0;
    while (written < frame_count) {
        int chunk = std::min(frame_count - written, audio::OPUS_FRAME_SIZE);

        // Mix all active user streams
        for (auto& [uid, stream] : streams_) {
            // Check if we need to decode a new frame for this user
            if (stream.pcm_pos >= static_cast<size_t>(audio::OPUS_FRAME_SIZE)) {
                // Need a new decoded frame
                user_buf_.assign(audio::OPUS_FRAME_SIZE, 0.0f);
                if (decode_frame(stream, user_buf_.data(), audio::OPUS_FRAME_SIZE)) {
                    stream.pcm_buf = user_buf_;
                    // Compute RMS for speaking detection
                    float sum = 0.0f;
                    for (int i = 0; i < audio::OPUS_FRAME_SIZE; i++)
                        sum += user_buf_[i] * user_buf_[i];
                    stream.level = std::sqrt(sum / audio::OPUS_FRAME_SIZE);
                } else {
                    std::memset(stream.pcm_buf.data(), 0,
                               audio::OPUS_FRAME_SIZE * sizeof(float));
                    stream.level = 0.0f;
                }
                stream.pcm_pos = 0;
            }

            // Mix this user's decoded PCM into output
            float vol = stream.volume;
            for (int i = 0; i < chunk; i++) {
                output[written + i] += stream.pcm_buf[stream.pcm_pos + i] * vol;
            }
            stream.pcm_pos += chunk;
        }
        written += chunk;
    }

    // Soft-clip the mixed output to [-1, 1]
    for (int i = 0; i < frame_count; i++) {
        if (output[i] > 1.0f) output[i] = 1.0f;
        else if (output[i] < -1.0f) output[i] = -1.0f;
    }
}

void VoiceMixer::remove_user(UserId user_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    streams_.erase(user_id);
}

void VoiceMixer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    streams_.clear();
}

void VoiceMixer::set_user_volume(UserId user_id, float volume) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(user_id);
    if (it != streams_.end())
        it->second.volume = std::clamp(volume, 0.0f, 1.0f);
}

std::unordered_map<UserId, float> VoiceMixer::get_user_levels() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::unordered_map<UserId, float> result;
    for (auto& [uid, stream] : streams_)
        result[uid] = stream.level;
    return result;
}

} // namespace parties::client
