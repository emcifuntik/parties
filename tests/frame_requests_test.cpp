#include <client/frame_requests.h>

#include <chrono>
#include <cstdio>
#include <limits>
#include <thread>

using namespace std::chrono_literals;
using parties::client::FrameRequests;
using parties::client::RenderClock;
using parties::client::idle_wait_ms;
using parties::client::ui_update_deadline;

namespace {

bool Check(bool condition, const char* message) {
    if (!condition)
        std::fprintf(stderr, "Frame requests test failed: %s\n", message);
    return condition;
}

}

int main() {
    bool success = true;

    FrameRequests fresh;
    success &= Check(fresh.take(), "the first frame is requested");
    success &= Check(!fresh.take(), "take cleared the request");
    fresh.request();
    fresh.request();
    success &= Check(fresh.take(), "a burst of requests is one frame");
    success &= Check(!fresh.take(), "a burst of requests is only one frame");

    FrameRequests idle;
    idle.take();
    const auto idle_start = RenderClock::now();
    idle.wait(40);
    success &= Check(RenderClock::now() - idle_start >= 30ms, "an idle wait slept until its timeout");

    FrameRequests woken;
    woken.take();
    const auto woken_start = RenderClock::now();
    std::thread producer([&] {
        std::this_thread::sleep_for(20ms);
        woken.request();
    });
    woken.wait(2000);
    producer.join();
    success &= Check(RenderClock::now() - woken_start < 500ms, "a request woke the waiting loop");
    success &= Check(woken.take(), "the request that woke the loop is still pending");

    FrameRequests early;
    early.take();
    early.request();
    const auto early_start = RenderClock::now();
    early.wait(2000);
    success &= Check(RenderClock::now() - early_start < 500ms, "a request before the wait is not lost");

    const auto now = RenderClock::now();
    success &= Check(!idle_wait_ms(true, now, now + 1s), "a dirty frame renders now");
    success &= Check(!idle_wait_ms(false, now, now), "a reached deadline renders now");
    success &= Check(!idle_wait_ms(false, now, now - 1ms), "a passed deadline renders now");
    success &= Check(idle_wait_ms(false, now, (RenderClock::time_point::max)()) == 1000u,
                     "no deadline still wakes once a second");
    success &= Check(idle_wait_ms(false, now, now + 500ms) == 500u, "waits until the deadline");
    success &= Check(idle_wait_ms(false, now, now + 400us) == 1u,
                     "a sub-millisecond deadline rounds up instead of spinning");

    success &= Check(ui_update_deadline(now, std::numeric_limits<double>::infinity()) ==
                         (RenderClock::time_point::max)(),
                     "an idle context has no deadline");
    success &= Check(ui_update_deadline(now, 0.0) == now, "an animating context is due now");
    success &= Check(ui_update_deadline(now, 0.5) == now + 500ms, "a caret blink is due after its delay");

    return success ? 0 : 1;
}
