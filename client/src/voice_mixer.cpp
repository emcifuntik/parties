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
    bool is_new;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        is_new = (streams_.find(user_id) == streams_.end());
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

    // Notify after releasing mutex so the handler can call set_user_volume etc.
    if (is_new && on_stream_created)
        on_stream_created(user_id);
}

// Sequence-continuity-aware playout with Opus in-band FEC recovery.
//
// Invariant: every emitted 20ms slot makes EXACTLY ONE decoder call and advances
// next_seq by EXACTLY ONE. The only same-packet double-decode is the documented
// pair — FEC(successor) to fill a lost slot, then a normal decode of that same
// successor on the next slot — two consecutive decoder calls with nothing between
// (required for Opus concealment state continuity).
//
// next_seq is the sequence number we owe THIS output slot. Buffer gaps come only
// from real loss/reorder: sequence numbers are contiguous across VAD silence
// (voice_seq_ only advances when a packet is actually sent), so a talk-spurt pause
// drains the queue (step 2 underrun) rather than jumping the sequence.
bool VoiceMixer::decode_frame(UserStream& stream, float* pcm_out, int frame_size) {
	ZoneScopedN("VoiceMixer::decode_frame");
    if (!stream.initialized) return false;

    // 1. Prime: buffer JITTER_PRE_BUFFER packets before first playback, then start
    //    the playout clock at the lowest buffered sequence number.
    if (!stream.primed) {
        if (stream.packet_queue.size() < static_cast<size_t>(JITTER_PRE_BUFFER))
            return false;
        stream.primed = true;
        stream.next_seq = stream.packet_queue.front().seq;
    }

    // 2. Underrun (empty queue): no successor to recover from — bounded PLC, then
    //    re-prime. This is also the path a VAD-silence pause takes.
    if (stream.packet_queue.empty()) {
        if (++stream.consecutive_empty <= PLC_MAX_FRAMES) {
            int d = stream.decoder.decode(nullptr, 0, pcm_out, frame_size, 0);
            ++stream.next_seq;
            ++stats_.plc;
            return d > 0;
        }
        stream.primed = false;
        stream.has_seq = false;
        return false;
    }

    // 3. Discard any stale front (older than the slot we owe) WITHOUT decoding —
    //    never rewind the playout clock. push_packet already rejects these, but a
    //    prime/underrun reset can transiently leave one.
    while (!stream.packet_queue.empty() &&
           seq_diff(stream.next_seq, stream.packet_queue.front().seq) < 0) {
        stream.packet_queue.pop_front();
    }
    if (stream.packet_queue.empty()) {
        ++stream.consecutive_empty;
        int d = stream.decoder.decode(nullptr, 0, pcm_out, frame_size, 0);
        ++stream.next_seq;
        ++stats_.plc;
        return d > 0;
    }

    uint16_t front = stream.packet_queue.front().seq;
    int16_t gap = seq_diff(stream.next_seq, front);   // 0 = owed frame present; >0 = hole(s)

    // 4. Resync valve: a gap this large is not recoverable loss (a voice_seq_ reset
    //    on reconnect, or a pathological burst). Snap the clock to the next packet.
    if (gap >= RESYNC_GAP) {
        stream.next_seq = front;
        gap = 0;
        ++stats_.resync;
    }

    // 5. In-order: the owed frame is at the front — normal decode and consume.
    if (gap == 0) {
        auto& pkt = stream.packet_queue.front();
        int d = stream.decoder.decode(pkt.data.data(), static_cast<int>(pkt.data.size()),
                                      pcm_out, frame_size, 0);
        stream.next_seq = pkt.seq + 1;
        stream.packet_queue.pop_front();
        stream.consecutive_empty = 0;
        ++stats_.normal;
        return d > 0;
    }

    // 6. Single loss: the owed frame is missing but its immediate successor is the
    //    front — reconstruct it from the successor's in-band FEC (LBRR). Do NOT pop
    //    the successor; it is decoded normally on the next slot (step 5), which is
    //    the required consecutive FEC(succ)→normal(succ) pair for Opus state
    //    continuity. (The successor can't be evicted between the two calls:
    //    MAX_JITTER_PACKETS drops the OLDEST and the successor is the newest in a
    //    burst.) Opus internally PLCs if the packet carried no LBRR — always terminal.
    if (gap == 1) {
        auto& succ = stream.packet_queue.front();   // seq == next_seq + 1
        int d = stream.decoder.decode(succ.data.data(), static_cast<int>(succ.data.size()),
                                      pcm_out, frame_size, /*decode_fec=*/1);
        ++stream.next_seq;
        stream.consecutive_empty = 0;
        ++stats_.fec;
        return d > 0;
    }

    // 7. Burst / non-adjacent gap (gap >= 2): the owed frame's successor isn't here
    //    yet (FEC is 1-deep), so conceal this slot with PLC and advance. As next_seq
    //    climbs the gap shrinks; the last missing frame before `front` is recovered
    //    via FEC (step 6), then step 5 resumes. Bound the run, then snap forward.
    if (++stream.consecutive_empty > PLC_MAX_FRAMES) {
        stream.next_seq = front;
        stream.consecutive_empty = 0;
        ++stats_.resync;
        auto& pkt = stream.packet_queue.front();
        int d = stream.decoder.decode(pkt.data.data(), static_cast<int>(pkt.data.size()),
                                      pcm_out, frame_size, 0);
        stream.next_seq = pkt.seq + 1;
        stream.packet_queue.pop_front();
        ++stats_.normal;
        return d > 0;
    }
    int d = stream.decoder.decode(nullptr, 0, pcm_out, frame_size, 0);
    ++stream.next_seq;
    ++stats_.plc;
    return d > 0;
}

