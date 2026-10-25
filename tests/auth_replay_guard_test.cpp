#include <server/auth_replay_guard.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <utility>
#include <variant>

using parties::server::AuthReplayGuard;

namespace {

constexpr uint64_t NOW = 1'700'000'000;

int failures = 0;

void expect(bool ok, const char* what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

bool is_ticket(const AuthReplayGuard::Verdict& v) { return std::holds_alternative<AuthReplayGuard::Ticket>(v); }
bool is_outside(const AuthReplayGuard::Verdict& v) { return std::holds_alternative<AuthReplayGuard::OutsideWindow>(v); }

std::optional<uint64_t> replayed_last(const AuthReplayGuard::Verdict& v) {
    if (auto* r = std::get_if<AuthReplayGuard::Replayed>(&v)) return r->last_accepted_ts;
    return std::nullopt;
}

void accept(AuthReplayGuard& guard, const char* fp, uint64_t ts, uint64_t now) {
    auto verdict = guard.check(fp, ts, now);
    expect(is_ticket(verdict), "setup auth is accepted");
    if (auto* ticket = std::get_if<AuthReplayGuard::Ticket>(&verdict)) std::move(*ticket).commit();
}

}

int main() {
    {
        AuthReplayGuard guard;
        expect(is_ticket(guard.check("a", NOW, NOW)), "fresh timestamp is accepted");
        expect(is_ticket(guard.check("a", NOW - 30, NOW)), "timestamp 30 s old is inside the window");
        expect(is_ticket(guard.check("a", NOW + 30, NOW)), "timestamp 30 s ahead is inside the window");
        expect(is_outside(guard.check("a", NOW - 31, NOW)), "timestamp 31 s old is rejected");
        expect(is_outside(guard.check("a", NOW + 31, NOW)), "timestamp 31 s ahead is rejected");
    }
    {
        AuthReplayGuard guard;
        auto first = guard.check("victim", NOW, NOW);
        expect(is_ticket(first), "forged attempt passes the pre-verify check");
        expect(is_ticket(guard.check("victim", NOW, NOW)),
               "an uncommitted check does not lock the identity out");
    }
    {
        AuthReplayGuard guard;
        accept(guard, "a", NOW, NOW);
        expect(replayed_last(guard.check("a", NOW, NOW)) == NOW, "same timestamp is a replay");
        expect(replayed_last(guard.check("a", NOW - 5, NOW)) == NOW, "older timestamp is a replay");
        expect(is_ticket(guard.check("a", NOW + 1, NOW + 1)), "newer timestamp is accepted");
        expect(is_ticket(guard.check("b", NOW, NOW)), "replay state is per fingerprint");
    }
    {
        AuthReplayGuard guard;
        accept(guard, "a", NOW, NOW);
        accept(guard, "a", NOW + 10, NOW + 10);
        expect(replayed_last(guard.check("a", NOW + 5, NOW + 10)) == NOW + 10,
               "the newest committed timestamp is the replay bound");
    }
    {
        AuthReplayGuard guard{std::chrono::seconds{5}};
        expect(is_outside(guard.check("a", NOW - 6, NOW)), "window length is configurable");
    }

    if (failures) return 1;
    std::cout << "auth_replay_guard: all checks passed\n";
    return 0;
}
