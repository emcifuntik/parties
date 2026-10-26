#pragma once

#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <optional>

namespace parties {

template<typename T>
class ThreadQueue {
public:
    void push(T item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(item));
        }
        cv_.notify_one();
    }

    std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return std::nullopt;
        T item = std::move(queue_.front());
        queue_.pop_front();
        return item;
    }

    T pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !queue_.empty(); });
        T item = std::move(queue_.front());
        queue_.pop_front();
        return item;
    }

    std::vector<T> drain() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<T> items;
        items.reserve(queue_.size());
        while (!queue_.empty()) {
            items.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
        return items;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<T> queue_;
};

}
