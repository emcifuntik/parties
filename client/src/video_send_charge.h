#pragma once

#include <parties/video_send_controller.h>

#include <cstddef>

namespace parties::client::detail {

// Charged before StreamSend because SEND_COMPLETE can run on the MsQuic worker
// before StreamSend returns; the destructor refunds a send that never queued.
class InFlightVideoBytes {
public:
    struct Completion {
        VideoSendAccounting* accounting = nullptr;
        size_t               bytes = 0;
        void retire() const { accounting->on_bytes_retired(bytes); }
    };

    InFlightVideoBytes(VideoSendAccounting& accounting, size_t bytes)
        : completion_{&accounting, bytes} {
        accounting.on_bytes_queued(bytes);
    }
    ~InFlightVideoBytes() {
        if (armed_) completion_.retire();
    }
    InFlightVideoBytes(const InFlightVideoBytes&) = delete;
    InFlightVideoBytes& operator=(const InFlightVideoBytes&) = delete;

    Completion completion() const { return completion_; }

    void disarm() && { armed_ = false; }

private:
    Completion completion_;
    bool       armed_ = true;
};

}
