#pragma once

#include <parties/types.h>

#include <atomic>
#include <functional>
#include <string>
#include <unordered_set>

namespace parties::client {

class LobbyModel;

enum class PipCloseReason {
    UserClosed,
    ReturnedToMain,
    StreamUnwatched,
};

class StreamPipController {
public:
    struct Presenter {
        std::function<void(UserId stream, const std::string& title)> show;
        std::function<void(UserId stream, PipCloseReason reason)> hide;
        std::function<void(UserId stream)> stream_moved;
    };

    void attach(LobbyModel* model, Presenter presenter);

    UserId stream() const { return stream_.load(std::memory_order_acquire); }
    bool is_open() const { return stream() != 0; }

    bool open(UserId id, const std::unordered_set<UserId>& watched);
    void toggle(UserId id, const std::unordered_set<UserId>& watched);
    void close(PipCloseReason reason);
    void reconcile(const std::unordered_set<UserId>& watched);

private:
    std::string title_for(UserId id) const;
    void set_stream(UserId id);

    std::atomic<UserId> stream_{0};
    LobbyModel* model_ = nullptr;
    Presenter presenter_;
};

}