VoiceMixer::DecodeStats VoiceMixer::decode_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
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

            // Per-user compression: adjust gain to normalize this user's level
            if (stream.compress && stream.level > 0.001f) {
                float target = stream.compress_target * stream.compress_target * stream.compress_target;
                float desired_gain = target / stream.level;
                desired_gain = std::clamp(desired_gain, 0.1f, 10.0f);
                constexpr float alpha = 0.05f;
                stream.compress_gain += alpha * (desired_gain - stream.compress_gain);
            }

            // Mix this user's decoded PCM into output. The stored volume is the
            // slider position (0..2); map it through the perceptual curve so the
            // control feels linear to the ear. Compression gain stays linear.
            float vol = audio::volume_position_to_gain(stream.volume)
                      * (stream.compress ? stream.compress_gain : 1.0f);
            for (int i = 0; i < chunk; i++) {
                output[written + i] += stream.pcm_buf[stream.pcm_pos + i] * vol;
            }
            stream.pcm_pos += chunk;
        }
        written += chunk;
    }

    // Apply master makeup gain (received voice is low at unity), then clip.
    const float master = audio::db_to_gain(audio::VOICE_OUTPUT_GAIN_DB);
    for (int i = 0; i < frame_count; i++) {
        float s = output[i] * master;
        if (s > 1.0f) s = 1.0f;
        else if (s < -1.0f) s = -1.0f;
        output[i] = s;
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
        it->second.volume = std::clamp(volume, 0.0f, 2.0f);
}

float VoiceMixer::get_user_volume(UserId user_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(user_id);
    if (it != streams_.end())
        return it->second.volume;
    return 1.0f;
}

void VoiceMixer::set_user_compression(UserId user_id, bool enabled, float target) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(user_id);
    if (it != streams_.end()) {
        it->second.compress = enabled;
        it->second.compress_target = std::clamp(target, 0.0f, 1.0f);
        if (!enabled)
            it->second.compress_gain = 1.0f; // Reset gain when disabling
    }
}

bool VoiceMixer::get_user_compression(UserId user_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(user_id);
    return it != streams_.end() && it->second.compress;
}

float VoiceMixer::get_user_compression_target(UserId user_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(user_id);
    if (it != streams_.end())
        return it->second.compress_target;
    return 0.8f;
}

std::unordered_map<UserId, float> VoiceMixer::get_user_levels() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::unordered_map<UserId, float> result;
    for (auto& [uid, stream] : streams_)
        result[uid] = stream.level;
    return result;
}

} // namespace parties::client
