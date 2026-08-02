#include "nvidia/nvdec_sequence_policy.h"
#include "nvidia/nvdec_surface_wait.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <stop_token>

using parties::encdec::nvidia::detail::NvdecSequenceFormat;
using parties::encdec::nvidia::detail::NvdecSequenceState;
using parties::encdec::nvidia::detail::SurfaceWaitResult;
using parties::encdec::nvidia::detail::can_reuse_decoder;
using parties::encdec::nvidia::detail::wait_for_surface_release;

namespace {
void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}
} // namespace

int main() {
    const NvdecSequenceState state{
        true, 3840, 2160, 3840, 2176, 0, 0, 8, 32};
    const NvdecSequenceFormat repeated{
        3840, 2160, 3840, 2176, 0, 0, 8, 30};

    check(can_reuse_decoder(state, repeated),
          "a 30-surface AV1 sequence must reuse a 32-surface decoder");

    auto too_many_surfaces = repeated;
    too_many_surfaces.minimum_decode_surfaces = 33;
    check(!can_reuse_decoder(state, too_many_surfaces),
          "a decoder with too few surfaces must be recreated");

    auto resized = repeated;
    resized.width = 2560;
    check(!can_reuse_decoder(state, resized),
          "a visible resolution change must recreate the decoder");

    auto recropped = repeated;
    recropped.crop_y = 8;
    check(!can_reuse_decoder(state, recropped),
          "a crop change must recreate the decoder");

    auto deeper = repeated;
    deeper.bit_depth = 10;
    check(!can_reuse_decoder(state, deeper),
          "a bit-depth change must recreate the decoder");

    std::condition_variable_any released;
    std::mutex surface_mutex;
    std::stop_source stop_source;
    std::atomic<bool> predicate_entered = false;
    std::promise<void> entered_promise;
    auto entered = entered_promise.get_future();
    auto waiter = std::async(std::launch::async, [&] {
        return wait_for_surface_release(
            released, surface_mutex, stop_source.get_token(), std::chrono::seconds(5), [&] {
                if (!predicate_entered.exchange(true)) entered_promise.set_value();
                return false;
            });
    });
    entered.wait();
    stop_source.request_stop();
    check(waiter.wait_for(std::chrono::milliseconds(250)) == std::future_status::ready,
          "surface wait must wake promptly when decoder teardown is requested");
    check(waiter.get() == SurfaceWaitResult::Cancelled,
          "cancelled surface wait must not be reported as a timeout");

    std::cout << "NVDEC sequence and cancellable surface wait policies passed\n";
    return 0;
}
