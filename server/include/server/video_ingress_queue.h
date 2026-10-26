#pragma once

#include <server/session.h>
#include <parties/thread_queue.h>
#include <parties/video_common.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace parties::server {

class VideoIngressQueue {
public:
    struct Pending {
        std::shared_ptr<Session> session;
        uint64_t                 generation;
        std::vector<uint8_t>     wire;
    };

    void offer(const std::shared_ptr<Session>& session, std::span<const uint8_t> wire) {
        if (wire.size() < VIDEO_FRAME_HEADER_SIZE || wire.size() > VIDEO_FRAME_MAX_PAYLOAD_BYTES) return;
        const uint64_t generation = session->video_generation.load(std::memory_order_acquire);
        if (!session->phase.admits_video_streams()) return;
        const auto bytes = static_cast<int64_t>(wire.size());
        const int64_t pending = session->video_pending_bytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
        if (pending > Session::VIDEO_INGRESS_MAX_BUFFERED_BYTES) {
            session->video_pending_bytes.fetch_sub(bytes, std::memory_order_relaxed);
            return;
        }
        queue_.push({session, generation, std::vector<uint8_t>(wire.begin(), wire.end())});
    }

    std::vector<Pending> drain() { return queue_.drain(); }

private:
    ThreadQueue<Pending> queue_;
};

}
