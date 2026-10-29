#pragma once

#include <parties/types.h>

#include <atomic>
#include <functional>
#include <string>
#include <unordered_set>

namespace parties::client {

class LobbyModel;

// Why picture-in-picture closed. A platform keeps the stream's picture alive
// (it returns to the grid) unless the stream itself is gone.
enum class PipCloseReason {
    UserClosed,       // PiP close action: keep watching in the grid
    ReturnedToMain,   // PiP "return" action: grid in the restored main window
    StreamUnwatched,  // stream ended, stopped watching, left channel, disconnected
};

// Platform-neutral picture-in-picture state: closed, or open on exactly one
// watched stream. AppCore owns one instance; platforms only present it.
//
// The authoritative input is the watched set. AppCore calls reconcile() after
// every watch mutation, so every path that drops a stream (share stopped,
// stop watching, leave channel, disconnect) closes PiP the same way.
//
// Main thread only, except stream(), which decode/render threads may read.
class StreamPipController {
public:
    struct Presenter {
        // Show the PiP surface for `stream`, or switch the open one to it.
        std::function<void(UserId stream, const std::string& title)> show;
        // Hide the PiP surface that was showing `stream`.
        std::function<void(UserId stream, PipCloseReason reason)> hide;
        // A stream changed surface (grid -> PiP or back). The new surface starts
        // empty, so the platform asks the sharer for a fresh keyframe.
        std::function<void(UserId stream)> stream_moved;
    };

    // `model` mirrors the state for RML (pip_stream_id); may be null in tests.
    void attach(LobbyModel* model, Presenter presenter);

    UserId stream() const { return stream_.load(std::memory_order_acquire); }
    bool is_open() const { return stream() != 0; }

    // Open PiP on a watched stream, or switch the open PiP to it. Returns false
    // (and changes nothing) when `id` is not in `watched`.
    bool open(UserId id, const std::unordered_set<UserId>& watched);
    // PiP button: open on `id`, or close when `id` is already in PiP.
    void toggle(UserId id, const std::unordered_set<UserId>& watched);
    void close(PipCloseReason reason);
    // Close PiP when its stream left the watched set.
    void reconcile(const std::unordered_set<UserId>& watched);

private:
    std::string title_for(UserId id) const;
    void set_stream(UserId id);

    std::atomic<UserId> stream_{0};
    LobbyModel* model_ = nullptr;
    Presenter presenter_;
};

} // namespace parties::client
