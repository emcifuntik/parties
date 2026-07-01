#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace parties::client {

class WebcamCapture {
public:
    WebcamCapture();
    ~WebcamCapture();

    struct DeviceInfo {
        std::wstring symbolic_link;
        std::string  name;
    };

    static std::vector<DeviceInfo> enumerate_devices();

    // Start capturing from device at `device_index` (0 = first device).
    // Requests `req_width` x `req_height` @ `fps`. The actual negotiated size is
    // available via width()/height() after start() returns true.
    // Frames are delivered on an internal worker thread via on_frame.
    bool start(int device_index, uint32_t req_width, uint32_t req_height, uint32_t fps);
    void stop();
    bool running() const;

    uint32_t width() const;
    uint32_t height() const;

    // Delivered on the capture worker thread. `bgra` is 32bpp B,G,R,A, top-down,
    // with `stride` bytes per row (>= width*4). Valid only during the callback.
    std::function<void(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t stride)> on_frame;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace parties::client
