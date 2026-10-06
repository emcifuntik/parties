#include <client/stream_pip.h>
#include <client/lobby_model.h>

namespace parties::client {

void StreamPipController::attach(LobbyModel* model, Presenter presenter) {
    model_ = model;
    presenter_ = std::move(presenter);
}

std::string StreamPipController::title_for(UserId id) const {
    if (!model_) return {};
    for (const auto& sharer : model_->sharers.get())
        if (sharer.id == static_cast<int>(id)) return std::string(sharer.name);
    return {};
}

void StreamPipController::set_stream(UserId id) {
    stream_.store(id, std::memory_order_release);
    if (model_) model_->pip_stream_id = static_cast<int>(id);
}

bool StreamPipController::open(UserId id, const std::unordered_set<UserId>& watched) {
    if (id == 0 || watched.count(id) == 0) return false;
    const UserId previous = stream();
    if (previous == id) return true;

    set_stream(id);
    if (presenter_.show) presenter_.show(id, title_for(id));
    // Switching: the previous stream returns to its grid cell, the new one
    // leaves its cell. Both surfaces start empty.
    if (presenter_.stream_moved) {
        if (previous != 0) presenter_.stream_moved(previous);
        presenter_.stream_moved(id);
    }
    return true;
}

void StreamPipController::toggle(UserId id, const std::unordered_set<UserId>& watched) {
    if (id != 0 && stream() == id)
        close(PipCloseReason::UserClosed);
    else
        open(id, watched);
}

void StreamPipController::close(PipCloseReason reason) {
    const UserId previous = stream();
    if (previous == 0) return;
    set_stream(0);
    if (presenter_.hide) presenter_.hide(previous, reason);
    if (reason != PipCloseReason::StreamUnwatched && presenter_.stream_moved)
        presenter_.stream_moved(previous);
}

void StreamPipController::reconcile(const std::unordered_set<UserId>& watched) {
    const UserId current = stream();
    if (current != 0 && watched.count(current) == 0)
        close(PipCloseReason::StreamUnwatched);
}

} // namespace parties::client
