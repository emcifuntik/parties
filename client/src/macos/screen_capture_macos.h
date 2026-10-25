#pragma once

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

    void pick_and_start(uint32_t target_fps, float output_scale, bool audio_only,
                        std::function<void(bool success)> on_started);
    void stop();

    bool     is_capturing() const { return capturing_; }
    uint32_t width()        const { return width_;  }
    uint32_t height()       const { return height_; }

    // Called on an internal queue for each new frame.
    // pixel_buffer is in kCVPixelFormatType_32BGRA, Metal-compatible.
    // Caller must CFRetain/CFRelease if it needs to hold the buffer.
    std::function<void(CVPixelBufferRef, uint32_t width, uint32_t height)> on_frame;

    std::function<void(const float* samples, uint32_t frame_count)> on_audio;

    std::function<void()> on_closed;

private:
    struct Impl;
    Impl* impl_ = nullptr;

    bool     capturing_ = false;
    uint32_t width_     = 0;
    uint32_t height_    = 0;
};

}
