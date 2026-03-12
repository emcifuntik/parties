#pragma once

// macOS screen capture using ScreenCaptureKit (macOS 12.3+).
// On macOS 14+ uses SCContentSharingPicker for system-native target selection.
// Delivers frames as CVPixelBufferRef (BGRA, Metal-compatible) on a
// background queue.

#include <cstdint>
#include <functional>
#include <string>

#ifdef __OBJC__
#import <Foundation/Foundation.h>
#import <CoreVideo/CoreVideo.h>
#else
#include <CoreVideo/CVPixelBuffer.h>
#endif

namespace parties::client {

class ScreenCaptureMac {
public:
    ScreenCaptureMac();
    ~ScreenCaptureMac();

    // Show the system SCContentSharingPicker (macOS 14+) and start capturing.
    // Falls back to capturing the main display on macOS 12-13.
    // Calls on_started(true) on success, on_started(false) if user cancels or error.
    void pick_and_start(uint32_t target_fps,
                        std::function<void(bool success)> on_started);
    void stop();

    bool     is_capturing() const { return capturing_; }
    uint32_t width()        const { return width_;  }
    uint32_t height()       const { return height_; }

    // Called on an internal queue for each new frame.
    // pixel_buffer is in kCVPixelFormatType_32BGRA, Metal-compatible.
    // Caller must CFRetain/CFRelease if it needs to hold the buffer.
    std::function<void(CVPixelBufferRef, uint32_t width, uint32_t height)> on_frame;

    // Called on an internal queue with interleaved float32 stereo PCM at 48 kHz.
    // frame_count is the number of stereo sample pairs.
    std::function<void(const float* samples, uint32_t frame_count)> on_audio;

    // Called when the captured window/display is removed.
    std::function<void()> on_closed;

private:
    struct Impl;
    Impl* impl_ = nullptr;

    bool     capturing_ = false;
    uint32_t width_     = 0;
    uint32_t height_    = 0;
};

} // namespace parties::client
