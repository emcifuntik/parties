#pragma once

#include <parties/types.h>

#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <variant>

namespace parties::server {

class AuthReplayGuard {
public:
    explicit AuthReplayGuard(std::chrono::seconds window = std::chrono::seconds{30})
        : window_s_(window.count()) {}

    class Ticket {
    public:
        void commit() && { guard_->newest_accepted_[std::move(fp_)] = ts_; }

    private:
        friend class AuthReplayGuard;
        Ticket(AuthReplayGuard& guard, Fingerprint fp, uint64_t ts)
            : guard_(&guard), fp_(std::move(fp)), ts_(ts) {}

        AuthReplayGuard* guard_;
        Fingerprint      fp_;
        uint64_t         ts_;
    };
    struct OutsideWindow {};
    struct Replayed { uint64_t last_accepted_ts; };
    using Verdict = std::variant<Ticket, OutsideWindow, Replayed>;

    [[nodiscard]] Verdict check(const Fingerprint& fp, uint64_t ts_wall_s, uint64_t now_wall_s) {
        const int64_t age = age_s(ts_wall_s, now_wall_s);
        if (age > window_s_ || age < -window_s_) return OutsideWindow{};
        std::erase_if(newest_accepted_, [&](const auto& entry) {
            return age_s(entry.second, now_wall_s) > window_s_;
        });
        if (auto seen = newest_accepted_.find(fp); seen != newest_accepted_.end() && ts_wall_s <= seen->second)
            return Replayed{seen->second};
        return Ticket{*this, fp, ts_wall_s};
    }

private:
    static int64_t age_s(uint64_t ts_wall_s, uint64_t now_wall_s) {
        return static_cast<int64_t>(now_wall_s) - static_cast<int64_t>(ts_wall_s);
    }

    int64_t window_s_;
    std::unordered_map<Fingerprint, uint64_t> newest_accepted_;
};

}
